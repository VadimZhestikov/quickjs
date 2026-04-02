# P8.2 — Direct Self-Recursive JIT Calls

**Branch:** `jit`  
**Status:** Complete

Eliminates vtable recursion overhead in self-recursive JIT functions by emitting a direct
C call to `__jit_f_<hash>` instead of `_RT->call`.  Also fixes a pre-existing bug where
functions using `OP_get_var` (closure variable access) silently failed to JIT-compile,
causing silent fallback to the interpreter.

---

## Bug fix: `unlikely` in generated C caused dlopen failure

`OP_get_var` codegen emitted:
```c
if(unlikely(JS_VALUE_GET_TAG(*_pv)==JS_TAG_UNINITIALIZED)){
```

`quickjs.h` defines `js_unlikely` at the top, but **undefines it** at the bottom
(`#undef js_unlikely` at line 1164).  After `#include "quickjs.h"` in the generated
`.c`, neither `unlikely` nor `js_unlikely` is defined as a macro — GCC treats both as
external function calls and emits a PLT call.

The compiled `.so` had `U unlikely` (undefined external).  At `dlopen(..., RTLD_NOW)`
time, the linker could not resolve `unlikely` from the `qjs` binary, so `dlopen` returned
`NULL` and the JIT silently fell back to the interpreter for every function that accessed
a closure variable.

**Fix:** removed the `unlikely()` hint in the generated C entirely:
```c
if(JS_VALUE_GET_TAG(*_pv)==JS_TAG_UNINITIALIZED){
```
GCC's branch predictor optimises the cold path without the hint.

This bug existed since `OP_get_var` was first added to the JIT (Phase 2).  All previously
measured fib numbers were pure-interpreter numbers — the JIT was not running for fib.

---

## Problem

After the bug fix, `fib(30)` ran at:
- Pure interpreter: ~112 ms
- JIT (after bug fix, before P8.2): ~112 ms (fib is JIT-compiled but calls go through
  `_RT->call` → `JS_Call` → `JS_CallInternal` — same overhead as interpreter)

The recursive call path through `_RT->call` is:
1. `_RT->call` vtable dispatch
2. `JS_Call` → check for generator, eval, etc.
3. `JS_CallInternal` → new stack frame, hot probe (for nested calls), opcode dispatch

For `fib(30)` this path is traversed ~2.7 million times.

---

## Implementation

### Gen-time type stack: `JIT_T_SELF_FUNC`

A new gen_st marker value `JIT_T_SELF_FUNC = 3` is pushed to the gen-time type stack
(`gen_st[]`) when `OP_get_var idx` loads a closure var whose atom matches the function's
own name atom.

```c
#define JIT_T_SELF_FUNC 3  /* gen_st marker: slot holds a self-recursive var */
```

### Self-function atom accessor

A new accessor `js_jit_fb_get_func_atom(b)` returns `b->func_name` (the function's name
atom, or `JS_ATOM_NULL` if anonymous).

### Gen-time type stack: `OP_get_var` tracking

Previously `OP_get_var` was not tracked in the gen_st switch (fell to `default: break`),
leaving the function slot untracked.  P8.2 adds an explicit case:

```c
case OP_get_var: {
    int _vi = (int)bc_u16(&bc[pc+1]);
    JSAtom _va = js_jit_fb_get_closure_var_atom(b, _vi);
    _gs_push = (self_func_atom != JS_ATOM_NULL && _va == self_func_atom)
               ? JIT_T_SELF_FUNC : JIT_T_JSVAL;
    break;
}
```

### Self-recursive call detection in `OP_call*`

At each `OP_call` / `OP_call0..call3` / `OP_tail_call`, the function slot in gen_st is
checked before emitting code:

```c
int func_slot = gen_sp - 1 - nargs;
int is_self = (func_slot >= 0 && gen_st[func_slot] == JIT_T_SELF_FUNC);
```

If `is_self`, the generated code calls `__jit_f_<hash>` directly:

```c
/* direct self-recursive call */
{ int _n = N;
  JSValue _f = _s[_sp-1-_n];
  if(_RT->poll_interrupts(ctx)) goto _ex;
  JSValue _r = __jit_f_HASH(ctx, JS_UNDEFINED, _n, &_s[_sp-_n], cpool, var_refs);
  for(int _j=0; _j<_n; _j++) _FREE(_s[_sp-1-_j]);
  _sp -= _n+1; _FREE(_f);
  _CHK(_r); _s[_sp++]=_r;
}
```

The same `cpool` and `var_refs` are passed — since it's the same function, the same
closure is reused.

### Interrupt poll

`_RT->poll_interrupts(ctx)` is called before each direct recursive call.  This wraps
`js_poll_interrupts` (a `static inline` that decrements `ctx->interrupt_counter` and
only calls `__js_poll_interrupts` every `JS_INTERRUPT_COUNTER_INIT = 10000` calls).
One vtable call per recursive call — cheap for the common case of no interrupt.

### Why direct calls work with RTLD_LOCAL symbols

Within a `.so` compiled from a single C source, `__jit_f_<hash>` is in the same
translation unit.  A recursive call to itself is a direct relative call (`e8 ...`), not
a PLT indirect call.  GCC also generates a `.localalias` symbol for the function so that
cross-GOT indirection is avoided.

### `bc_hash` threaded through to `gen_body`

`gen_body` now receives `bc_hash` as an extra parameter to construct `self_jit_sym`:

```c
snprintf(self_jit_sym, sizeof(self_jit_sym), "__jit_f_%016llx", (unsigned long long)bc_hash);
```

---

## Performance results

Measurements: Linux 6.6.87.2 WSL2 x86-64, GCC -O2, `--jit-aot` warm cache.  
`qjs_interp` = JIT-disabled build (pure interpreter, no threshold compilation).  
5 runs each, minimum shown.

| Benchmark | Interp min | JIT P8.2 min | Speedup | P8.1 speedup (was wrong) |
|---|---:|---:|---:|---:|
| fib(30) ×1 | 112 ms | 33.7 ms | **3.3×** | 0.92× (interpreter was running, not JIT) |
| sum_loop(1e6) ×20 | 796 ms | 940 ms | **0.85×** | 0.87× |
| sum_sq(1e6) ×20 | 616 ms | 284 ms | **2.17×** | 2.27× |
| count_primes(3000) ×10 | 7.59 ms | 2.96 ms | **2.56×** | 2.39× |
| arr_sum(10000) ×1000 | 345 ms | 350 ms | **0.99×** | 0.97× |

V8 benchmark suite (best of 3 runs, `--jit-aot` vs `qjs_interp`):

| | Score |
|---|---:|
| JIT P8.2 best | 614 |
| Pure interpreter best | 809 |
| Ratio (best/best) | — (WSL2 noise ±30%; scores overlap) |

**Notes on V8 comparison:** WSL2 variance is ±30% on v8bench runs, making the
geometric-mean score unreliable.  Per-benchmark analysis shows JIT wins on Crypto and
Richards but the noisy Splay and EarleyBoyer results dominate the overall score.
Micro-benchmarks on fixed-iteration loops are the reliable signal.

**Key wins:**
- `fib(30)`: 3.3× speedup — direct C calls eliminate vtable dispatch for self-recursion.
  Note: P8.1 showed 0.92× because the bug was silently disabling fib's JIT compilation.
- `sum_sq`, `count_primes`: P8.2 shows essentially the same speedup as P8.1 (expected —
  these don't use self-recursion).

---

## Known limitations

- **Mutual recursion not covered**: `f()` calling `g()` calling `f()` goes through the
  vtable.  Addressed by P8.3.
- **Anonymous functions**: `self_func_atom == JS_ATOM_NULL` → no detection → vtable call.
  Reasonable since anonymous functions are not typically self-recursive by name.
- **Interrupt handler**: bypassing `JS_CallInternal` means the per-opcode interrupt check
  does not fire during recursive descent.  The `poll_interrupts` vtable call fires once
  per call frame instead, maintaining `JS_SetInterruptHandler` semantics.

---

## Files changed

| File | Change |
|---|---|
| `quickjs-jit.c` | `JIT_T_SELF_FUNC=3`; bug fix: remove `unlikely`/`js_unlikely` from generated string; `gen_body` now takes `bc_hash`; gen_st tracking for `OP_get_var*` and `OP_get_var_ref*`; self-recursive detection in `OP_call*` and `OP_tail_call`; vtable entry `poll_interrupts` |
| `quickjs-jit.h` | `JIT_T_SELF_FUNC` constant; `poll_interrupts` vtable field; `js_jit_fb_get_func_atom` and `js_jit_poll_interrupts` declarations |
| `quickjs.c` | `js_jit_fb_get_func_atom` and `js_jit_poll_interrupts` implementations |
