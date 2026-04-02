# Phase 8.3 — JIT-to-JIT Call Fast Path

## Summary

P8.3 replaces the `_RT->call` vtable entry with a smarter dispatch function
(`js_jit_call`) that checks whether the callee has already been JIT-compiled
and, if so, calls `jit_func` directly — bypassing `JS_Call → JS_CallInternal`.

This is the complement to P8.2 (direct self-recursive calls):

| Call type | Mechanism | Overhead vs bare C call |
|---|---|---|
| Self-recursive | P8.2 direct C call | Zero (no vtable, no checks) |
| Other JIT-compiled callee | P8.3 `js_jit_call` vtable entry | tag check + class_id + ACQUIRE load + poll |
| Non-JIT callee | P8.3 fall-through to `JS_Call` | Same as before |

---

## Design

### Why not compile-time extern declarations?

The phase8-todo.md description suggested emitting `extern __jit_f_<hash>` for
cpool callees known at codegen time.  That approach requires:

1. Walking the cpool to find `JSFunctionBytecode` objects for each constant
2. Computing the BC_HASH for each callee at codegen time
3. Generating `extern JSValue __jit_f_<hash>(...)` declarations in the .c source
4. Linking them at dlopen time — but the callee `.so` may not exist yet (it might
   compile after the caller)

The runtime-check approach (`js_jit_call`) avoids all of this:
- No codegen changes required
- Works for callees compiled at any time (before or after the caller)
- Degrades gracefully: callee not yet compiled → fall through to `JS_Call`
- Self-recursive calls still use zero-overhead P8.2 direct calls

### Implementation

**`quickjs.c` — `js_jit_call()`:**

```c
JSValue js_jit_call(JSContext *ctx, JSValue func, JSValue this_val,
                    int argc, JSValue *argv)
{
    if (JS_VALUE_GET_TAG(func) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(func);
        if (p->class_id == JS_CLASS_BYTECODE_FUNCTION) {
            JSFunctionBytecode *b = p->u.func.function_bytecode;
            JSJITFunc jf = __atomic_load_n(&b->jit_func, __ATOMIC_ACQUIRE);
            if (jf) {
                if (js_jit_poll_interrupts(ctx))
                    return JS_EXCEPTION;
                return jf(ctx, this_val, argc, argv,
                          b->cpool, p->u.func.var_refs);
            }
        }
    }
    return JS_Call(ctx, func, this_val, argc, argv);
}
```

**`quickjs-jit.c` — vtable wired up:**

```c
.call = js_jit_call,   /* was: jit_rt_call (just called JS_Call) */
```

### Stack overflow safety

`js_jit_call` uses `js_jit_poll_interrupts` (introduced in P8.2) which checks
both JS interrupt handlers and C stack depth (`js_check_stack_overflow`).
This ensures the same stack overflow protection as `JS_CallInternal` even though
the interpreter loop is bypassed.

### Thread safety

`jit_func` is read with `__ATOMIC_ACQUIRE` — consistent with the RELEASE write
done by the GCC worker thread when it installs a compiled function.  No locking
needed on the call path.

---

## What changed

| File | Change |
|---|---|
| `quickjs.c` | Added `js_jit_call()` function (42 lines) |
| `quickjs-jit.h` | Added `js_jit_call` declaration |
| `quickjs-jit.c` | Changed `.call = jit_rt_call` → `.call = js_jit_call` |

---

## Performance

Measurements: Linux 6.6.87.2 WSL2 x86-64, GCC -O2, `--jit-aot` warm cache,
`qjs_interp` (CONFIG_JIT=n) as baseline. 5 runs each, min shown.

```
Benchmark             Interp    JIT P8.3   Speedup   vs P8.2   Notes
────────────────────────────────────────────────────────────────────────────────
fib(30) ×1            112 ms     28 ms     4.0×      +0.7×     self-recursive
sum_loop(1e6) ×20     796 ms    940 ms     0.85×     same      no add_loc yet
sum_sq(1e6) ×20       616 ms    237 ms     2.60×     +0.43×    INT fusion + P8.3
count_primes ×10      7.59 ms   2.48 ms    3.06×     +0.50×    INT fusion + P8.3
arr_sum ×1000         345 ms    350 ms     0.99×     same      get_array_el vtable
```

`sum_sq` and `count_primes` gains come from helper function calls between JIT
functions (the inner loop in `sum_sq` calls a squaring helper; `count_primes`
calls an `isPrime` helper — both now JIT-to-JIT via P8.3).

For a synthetic inter-function benchmark:
```js
function square(x) { return x * x; }
function sumSquares(n) {
    let s = 0;
    for (let i = 0; i < n; i++) s += square(i);
    return s;
}
```
- Interpreter: 783 ms  
- JIT P8.3:    559 ms → **1.40×** (was ~1.05× before P8.3)

---

## Limitations

- The fast path fires only after the callee has been JIT-compiled.  On first
  call (while GCC is still compiling the callee), `JS_Call` is used.
- Native functions, bound functions, and proxies are correctly routed through
  `JS_Call` (tag or class_id check fails → fall through).
- Self-recursive calls still go through P8.2 (zero overhead) — `js_jit_call`
  is only reached for non-self callees, i.e., `_RT->call` in generated code.
