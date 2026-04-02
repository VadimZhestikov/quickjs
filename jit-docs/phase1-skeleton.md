# Phase 1 — JIT Skeleton Infrastructure

**Commit:** `731c0f2`
**Goal:** Wire up the hot probe and calling convention; no compilation yet.

---

## What was added

### Fields in JSFunctionBytecode (quickjs.c)

Five fields added inside `#ifdef CONFIG_JIT` blocks:

```c
struct JSFunctionBytecode {
    ...
#ifdef CONFIG_JIT
    uint8_t    jit_tier;        /* 0=none  1=TCC(removed)  2=GCC */
    uint8_t    jit_no_compile;  /* 1 = do not attempt JIT again  */
    int        jit_call_count;  /* incremented before threshold check */
    JSJITFunc  jit_func;        /* NULL until compiled; ACQUIRE-read on hot path */
    void      *jit_handle;      /* dlopen handle; NULL for TCC (in-process) */
#endif
};
```

All fields accessed through accessor functions declared in `quickjs-jit.h` and defined
in the `#ifdef CONFIG_JIT` section at the bottom of `quickjs.c`.  This keeps the struct
layout opaque to `quickjs-jit.c`.

### JIT function type

```c
typedef JSValue (*JSJITFunc)(JSContext   *ctx,
                             JSValue      this_val,
                             int          argc,
                             JSValue     *argv,
                             JSValue     *cpool,
                             JSVarRef   **var_refs);
```

Arguments match what `JS_CallInternal` already has on hand — no marshalling needed.
Return value: a new `JSValue` reference owned by the caller (matches interpreter contract).

### Hot probe in JS_CallInternal (quickjs.c ~line 17695)

```c
#ifdef CONFIG_JIT
    {
        JSJITFunc jf = (JSJITFunc)
            __atomic_load_n(&b->jit_func, __ATOMIC_ACQUIRE);
        if (jf) {
            /* Fast path: JIT stub already compiled.  Call it directly. */
            return jf(ctx, this_val, argc, argv,
                      b->cpool, b->closure_var ? var_refs : NULL);
        }
        /* Increment call counter and queue compilation at threshold. */
        int cnt = js_jit_fb_inc_count(b);
        if (cnt == JIT_THRESHOLD_GCC && js_jit_is_eligible(b)) {
            js_jit_queue_gcc(ctx, b);
        }
    }
#endif
```

Key properties of this probe:
- **Single ACQUIRE load** of `jit_func` — no mutex, no branch prediction penalty after JIT warms up
- **No-op overhead** before compilation: one atomic load + one NULL check + one increment
- `js_jit_is_eligible()` gates compilation on function properties (no generators, no eval, etc.)

### Eligibility check (js_jit_is_eligible)

Functions excluded from JIT:
- Generators and async functions (`func_kind != JS_FUNC_NORMAL`)
- Functions with complex parameter lists (destructuring, default values)
- Class methods requiring `home_object` (super references)
- Derived class constructors (`new.target` semantics)
- Direct or indirect `eval` calls

These all require complex interpreter state that the generated stub cannot replicate simply.

### Bytecode accessor pattern

`JSFunctionBytecode` is defined entirely in `quickjs.c` and not exposed via any header.
`quickjs-jit.c` (and JIT-generated `.so` files) cannot `#include` the struct definition.

All access goes through thin wrappers in `quickjs.c`:

```c
/* Examples: */
uint8_t js_jit_fb_get_tier(JSFunctionBytecode *b)  { return b->jit_tier; }
int     js_jit_fb_inc_count(JSFunctionBytecode *b) { return ++b->jit_call_count; }
void    js_jit_fb_set_no_compile(JSFunctionBytecode *b) { b->jit_no_compile = 1; }
void    js_jit_fb_set_func(JSFunctionBytecode *b, JSJITFunc f, void *h, int tier) {
    b->jit_handle = h;
    b->jit_tier   = (uint8_t)tier;
    __atomic_store_n(&b->jit_func, f, __ATOMIC_RELEASE);
}
```

The RELEASE store in `js_jit_fb_set_func` pairs with the ACQUIRE load in the hot probe,
guaranteeing that the worker thread's writes to the `.so` mapping are visible to the main
thread before `jit_func` becomes non-NULL.

---

## Sequence diagram

```
Main thread                         GCC worker thread
────────────                        ─────────────────
call JS_CallInternal(b)
  atomic_load(b->jit_func)  → NULL
  b->jit_call_count++
  [count == threshold]
    js_jit_queue_gcc(ctx, b)
      gen_body() → c_source         ← generated synchronously on main thread
      enqueue job ─────────────────────────────────────────────────────────►
      return (non-blocking)                                                 │
                                                               dequeue job  │
  ← continues interpreting                                                  │
                                                        write .c file       │
                                                        fork + exec gcc     │
                                                        dlopen .so          │
                                                        dlsym(func)         │
                                                        atomic_store        │
                                                          (RELEASE)  ───────►
call JS_CallInternal(b) again
  atomic_load(b->jit_func)  → func  ← sees RELEASE store
  call func(ctx, ...)
  ← native code executes
```

---

## Result

After Phase 1 the JIT machinery is live but no functions are ever compiled — the hot
probe increments a counter and calls `js_jit_queue_gcc` which is a stub returning
immediately.  Overhead in the hot path: ~3 ns (one atomic load + branch + increment).
