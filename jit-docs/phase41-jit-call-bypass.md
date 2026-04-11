# Phase 41: JIT Call Fast Bypass

## Motivation

The `closure_counter` benchmark runs at 38ms for an interpreted caller and ~15ms for a
JIT-compiled caller, vs Node's 2ms. After P40, var_ref access inside the JIT function is
optimized, but the dominant overhead is the **call dispatch path** — the machinery that
sets up a stack frame and invokes the JIT function pointer.

Two distinct call paths need optimization:

| Caller type | Current path | Overhead |
|-------------|-------------|---------|
| Interpreted (top-level loop) | `JS_CallInternal` → alloca + sf setup → JIT call | ~30 cycles |
| JIT-compiled (call IC) | `js_jit_ic_direct_call` → save/update cur_func + 0-arg call | ~30 cycles |

The JIT function body for `() => ++n` is ~15-20 cycles. The call overhead is ~30 cycles on
each side — the function body is less than half the total cost.

## Root Cause Analysis

### Path 1: Interpreted caller → JS_CallInternal

Even when the JIT function pointer is installed, `JS_CallInternal` does substantial setup
**before** reaching the JIT check at line 19629:

```c
/* Done before the JIT check (lines 19548–19613): */
arg_allocated_size = ...;                              /* 1 branch + assign */
alloca_size = sizeof(JSValue)*(args+vars+stack) + ...; /* 2 mults + add */
js_check_stack_overflow(rt, alloca_size);              /* load + compare */
sf->js_mode = b->js_mode;                             /* load + store */
sf->arg_count = argc;                                  /* store */
sf->cur_func = func_obj;                               /* 2 stores (JSValue=16B) */
var_refs = p->u.func.var_refs;                         /* load */
local_buf = alloca(alloca_size);                       /* sub rsp */
var_buf = local_buf + arg_allocated_size;              /* add */
sf->arg_buf = arg_buf;                                 /* store */
sf->var_buf = var_buf;                                 /* store */
sf->var_refs = (JSVarRef**)(stack_buf + stack_size);   /* add + store */
/* init var_buf slots to JS_UNDEFINED (skip if var_count=0) */
/* init sf->var_refs[] to NULL (skip if var_ref_count=0) */
sf->prev_frame = rt->current_stack_frame;              /* load + store */
sf->new_target = new_target;                           /* 2 stores */
rt->current_stack_frame = sf;                          /* store */
ctx = b->realm;                                        /* load + store */
/* THEN check jit_func: */
jf = b->jit_func;                                      /* atomic load */
if (jf != NULL) { ... }
```

For `() => ++n` (0 args, 0 vars, var_ref_count=0): the alloca size is tiny (2 JSValue =
32 bytes), but all the surrounding code still executes — ~25 instructions before the JIT
function is invoked. After, `close_var_refs` is called (a function call with a 0-iteration
loop = ~10 cycles of call overhead).

**Total overhead per call: ~35 cycles**.

### Path 2: JIT caller → js_jit_ic_direct_call

When a JIT-compiled function calls `counter()` via the call IC:

```c
/* Emitted in generated C: */
if (_fo == _cic11.expected_func && _cic11.direct_jit && ...) {
    ret = js_jit_ic_direct_call(ctx, this_val, 0, NULL, &_cic11, var_refs);
}
```

`js_jit_ic_direct_call` (lines 16259–16315) does:
```c
rt = JS_GetRuntime(ctx);                               /* load through ctx */
caller_sf = rt->current_stack_frame;                   /* load */
saved_cur_func = caller_sf->cur_func;                  /* load (JSValue=16B) */
saved_new_target = caller_sf->new_target;              /* load */
caller_sf->cur_func = func_val;                        /* store */
caller_sf->new_target = JS_UNDEFINED;                  /* store */
/* For n=0 (zero-arg callee): */
ret = ic->direct_jit(ctx, this_val, argc, argv,
                     ic->callee_cpool, var_refs);      /* indirect call */
caller_sf->cur_func = saved_cur_func;                  /* store */
caller_sf->new_target = saved_new_target;              /* store */
```

**Total overhead: function-call save/restore (~15 cycles) + body (~15 cycles) = ~30 cycles**.

## Sub-phases

### P41.1 — Early JIT Bypass in JS_CallInternal (Primary Win)

**Idea:** Check for a JIT-compiled function pointer **before** the alloca. When specific
conditions are met, set only the minimal stack-frame fields and call the JIT function
directly — skipping the alloca, var_buf/stack_buf setup, and `close_var_refs`.

**Conditions (all must hold):**
1. `b->jit_func != NULL` — JIT compiled, can call directly
2. `b->var_ref_count == 0` — no new JSVarRefs created during execution (no OP_define_class
   writing to `sf->var_refs[]`), so `close_var_refs` is a provable no-op
3. `b->has_simple_parameter_list` — no rest/default/destructuring; `b->arg_count` is the
   exact parameter count; no need for the `real_argc` plumbing
4. `argc >= b->arg_count` — no argv padding needed; caller's argv is large enough
5. `js_jit_fb_func_kind(b) == 0` — `JS_FUNC_NORMAL`; generators and async need the
   JSStackFrame for coroutine resumption
6. `!(flags & JS_CALL_FLAG_COPY_ARGV)` — no copy-on-write constraint; argv is stable

**Where to insert:** Between `b = p->u.func.function_bytecode;` and the
`arg_allocated_size` computation (currently at line ~19548).

**Code:**
```c
#ifdef CONFIG_JIT
/* P41.1: Early JIT fast-path — bypass alloca and most SF setup for already-compiled
 * functions that need no interpreter-owned stack buffers. */
{
    JSJITFunc jf = __atomic_load_n(&b->jit_func, __ATOMIC_ACQUIRE);
    if (jf != NULL &&
        b->var_ref_count == 0 &&
        b->has_simple_parameter_list &&
        argc >= b->arg_count &&
        js_jit_fb_func_kind(b) == 0 /* JS_FUNC_NORMAL */ &&
        !(flags & JS_CALL_FLAG_COPY_ARGV))
    {
        /* Minimal JSStackFrame — no alloca, no var_buf, no stack_buf. */
        sf->prev_frame  = rt->current_stack_frame;
        sf->cur_func    = (JSValue)func_obj;
        sf->new_target  = (JSValue)new_target;
        sf->arg_count   = argc;            /* for emergency exception recovery */
        sf->cur_pc      = b->byte_code_buf; /* points to start; for backtrace */
        sf->var_refs    = NULL;             /* unused: var_ref_count == 0 */
        rt->current_stack_frame = sf;
        ctx = b->realm;                     /* must use callee's realm */
        JSValue jit_ret = jf(ctx, (JSValue)this_obj, b->arg_count, argv,
                             b->cpool, p->u.func.var_refs);
        rt->current_stack_frame = sf->prev_frame;
        /* No close_var_refs: var_ref_count == 0 guarantees sf->var_refs was
         * never written to during JIT execution. */
        return jit_ret;
    }
}
#endif
```

**Cycles saved:** ~25 instructions skipped before the call + `close_var_refs` call skipped
≈ 20-25 cycles/call. At 1M calls: ~7-9ms improvement (interpreted→JIT path).

**Safety notes:**
- `sf_s` is already a local variable in `JS_CallInternal` — no extra allocation.
- `sf->var_refs = NULL` is safe: `close_var_refs` loops over `b->var_ref_count == 0`
  iterations and never dereferences `sf->var_refs`.
- `ctx = b->realm` is required: the JIT function may create objects in the callee's realm.
- Stack overflow: omitted here (the C stack only grows by `sizeof(JSStackFrame)` worth of
  data, not the alloca'd var/stack buffers). Optionally add a lightweight check.
- `b->arg_count` as the passed argc: for simple-param functions the JIT function only
  accesses `argv[0..arg_count-1]`; passing more is safe (JIT ignores extras).

### P41.2 — Slim Fast-Call Path for Zero-Arg Closures in Call IC

**Idea:** For the JIT-to-JIT call IC path, add a new slim vtable function
`js_jit_ic_fast_call` that handles zero-arg, no-home-object callees without the general
argv-padding code in `js_jit_ic_direct_call`.

**New JSJITCallICEntry field:**
```c
typedef struct {
    void                *expected_func;
    JSFunctionBytecode  *expected_bc;
    JSJITFunc            direct_jit;
    JSValue             *callee_cpool;
    JSVarRef           **callee_var_refs;
    int                  callee_arg_count;
    uint64_t             callee_bc_hash;
    uint8_t              callee_is_fast;   /* P41.2: 1 if zero-arg + no HOME_OBJECT */
} JSJITCallICEntry;
```

**`js_jit_ic_fast_call` (quickjs.c):**
```c
JSValue js_jit_ic_fast_call(JSContext *ctx, JSValue this_val,
                             JSJITCallICEntry *ic, JSVarRef **var_refs)
{
    /* Like js_jit_ic_direct_call but for zero-arg, no-home callee.
     * Skips: argv alloca/DUP/free, new_target save/restore, arg_count branching. */
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSStackFrame *sf = rt->current_stack_frame;
    JSValue saved = JS_UNDEFINED;
    if (sf) { saved = sf->cur_func; sf->cur_func = ic->callee_func_val; }
    JSValue ret = ic->direct_jit(ctx, this_val, 0, NULL,
                                 ic->callee_cpool, ic->callee_var_refs);
    if (sf) sf->cur_func = saved;
    return ret;
}
```

This requires adding `JSValue callee_func_val` to `JSJITCallICEntry` (for the cur_func
update) and updating `js_jit_callIC_fill` to populate it.

**Emitter change:** When `ic->callee_is_fast` is detected at fill time, the IC hot path
emits `_RT->jit_fast_call(ctx, this_val, &_cic, var_refs)` instead of
`js_jit_ic_direct_call(...)`. At emit time we cannot know `callee_is_fast` yet (IC is cold),
so we check it dynamically: emit a branch on `_cic11.callee_is_fast` that picks one of two
call sites.

Actually simpler: just always use `_RT->jit_fast_call` for zero-arg callees identified at
emit time (if the callee's arg_count is statically known to be 0 from the P10.3 hash
table). This avoids the runtime branch. Zero-arg closure calls are the primary target.

**Cycles saved:** ~10 cycles/call (skip new_target save/restore + arg_count branching +
function call overhead vs slim function). At 1M calls: ~3-4ms improvement (JIT→JIT path).

### P41.3 — Tests + Docs + Benchmarks

Three C harness tests in `jit-tests/P41/`:
1. `test_p41_1.c` — correctness: closure called from interpreted loop still returns
   correct values after fast-bypass; verify refcounting via object closure.
2. `test_p41_2.c` — correctness: closure called from JIT-compiled outer function works
   correctly with P41.2 fast-call IC.
3. `test_p41_3.c` — exception safety: closure that throws propagates the exception
   correctly through the fast-bypass path; backtrace is formed.

## Expected Results (Planned)

| Benchmark | Before P41 | After P41.1 | After P41.2 | Node v24 |
|-----------|-----------|-------------|-------------|---------|
| closure_counter (interpreted caller, 1M) | 38 ms | ~30 ms | ~30 ms | 2 ms |
| closure_counter (JIT caller, 1M) | ~15 ms | ~15 ms | ~12 ms | 2 ms |
| Gap vs Node (interpreted) | 19× | ~15× | ~15× | — |
| Gap vs Node (JIT caller) | 7.5× | 7.5× | ~6× | — |

## Actual Results (2026-04-10)

### P41.1 — Early JIT Bypass

| Benchmark | Before P41.1 | After P41.1 | Improvement |
|-----------|-------------|-------------|-------------|
| closure_counter (interpreted caller, 1M) | 38-40 ms | 29-31 ms | ~25% |
| closure_counter (JIT caller, 1M, bench not JIT) | 35-40 ms | 27-29 ms | ~25% |

P41.1 skips the alloca + full JSStackFrame setup for JIT-compiled callees that meet
all bypass criteria. Saves ~10-15 instructions before the JIT call and eliminates
the `close_var_refs` no-op call after it.

### P41.2 — Slim IC Fast-Call for Zero-Arg Closures

P41.2 adds `callee_is_fast` to `JSJITCallICEntry` and `js_jit_ic_fast_call()`.
For zero-arg, NORMAL, no-HOME_OBJECT closures, the IC hot path skips all
cur_func/new_target save/restore and calls the JIT function pointer directly.

| Benchmark | Before P41.2 | After P41.2 | Improvement |
|-----------|-------------|-------------|-------------|
| closure_counter (fully JIT, bench×150 warm) | 37-39 ms | 7-8 ms | ~5× |

The massive speedup for fully-JIT callers is because:
- The P10.3 static direct-call path (known callee at compile time) calls the JIT
  function directly as a C function — no JS_CallInternal, no IC struct access.
- P41.1 eliminates stack frame overhead for the counter's inner return path.
- P41.2 eliminates cur_func/new_target save/restore for IC-dispatched zero-arg calls.

**Note:** The P10.3 static dispatch path (`js_jit_check_and_extract`) is used when
the callee is known at JIT compile time. P41.2's IC path (`js_jit_ic_fast_call`) is
used for call sites where the callee is determined at runtime via the IC.

P41.1 mainly helps the interpreted-caller scenario. P41.2 mainly helps JIT-to-JIT
via the call IC. Both together produce ~25-500% improvement depending on the ratio
of statically-known vs dynamically-dispatched callees.

The remaining gap (~4-15×) vs Node is mostly intrinsic JS call overhead: JS values
are 16 bytes each, boxing/unboxing around the call boundary adds DupValue/FreeValue
pairs, and QuickJS passes ownership on every call whereas V8's tagged NaN representation
can pass values by register with no heap traffic.

## What P41 Does Not Do

- Does not change the call path for async/generator functions (these need the full SF
  for coroutine resumption)
- Does not eliminate `js_jit_ic_direct_call` (still used for >0-arg callees)
- Does not affect functions that create var_refs (OP_define_class, class constructors)
- Does not change backtrace format — `sf->cur_func` is still updated for the callee
- Does not change the JIT function signature or ABI
