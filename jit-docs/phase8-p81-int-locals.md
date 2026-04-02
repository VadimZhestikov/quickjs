# P8.1 — `JIT_T_INT` Integer Locals

**Branch:** `jit`  
**Status:** Complete

Adds a third inferred local type `JIT_T_INT` that uses `int64_t _li[]` storage for locals
that are provably always integral.  Eliminates the floating-point intermediate that
`JIT_T_NUMBER` (`double _ld[]`) required for integer-only loops.

---

## Problem

Phase 5 introduced `JIT_T_NUMBER` → `double _ld[]` for typed locals.  Loop counters such
as `i` in `for (var i=0; i<n; i++) s += i;` were inferred as NUMBER and stored as
`double`.  This enabled the `_ld[idx]+=1.0` fast path for `inc_loc` and
`_ld[idx]+=(double)val` fast path for `add_loc`, eliminating one boxing/unboxing round
trip per iteration.

However, `double` has two disadvantages over a pure integer type:

1. Every `get_loc` boxing involves a floating-point comparison (`_d == (int32_t)_d`)
   to decide between `JS_NewInt32` and `JS_NewFloat64`.
2. Arithmetic involving NUMBER locals requires the FPU pipeline even for integer-only
   computations (`j * j` for counting primes).

---

## Implementation

### Type constant

```c
#define JIT_T_JSVAL  0  /* unknown — always use JSValue    */
#define JIT_T_NUMBER 1  /* provably numeric — double _ld[] */
#define JIT_T_INT    2  /* provably integral — int64_t _li[] */
```

INT is the most specific type.  Type inference starts optimistically at INT and
downgrades via `_TI_WRITE` whenever a less-specific value is stored to the local.

### Type inference (`jit_infer_types`)

- **Initial state:** `memset(lt, JIT_T_INT, var_count)` — start optimistic.
- **Integer literal pushes** (`push_0`..`push_7`, `push_minus1`, `push_i8`, `push_i16`,
  `push_i32`) → type `JIT_T_INT`.
- **Binary arithmetic propagation:** INT×INT→INT, INT×NUMBER or NUMBER×NUMBER→NUMBER,
  anything with JSVAL→JSVAL.
- **`inc_loc`/`dec_loc`:** local stays unchanged (in-place update preserves type).
- **`add_loc`:** local downgraded to the type of the stack top being added in.
- **Unary ops:** preserve type if operand was numeric.

### Preamble changes (`gen_preamble`)

```c
if (nhave_int > 0) {
    jit_buf_printf(cb, "    int64_t _li[%d];\n", var_count);
    for (int j = 0; j < var_count; j++)
        if (local_type[j] == JIT_T_INT)
            jit_buf_printf(cb, "    _li[%d]=0;\n", j);
}
```

`_l[idx]` is still declared and initialised to `JS_UNDEFINED` for both NUMBER and INT
locals — the `_FREE` loop in the function footer is unchanged (freeing UNDEFINED is a
no-op).

### Local access macros

**`GEN_GET_LOC(idx)`** — box int64 to JSValue:
```c
{ int64_t _v=_li[idx];
  _s[_sp++]=((int32_t)_v==_v)?JS_NewInt32(ctx,(int32_t)_v)
                              :JS_NewFloat64(ctx,(double)_v); }
```

Range check `(int32_t)_v==_v` is always true for typical loop counters (0..N, N<2^31)
and is purely an integer comparison (sign-extend + compare) — cheaper than the NUMBER
path's FPU comparison.

**`GEN_PUT_LOC(idx)`** / **`GEN_SET_LOC(idx)`** — unbox JSValue to int64:
```c
{ JSValue _t=_s[--_sp];
  _li[idx]=(JS_VALUE_GET_TAG(_t)==JS_TAG_INT)
           ?(int64_t)JS_VALUE_GET_INT(_t)
           :(int64_t)JS_VALUE_GET_FLOAT64(_t); }
```

### `OP_inc_loc` / `OP_dec_loc` fast paths

```c
case OP_inc_loc: {
    int idx = bc[pc + 1];
    if (local_type && idx < var_count && local_type[idx] == JIT_T_INT)
        jit_buf_printf(cb, "    _li[%d]++;\n", idx);   /* branch-free int64 */
    else if (...NUMBER...) jit_buf_printf(cb, "    _ld[%d]+=1.0;\n", idx);
    else { /* JSVAL: tag-check + overflow guard */ }
}
```

`_li[idx]++` is a single x86 `add` instruction — no boxing, no refcount check.

### `OP_add_loc` INT fast path

```c
if (local_type && idx < var_count && local_type[idx] == JIT_T_INT) {
    jit_buf_printf(cb,
        "    { JSValue _b=_s[--_sp];\n"
        "      _li[%d]+=(JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n"
        "              ?(int64_t)JS_VALUE_GET_INT(_b)\n"
        "              :(int64_t)JS_VALUE_GET_FLOAT64(_b); }\n",
        idx);
}
```

One tag check, one integer addition, no boxing on the store.

### Comparison fusion update

All comparison opcodes (`lt`, `lte`, `gt`, `gte`, `eq`, `neq`, `strict_eq`,
`strict_neq`) use:

```c
int _bn = (_GS_TOP2() >= JIT_T_NUMBER && _GS_TOP() >= JIT_T_NUMBER);
```

(Changed from `== JIT_T_NUMBER`.)  This allows `JIT_T_INT`-typed stack operands to
trigger `GEN_CMP_FUSE_NUM`, emitting direct double arithmetic instead of a vtable call.

### gen_st tracking

Integer constant pushes now inject `JIT_T_INT` into the gen-time type stack.  Arithmetic
propagation in gen_st mirrors `jit_infer_types`: INT×INT→INT, anything else→NUMBER or
JSVAL.

---

## Why int64_t and not int32_t

Loop counters like `sum += i` (1e6 iterations) accumulate to ~5×10^11, which overflows
int32.  `int64_t` covers all practical accumulator values without requiring a fallback.
The `get_loc` boxing still trims to `int32` for typical values (range check
`(int32_t)_v==_v`) to produce `JS_TAG_INT` rather than `JS_TAG_FLOAT64`.

---

## Key limitation: `let` vs `var`

QuickJS's bytecode compiler emits TDZ-guarded `get_loc_check` / `put_loc_check` for
`let`/`const` variables but plain `get_loc` / `put_loc` for `var`.  The peephole
optimisation that converts `get_loc+post_inc+put_loc+drop → inc_loc` only fires on the
unguarded forms.  Consequently:

- `var` loops: `inc_loc`/`add_loc` fire → pure `_li[idx]++` / `_li[idx]+=...`
- `let` loops: `post_inc+put_loc+drop` pattern, no `inc_loc` emitted

The bench_aot.js benchmarks use `let`, so `inc_loc`'s direct `_li[idx]++` benefit is
not exercised there.  The speedup in sum_sq and count_primes comes primarily from the
improved comparison fusion (INT type in gen_st triggers `GEN_CMP_FUSE_NUM` more often).

---

## Performance results

Measurements: Linux 6.6.87.2 WSL2 x86-64, GCC -O2, `--jit-aot` warm cache.  
Same binary used for both JIT AOT and interpreter baseline (3 runs, min shown).

| Benchmark | Interp min | JIT P8.1 min | Speedup | Phase 7 speedup |
|---|---:|---:|---:|---:|
| fib(30) ×1 | 142 ms | 129 ms | **0.92×** | 0.79× |
| sum_loop(1e6) ×20 | 787 ms | 910 ms | **0.87×** | 0.90× |
| sum_sq(1e6) ×20 | 639 ms | 282 ms | **2.27×** | 1.95× |
| count_primes(3000) ×10 | 7.66 ms | 3.20 ms | **2.39×** | 2.38× |
| arr_sum(10000) ×1000 | 348 ms | 357 ms | **0.97×** | 0.97× |

V8 benchmark (best-of-3 JIT; pure interpreter binary):

| | Score |
|---|---:|
| JIT P8.1 best | 874 |
| Pure interpreter | 776 |
| Ratio | **1.13×** |

**Key wins:**
- `sum_sq`: +16% over Phase 7 — INT type enables better gen_st propagation
  through `i*i` multiplication, triggering `GEN_CMP_FUSE_NUM` for the loop condition.
- `fib`: small improvement from 0.79× to 0.92× — noise, but no regression.
- V8 score: 0.90× → 1.13× vs interpreter — P8.1 crosses the "faster than interpreter"
  threshold on the real-world v8 suite.

---

## Files changed

| File | Change |
|---|---|
| `quickjs-jit.c` | `JIT_T_INT=2`, `jit_infer_types` INT propagation, `gen_preamble` `_li[]`, `GEN_GET_LOC/PUT_LOC/SET_LOC` INT paths, `OP_inc_loc/dec_loc/add_loc` INT paths, comparison `_bn >= JIT_T_NUMBER`, gen_st INT tracking |
