# Phase 5 — Typed Variable Inference

**Commit:** `b6687ec`
**Goal:** Eliminate JSValue boxing for numeric loop variables.

---

## The problem

After Phase 4, GCC receives code like:

```c
/* sum_loop: s += i  (every iteration) */
JSValue _a = _l[0];                          /* load JSValue s */
int64_t _ia = JS_VALUE_GET_INT(_a);          /* unbox */
JSValue _b = _l[1];                          /* load JSValue i */
int64_t _ib = JS_VALUE_GET_INT(_b);          /* unbox */
int64_t _r = _ia + _ib;
_l[0] = JS_NewInt32(ctx, (int32_t)_r);       /* re-box → heap struct */
```

GCC cannot hoist the `JS_VALUE_GET_INT` calls out of the loop: `_l[0]` is a `JSValue`
(an opaque struct), and GCC has no proof that its tag stays `JS_TAG_INT` across
iterations.  Every iteration pays the full unbox/rebox cost.

Phase 5 gives the compiler proof: if a local variable is always numeric, represent it
as a bare `double` in the generated C, and let GCC keep it in an XMM register.

---

## Type system

Two types per local slot:

```c
#define JIT_T_JSVAL   0   /* general boxed JSValue — default */
#define JIT_T_NUMBER  1   /* always numeric: can use double _ld[i] */
```

A slot is `JIT_T_NUMBER` iff every write to it produces a numeric value and it is
never read before its first write (no TDZ hazard).

---

## Type inference algorithm (jit_infer_types)

Forward abstract interpretation over the bytecode, 3-pass fixpoint:

```
local_type[i] = JIT_T_NUMBER for all i   (optimistic start)

repeat up to 3 times:
    type_stack[] = empty
    for each instruction in bytecode:
        switch opcode:
        case push_i32, push_0..7, push_minus1:
            push NUMBER
        case push_null, push_undefined, push_false, push_true, push_atom_value:
            push JSVAL
        case add, sub, mul, div, mod:
            b = pop; a = pop
            push (a==NUMBER && b==NUMBER) ? NUMBER : JSVAL
        case pow:
            pop 2; push JSVAL   (BigInt possible)
        case shl, sar, shr, and, or, xor, not:
            pop N; push NUMBER  (always produce int32)
        case lt, lte, gt, gte, eq, neq, strict_eq, strict_neq:
            pop 2; push JSVAL   (produce bool)
        case get_length:
            pop 1; push NUMBER  (array/string length is always non-negative int32)
        case get_loc N:
            push local_type[N]
        case put_loc N:
            t = pop
            if t != NUMBER: local_type[N] = JSVAL   (downgrade)
        case get_arg, get_var_ref, get_var, get_global, get_field, ...:
            push JSVAL
        case inc_loc N, dec_loc N:
            no-op (in-place, type preserved)
        case if_false, if_true, goto:
            pop 0–1; no type change on locals
until no downgrade occurred in this pass
```

The result is `local_type[var_count]` — a byte array passed to `gen_body()`.

**Why 3 passes?**  A variable written by two different code paths converges within 3
iterations of the fixpoint.  In practice, almost all functions converge in pass 1 or 2.

**The `get_length → NUMBER` rule** (added in Phase 6.1) propagates numeric type
downstream: a local assigned from `arr.length` is inferred as NUMBER, enabling the
inner loop counter to avoid boxing.

---

## Generated C changes

With `local_type[i] == JIT_T_NUMBER`, the preamble adds a parallel array of doubles:

```c
JSValue _l[var_count];      /* still needed for GC and non-NUMBER locals */
double  _ld[var_count];     /* typed locals — kept in XMM regs by GCC */
```

Locals with `JIT_T_NUMBER` are managed exclusively through `_ld[i]`.
`_l[i]` is set to `JS_UNDEFINED` and never touched for those slots (so the GC
epilogue at `_ex:` safely frees them as undefined).

### Opcode changes

**get_loc N (NUMBER local):**

```c
/* Before Phase 5: */
_s[_sp++] = _DUP(_l[N]);

/* After Phase 5 (NUMBER): */
_s[_sp++] = JS_NewFloat64(ctx, _ld[N]);
/* GCC CSE: if _ld[N] was just written, this is just a tag + value store */
```

**put_loc N (NUMBER local, value is NUMBER):**

```c
/* Before: */
{ JSValue _v = _s[--_sp]; _FREE(_l[N]); _l[N] = _v; }

/* After (NUMBER ← NUMBER): */
{ JSValue _v = _s[--_sp];
  _ld[N] = JS_VALUE_GET_TAG(_v) == JS_TAG_INT
           ? (double)JS_VALUE_GET_INT(_v)
           : JS_VALUE_GET_FLOAT64(_v);
  _FREE(_v);
  /* _l[N] stays JS_UNDEFINED — never modified */ }
```

**inc_loc N (NUMBER local):**

```c
/* Before Phase 5 (JSValue path): */
{ JSValue _v = _l[N];
  if (IS_INT(_v) && INT(_v) != INT32_MAX) {
      _l[N] = JS_NewInt32(ctx, INT(_v) + 1);
  } else {
      JSValue _r = _RT->add(ctx, _DUP(_v), JS_NewInt32(ctx, 1));
      _CHK(_r); _FREE(_l[N]); _l[N] = _r;
  } }

/* After Phase 5 (double path): */
_ld[N] += 1.0;
/* GCC: single ADDSD instruction, no tag check, no heap allocation */
```

**add_loc N (NUMBER local, Phase 5 extension):**

```c
/* Equivalent of: local[N] += stack_top */
{ double _dv = JS_VALUE_GET_TAG(_s[_sp-1]) == JS_TAG_INT
               ? (double)JS_VALUE_GET_INT(_s[_sp-1])
               : JS_VALUE_GET_FLOAT64(_s[_sp-1]);
  _FREE(_s[--_sp]);
  _ld[N] += _dv;
}
/* GCC: CVTSI2SD or ADDSD, no boxing */
```

---

## What GCC does with this

For `sum_sq` (computing `sum += i*i` in a loop with `i` and `sum` as NUMBER locals):

```c
/* Generated C (simplified): */
while (1) {
    double _da = _ld[1];              /* i */
    double _db = _ld[1];              /* i (same) */
    double _prod = _da * _db;         /* i*i */
    _ld[0] += _prod;                  /* sum += i*i */
    _ld[1] += 1.0;                    /* i++ */
    /* comparison+branch: Phase 6.1 adds this; for now pretend it's inline */
    if (_ld[1] >= 1000000.0) break;
}
```

GCC -O2 with CSE:

```asm
; Both reads of _ld[1] CSE'd into one register load
; MULSD xmm0, xmm0    ; i*i
; ADDSD xmm1, xmm0    ; sum += i*i
; ADDSD xmm0, [1.0]   ; i++
; UCOMISD xmm0, [limit]
; JB loop
```

The `JSValue` boxing is entirely gone.  No `JS_NewFloat64`, no `JS_FreeValue`, no heap
traffic — pure floating-point loop.

---

## Also fixed in Phase 5: JS_ATOM_length atom enum

The `JSAtomEnumJIT` enum in `quickjs-jit.c` was missing a leading `__JIT_ATOM_NULL = 0`
entry.  Every predefined atom value in generated C code was therefore one too low:
`JS_ATOM_length` baked in as 49 instead of the correct 50, causing `OP_get_length`
to look up the wrong property (`callee` instead of `length`).

**Symptom:** `arr_sum` appeared to give ~3290× speedup — the loop condition `i < undefined`
was always false, so the loop body never executed and the function returned 0 in 0 ms.
DeltaBlue also ran incorrect code silently.

**Fix:** Add `__JIT_ATOM_NULL = 0` as the first enum entry so all subsequent values
align with the runtime atom table.

---

## Performance results

Measurements from `bench_gcc.js` (threshold=2, 8-second GCC warm-up):

```
Benchmark           Interp min    JIT P5 min   Speedup
──────────────────────────────────────────────────────
fib(30) ×1          89.83 ms      107.92 ms    0.83×   vtable recursion overhead
sum_loop(1e6) ×20  659.80 ms      888.86 ms    0.74×   tight int loop, boxing > gain
sum_sq(1e6) ×20    522.61 ms      291.10 ms    1.79×   ★ GCC CSE + XMM regs
count_primes ×10     6.43 ms        2.20 ms    2.92×   ★ inc_loc → single ADDSD
arr_sum ×1000      293.65 ms      345.06 ms    0.85×   get_array_el still vtable
```

### Why fib and sum_loop regress

**fib**: recursive calls go through `_RT->call` vtable → `JS_CallInternal` →
dup all arguments into a local buffer → free on return.  Three heap refcount operations
per recursive call.  The interpreter uses direct C recursion with no overhead.

**sum_loop** (`s += i` with `i++`): Phase 5 type inference does not fire on this loop
because `s` accumulates both the increment `i` and the initial `0`, and the intermediate
`JSValue` boxing in the vtable add path prevents GCC from proving the type is constant.
(Phase 6.1 gen-time type stack partially addresses this.)

### Atom fix impact on arr_sum

After the atom fix, `arr_sum` correctly computes the sum (verified) but is now *slower*
than the interpreter (0.85×).  The JIT correctly accesses `.length` each iteration via
`_RT->get_prop` (a full `JS_GetProperty` call), while the interpreter has an inline
`fast_array` path for array length.  Phase 6.2 IC partially recovers this.
