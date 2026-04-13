# Phase 44 — Mixed-Type Arithmetic Fast Path

## Background

### What P38.2 already does

P38.2 added a typed-index fast path for `OP_get_array_el` / `OP_put_array_el`:
when `gen_st[d-1] == JIT_T_INT` (index is a native `int64_t _ti{N}`), the
emitter skips `JS_NewInt32` boxing and uses `_ti{d-1}` directly as `uint32_t
_ai` in the dense-array guard.  That part is fully done.

### The remaining gap

`get_array_el` always returns `JIT_T_JSVAL` (line 886 in `jit_infer_types`,
line 7440 in `gen_st` tracking).  When the result is immediately consumed by
`OP_add/sub/mul/div`, the accumulator is typically `JIT_T_NUMBER` or
`JIT_T_INT` (e.g. `s` in `arr_sum`), but the array element is `JIT_T_JSVAL`.

The `_bn` guard in `gen_body` (line 3979 etc.) fires only when **both** operands
are NUMBER/INT.  When one is JSVAL, the code falls into the else branch:

```c
_P94_ENSURE(d-2); _P94_ENSURE(d-1);   // boxes the NUMBER operand
// emitted C:
int _ta=JS_VALUE_GET_TAG(_a), _tb=JS_VALUE_GET_TAG(_b);
if (_ta==JS_TAG_INT && _tb==JS_TAG_INT) { ... }
else if (numeric tags) { double arithmetic }
else { _RT->add(ctx, _a, _b) }
```

`_P94_ENSURE` on a NUMBER slot emits `_tsv{N} = JS_NewFloat64(ctx, _tsd{N})`.
On 64-bit that is a cheap struct store (no heap alloc), but it still costs
~2 instructions plus two `JS_VALUE_GET_TAG` calls per iteration — on top of the
correct double arithmetic that follows.

For `arr_sum(10k) × 1k` = 10M iterations, this is ~30–50M extra instructions
≈ ~10–20 ms on this hardware, consistent with the 50 ms JIT vs 9 ms Node gap.

### Fix

When **one** operand is `JIT_T_NUMBER` or `JIT_T_INT` at gen-time and the other
is `JIT_T_JSVAL`, emit a specialized "half-typed" path that:

1. Uses the typed operand's raw `_tsd{N}` or `_ti{N}` directly — no boxing.
2. Does a single `JS_VALUE_GET_TAG` check on the JSVAL operand.
3. If JSVAL is INT or FLOAT64, does the arithmetic natively.
4. Otherwise falls through to the existing vtable path.

This eliminates the `_P94_ENSURE` box + one tag check per iteration for the
dominant array-arithmetic pattern.

---

## Implementation Status

All sub-phases complete. `JIT_CODEGEN_VERSION` bumped to `4u`.

---

## Sub-phases

### P44.1 — Audit: confirm OP_add bottleneck in arr_sum ✓ DONE

**Goal:** Verify the box+tag-check overhead is the measurable gap before writing
any code.

```sh
# Build with default optimization
make CONFIG_JIT=y -j$(nproc) qjs

# Baseline: arr_sum with JIT
rm -rf ~/.cache/qjs-jit
./qjs --jit-threshold-gcc=1 jit_perf_tests/bench_runner.js 2>/dev/null   # compile
./qjs --jit-threshold-gcc=1 jit_perf_tests/bench_runner.js 2>/dev/null   # measure (warm)
```

Record `arr_sum(10000) x1e3` time.  Expected: ~40–50 ms.

**Verify the typed path is NOT firing for arr_sum** by inspecting generated C:

```sh
# Find the arr_sum .so source
ls ~/.cache/qjs-jit/*.c | xargs grep -l "arr_sum" 2>/dev/null | head -3
# Look for _P94_ENSURE or JS_NewFloat64 adjacent to _tsd
grep -A10 "_P94_ENSURE\|JS_NewFloat64" ~/.cache/qjs-jit/<arr_sum_hash>.c | head -40
```

Expected: see `_tsv{N}=JS_NewFloat64(ctx,_tsd{N})` before the tag-check block,
confirming the NUMBER operand is being boxed on every iteration.

---

### P44.2 — Extend OP_add for half-typed operands ✓ DONE

**File:** `quickjs-jit.c`, `case OP_add:` (~line 3978)

**Change:** Extend `_bn` to cover 4 new combinations, or add a separate
`_bh` ("half-typed") branch between the fully-typed `_bn` path and the
existing JSVAL-only else.

Add after the existing `_bn` block, before `} else {`:

```c
/* P44: half-typed fast path — one operand NUMBER/INT, one JSVAL.
 * Avoids boxing the typed operand; does single tag-check on JSVAL side.
 * Cases: (NUMBER, JSVAL), (INT, JSVAL), (JSVAL, NUMBER), (JSVAL, INT). */
} else if (gen_sp > d-1) {
    uint8_t _t2 = _GS_TOP2(), _t1 = _GS_TOP();
    int _t2n = (_t2 == JIT_T_NUMBER || _t2 == JIT_T_INT);
    int _t1n = (_t1 == JIT_T_NUMBER || _t1 == JIT_T_INT);
    if (_t2n && !_t1n) {
        /* left = NUMBER/INT typed, right = JSVAL */
        _P94_ENSURE(d-1); /* box right (already JSVAL, no-op) */
        const char *_la = (_t2==JIT_T_INT) ? "(double)_ti" : "_tsd";
        jit_buf_printf(cb,
            "    { JSValue _b=_tsv%d; int _tb=JS_VALUE_GET_TAG(_b);\n"
            "      if(js_likely(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
            "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b)"
                                              ":JS_VALUE_GET_FLOAT64(_b);\n"
            "        _tsd%d=%s%d+_db; _sp=%d;\n"
            "      } else {\n"
            "        JSValue _a=%s; _sp=%d;\n"
            "        JSValue _r=_RT->add(ctx,_a,_b); _CHK(_r);\n"
            "        _tsv%d=_r; _sp=%d;\n"
            "      } }\n",
            d-1,
            d-2, _la, (_t2==JIT_T_INT ? d-2 : d-2), d-1,
            /* slow-path box for _a */ ..., d-2,
            d-2, d-1);
    } else if (!_t2n && _t1n) {
        /* left = JSVAL, right = NUMBER/INT typed */
        _P94_ENSURE(d-2);
        /* symmetric to above */
        ...
    } else {
        /* both JSVAL — fall through to existing else */
        _P94_ENSURE(d-2); _P94_ENSURE(d-1);
        ...
    }
```

> **Note on slow-path boxing:** The `_RT->add` slow path needs JSValues for
> both operands.  When the typed operand is `_tsd{N}`, box it inline with
> `JS_NewFloat64(ctx, _tsd{N})`.  This allocation only happens for strings,
> BigInt, objects — not for the common numeric path.

**Result type in gen_st:** The result of the half-typed add is NUMBER (double),
so set `_gs_push = JIT_T_NUMBER` for this path.

**Testable check:** After the change, the generated `.c` for `arr_sum` should
show `_tsd{N} = _tsd{N} + _db;` with a single tag check on the array element,
and no `JS_NewFloat64` in the hot path.

```sh
rm -rf ~/.cache/qjs-jit
./qjs --jit-threshold-gcc=1 jit_perf_tests/bench_runner.js 2>/dev/null
grep -A8 "_tb==JS_TAG_INT" ~/.cache/qjs-jit/<arr_sum_hash>.c | head -20
```

---

### P44.3 — Extend OP_sub, OP_mul, OP_div the same way ✓ DONE

`OP_sub` (~line 4016), `OP_mul` (~line 4070), `OP_div` (~line 4110) have the
same `_bn` guard structure.  Apply the same half-typed extension to each.

`OP_div` can only produce NUMBER (never INT); the result gen_st for all four ops
in the half-typed case is `JIT_T_NUMBER`.

**Priority:** `OP_add` is the most common (accumulators, string-concat fallback).
`OP_mul` matters for Crypto (polynomial multiply).  `OP_sub` / `OP_div` are
lower priority but trivial to add while the pattern is fresh.

---

### P44.4 — Extend comparisons OP_lt / OP_lte / OP_gt / OP_gte ✓ DONE

Comparison fast paths (`_bn` guard in the `<`/`<=`/`>`/`>=` emitters) have the
same half-typed gap.  When comparing a typed loop counter against a JSVAL
(e.g. `i < arr.length` where `length` is `JIT_T_INT` but could theoretically
be JSVAL in edge cases), or a typed accumulator against a JSVAL threshold:

```c
/* P44: half-typed comparison: typed INT vs JSVAL */
if (_t2==JIT_T_INT && _t1==JIT_T_JSVAL) {
    _P94_ENSURE(d-1);  /* ensure JSVAL slot is filled */
    jit_buf_printf(cb,
        "    { JSValue _b=_tsv%d; int _tb=JS_VALUE_GET_TAG(_b);\n"
        "      int _r;\n"
        "      if(js_likely(_tb==JS_TAG_INT)) _r=(_ti%d < JS_VALUE_GET_INT(_b));\n"
        "      else if(_tb==JS_TAG_FLOAT64) _r=((double)_ti%d < JS_VALUE_GET_FLOAT64(_b));\n"
        "      else { JSValue _a=JS_NewInt32(ctx,(int32_t)_ti%d);\n"  /* rare */
        "             _r=(_RT->lt(ctx,_a,_b)>0); }\n"
        "      _tsv%d=JS_NewBool(ctx,_r); _sp=%d; }\n",
        d-1, d-2, d-2, d-2, d-2, d-1);
```

**Note:** Comparisons always produce JSVAL (bool), so `gen_st` output stays
`JIT_T_JSVAL` here.

---

### P44.5 — Performance measurement ✓ DONE

```sh
make CONFIG_JIT=y -j$(nproc) qjs

# Clear cache (new codegen)
rm -rf ~/.cache/qjs-jit

# Pass 1: compile
cd jit_perf_tests && ../qjs --jit-threshold-gcc=1 bench_runner.js 2>/dev/null

# Pass 2: measure
../qjs --jit-threshold-gcc=1 bench_runner.js 2>/dev/null
```

**Expected primary improvements:**

| Benchmark | Before P44 | Expected After |
|---|---|---|
| arr_sum(10k) x1e3 | ~50 ms | ~35–40 ms |
| EarleyBoyer (V8 suite) | 1131 | +10–20% expected |

Secondary: any benchmark with `property_value + local` or `array_element * local`
patterns (Crypto, DeltaBlue).

**V8 benchmark suite:**

```sh
cd jit_perf_tests/v8bench
rm -rf ~/.cache/qjs-jit
../../qjs run_qjs.js 2>/dev/null   # pass 1: compile
../../qjs run_qjs.js 2>/dev/null   # pass 2: measure
```

Record results in `jit_perf_tests/RESULTS_P44.md`.

---

### P44.6 — Correctness tests ✓ DONE

**File:** `jit-tests/js/test_jit_p44_mixed_arith.js`

Tests to write:

```javascript
// 1. arr_sum — typed accumulator + JSVAL element
function arr_sum_int(arr) {
    var s = 0;
    for (var i = 0; i < arr.length; i++) s += arr[i];
    return s;
}
var r1 = arr_sum_int([1,2,3,4,5]);
if (r1 !== 15) throw new Error("arr_sum_int: " + r1);

// 2. arr_sum float — same with float elements
function arr_sum_float(arr) {
    var s = 0.0;
    for (var i = 0; i < arr.length; i++) s += arr[i];
    return s;
}
var r2 = arr_sum_float([1.5, 2.5, 3.0]);
if (Math.abs(r2 - 7.0) > 1e-10) throw new Error("arr_sum_float: " + r2);

// 3. sub: typed accumulator minus JSVAL
function arr_sub(arr) {
    var s = 100;
    for (var i = 0; i < arr.length; i++) s -= arr[i];
    return s;
}
if (arr_sub([10, 20, 5]) !== 65) throw new Error("arr_sub");

// 4. mul: typed × JSVAL
function arr_product(arr) {
    var p = 1;
    for (var i = 0; i < arr.length; i++) p *= arr[i];
    return p;
}
if (arr_product([2, 3, 4]) !== 24) throw new Error("arr_product");

// 5. Overflow / edge: JSVAL element is string → vtable path must fire
function arr_concat(arr) {
    var s = "";
    for (var i = 0; i < arr.length; i++) s += arr[i];
    return s;
}
if (arr_concat(["a","b","c"]) !== "abc") throw new Error("arr_concat");

// 6. comparison: typed INT vs JSVAL array length
function count_up_to(arr) {
    var n = 0;
    for (var i = 0; i < arr.length; i++) n++;
    return n;
}
if (count_up_to([1,2,3,4,5]) !== 5) throw new Error("count_up_to");
```

Add to `jit-tests/js/Makefile` JS_TESTS list.

**Run:**
```sh
make -C jit-tests/js run THRESHOLD=1
```

---

### P44.7 — Documentation ✓ DONE

- `jit-docs/phase44-steps.md` (this file): all sub-phases marked complete.
- `jit_perf_tests/RESULTS_P44.md` created with before/after tables.
- `jit-docs/performance-benchmarks.md` section 4.9 updated.

**Key implementation notes (deviations from plan):**

1. **P44.3 result type is JSVAL, not NUMBER.** The plan said HALF_L sub/mul
   result should be `JIT_T_NUMBER`. The initial implementation followed this
   but caused a ~25% regression on `fib(30)` because `n - 1` was stored as
   `double (_tsd)` and then re-boxed for the recursive call. Fixed: HALF_L
   sub/mul paths now produce `_tsv` (JSValue) with an INT integer fast path.
   `gen_st` and `jit_infer_types` now return JSVAL for half-typed sub/mul
   (same as OP_add). `OP_div` still always returns NUMBER since division
   always produces a floating-point result.

2. **P44.4 comparison macros use INT fast path.** The plan sketched a
   `(double)_ti op _db` double comparison. Implementation adds an INT branch
   first (`if(js_likely(_tb==JS_TAG_INT)) _cond = (int32_t)_ti op GET_INT(_b)`)
   avoiding INT→double conversion on integer inputs.

3. **`JIT_CODEGEN_VERSION` bumped to 4u** (was 3u after initial P44, 4u after
   the INT fast path fix).

---

## What is NOT in scope for P44

- `OP_get_array_el` result type propagation (element type IC) — that is P45
  territory.
- Typed-index fast path for `get_array_el` — already done in P38.2.
- `OP_add` with string operands — no change; vtable path is correct.
- `OP_mul` overflow: int×int path already handles overflow by promoting to
  double; the half-typed NUMBER×JSVAL path always produces NUMBER (double) so
  overflow is automatic.

---

## Complexity Assessment

**Low-medium.** All changes are within `gen_body` in `quickjs-jit.c`.  No new
data structures.  No IC changes.  No cache invalidation.  The half-typed branch
sits entirely within the existing `case OP_add:` block.

Risk areas:
- Slow-path boxing of the typed operand for the vtable fallback — must be
  correct (use `JS_NewInt32` for INT, `JS_NewFloat64` for NUMBER).
- `_sp` must be set to `d-2` before any vtable call for exception safety.
- Result slot `gen_st` update: must set NUMBER (not INT) since the JSVAL
  operand could be FLOAT64.
