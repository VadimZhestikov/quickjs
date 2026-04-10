# Phase 12 — Generator and Async Function JIT Support

## Status

**IMPLEMENTED** (P12.1–P12.4 complete as of 2026-04-06)

| Sub-phase | Description | Status |
|---|---|---|
| P12.1 | Generator with try/catch (catch state spill) | ✓ Done |
| P12.2 | Generator with closures (vref spill/restore) | ✓ Done |
| P12.3 | `async function` JIT | ✓ Done |
| P12.4 | `async function*` JIT | ✓ Done |
| P12.5 | `OP_yield_star` / `OP_async_yield_star` | Deferred — fallback to interpreter |
| P12.6 | `OP_for_await_of_start` / `OP_for_await_of_next` | Deferred — fallback to interpreter |

---

## Motivation

Generator functions (`function*`) and async functions (`async function`, `async function*`)
were excluded from the JIT by the eligibility gate in `js_jit_is_eligible()`:

```c
if (js_jit_fb_func_kind(b) != JS_JIT_FUNC_NORMAL)
    return 0;
```

This blocked all four non-normal func_kinds:

| `func_kind` value      | Category         |
|------------------------|------------------|
| `JS_FUNC_GENERATOR`    | `function*`      |
| `JS_FUNC_ASYNC`        | `async function` |
| `JS_FUNC_ASYNC_GENERATOR` | `async function*` |
| 0 (normal)             | plain functions — already JIT-compiled |

---

## Root cause: the suspend/resume problem

The interpreter handles `OP_yield` / `OP_await` by saving the entire execution state into
a heap-allocated `JSAsyncFunctionState` and returning to the caller.

The JIT emits **straight-line C functions** whose locals live on the **C stack**.
The C stack vanishes on `return`. There is no `cur_pc` to save — the resume point is a C
label, not an integer.

Additionally, two sources of mutable state must survive across yields:
1. **The C catch stack** (`_catch_depth`, `_catch_sp[]`, `_catch_h[]`) — C locals tracking active try/catch regions
2. **Closure var-refs** (`_sf_vrefs[]`, `_cap_buf[]`) — C-local arrays of JSVarRef pointers and their local-stack buffers

---

## Implemented solution

### `JSJITGeneratorFrame` (quickjs-jit.h)

A heap-allocated save area stored in `JSAsyncFunctionState.jit_gen_frame`:

```c
typedef struct JSJITGeneratorFrame {
    int      resume_idx;    /* -1 = exhausted; 0 = initial yield; N = yield N */
    int      n_lv;          /* count of spilled JSValue locals */
    JSValue *saved_lv;      /* heap array [n_lv] */
    /* catch state */
    int      catch_depth;
    int      catch_sp[32];
    int      catch_h[32];
    /* closure var-ref state */
    int        n_vrefs;
    JSVarRef **saved_vrefs;
} JSJITGeneratorFrame;
```

Key difference from original plan: **no separate int64/double arrays**.
The type inference pass assigns concrete `_li_*`/`_ld_*` slots only when types are
provably non-JSValue.  In practice nearly all generator locals that are live across a yield
are already JSValues (due to the yield expression leaving a JSValue on the virtual stack).
The implementation stores only `saved_lv[n_lv]` to keep the frame compact.

### Allocation / free helpers (quickjs.c)

- `js_jit_gen_init_frame(ctx, n_lv, n_vrefs)` — allocates frame with zero-initialized arrays
- `async_func_free_frame` — frees `saved_vrefs[i]` via `free_var_ref(rt, ...)`, then frees arrays and frame
- `js_jit_gen_save_vrefs(ctx, vrefs, n, gf)` — increments each vref's refcount and copies pointers to frame
- `js_jit_gen_restore_vrefs(vrefs, n, gf)` — transfers frame-owned pointers back to caller, clears frame slots

### Resume dispatch (gen_preamble in quickjs-jit.c)

```c
/* Generated preamble (has_yield functions only) */
JSJITGeneratorFrame *_gf = js_jit_get_gen_frame(ctx);
if (!_gf) {
    _gf = js_jit_gen_init_frame(ctx, N_LV, N_VREFS);
    if (!_gf) goto exception;
}
if (_gf->resume_idx != -2 /* first entry sentinel */) {
    /* restore spilled JSValue locals */
    _jsv_0 = _gf->saved_lv[0]; ...
    /* restore catch state */
    _catch_depth = _gf->catch_depth;
    memcpy(_catch_sp, _gf->catch_sp, ...);
    memcpy(_catch_h,  _gf->catch_h,  ...);
    /* restore var-refs and re-attach to local cap buffers */
    js_jit_gen_restore_vrefs(_sf_vrefs, N_VREFS, _gf);
    for each captured local j:
        _cap_buf[j] = _sf_vrefs[vi]->value;
        _sf_vrefs[vi]->value = JS_UNDEFINED;
        _sf_vrefs[vi]->pvalue = &_cap_buf[j];
        _sf_vrefs[vi]->is_detached = FALSE;
    /* dispatch to resume label */
    switch (_gf->resume_idx) {
    case 1: goto _resume_1;
    ...
    }
    /* case 0: only for func_kind != ASYNC */
}
_resume_0:  /* initial yield fall-through */
```

### OP_initial_yield

Sets `_gf->resume_idx = 0`, saves no locals (none live yet), returns
`FUNC_RET_INITIAL_YIELD` via `js_jit_yield_setup`.

### OP_yield (generators and async generators)

At each yield site (index N ≥ 1):
1. `js_jit_close_caps(ctx, _sf_vrefs, n_vrefs)` — heap-promotes all captured vars
   so `vref->pvalue` no longer points to the C stack
2. `js_jit_gen_save_vrefs(ctx, _sf_vrefs, n_vrefs, _gf)` — snapshot with extra refcounts
3. Save catch state to frame
4. Spill live JSValue locals to `_gf->saved_lv[]`
5. `_gf->resume_idx = N`
6. Call `js_jit_yield_setup` (writes yield value + magic int to `stack_start[-2,-1]`)
7. Return `FUNC_RET_YIELD`

On resume, the dispatch table restores locals (step already done in preamble above).
The `.next(v)` value is read from `js_jit_gen_get_next_val(ctx)`.

### OP_await (async functions and async generators)

Identical to OP_yield but:
- Returns `FUNC_RET_AWAIT` (value 0, not FUNC_RET_YIELD)
- Resume reads the resolved promise value from `js_jit_gen_get_next_val(ctx)`
- `case 0:` in the resume dispatch is **omitted** for `func_kind == JS_JIT_FUNC_ASYNC`
  (async functions have no `OP_initial_yield`)
- `throw_flag` check for rejected promises is handled by the existing
  `JS_CallInternal` GENERATOR path before reaching the JIT call

### OP_return_async

Frees captured locals via `js_jit_close_caps` + explicit `_FREE(_cap_buf[j])` / `_FREE(_arg_cap_buf[j])`,
then falls through to the standard return path.

### Closure handling (P12.2 — the `has_fclosure` bail removal)

The previous `if (sr.has_yield && sr.has_fclosure) return 0;` bail was removed.
The vref spill/restore (described above) handles it correctly:
- At yield: `close_caps` promotes stack-allocated upvalues to the heap;
  subsequent frame save only stores the JSVarRef* pointers
- At resume: pointers are transferred back and re-attached to new local cap buffers

### Async generator (P12.4 — `async function*`, func_kind == 3)

Required only removing the `if (fk > JS_JIT_FUNC_ASYNC_GENERATOR) return 0;` gate.
The existing OP_initial_yield + OP_yield + OP_await + OP_return_async codegen already
covers async generators since they combine both protocols — no additional codegen needed.

### func_kind constants (quickjs-jit.c)

```c
#define JS_JIT_FUNC_NORMAL          0
#define JS_JIT_FUNC_GENERATOR       1
#define JS_JIT_FUNC_ASYNC           2
#define JS_JIT_FUNC_ASYNC_GENERATOR 3
```

---

## What the original plan got wrong

The original P12.0–P12.10 plan described hooking into `JS_CallInternal` at the
`JS_CALL_FLAG_GENERATOR` path (P12.2 in the old numbering). The actual implementation
is simpler: `async_func_resume` in `quickjs.c` detects `b->jit_func` and calls it
directly, passing the `JSJITGeneratorFrame*` via `js_jit_get_gen_frame(ctx)` which reads
`ctx->rt->current_stack_frame` → `JSAsyncFunctionState.jit_gen_frame`.

The plan also said catch state might not need spilling ("you cannot yield from inside
a catch handler"). This is FALSE — QuickJS does allow `yield` inside `try` blocks:
the try region covers the suspension point. Catch state must be spilled.

---

## Remaining deferred items

### `OP_yield_star` / `OP_async_yield_star`

`yield* inner` delegates to an inner iterable. These opcodes are currently emitted
as unsupported → the containing function bails to interpreter.

Approach when implemented: emit a `js_jit_yield_star(ctx, inner_iter, _gf)` C helper
call that runs the delegation loop via the interpreter.

### `OP_for_await_of_start` / `OP_for_await_of_next`

Async iterator opcodes used in `for await (... of ...)`. Excluded until yield_star
is working. Follow the same approach as P15's sync `for_of` handling.

---

## Test coverage

- `jit-tests/js/test_jit_p121_generators.js` — P12.1: generators with try/catch
- `jit-tests/js/test_jit_p123_async.js` — P12.3: async function JIT (6 tests, 200 iters each)
- `jit-tests/js/test_jit_p124_closures_async_gen.js` — P12.2+P12.4: generator+closure and async generator (7 tests)

---

## Interaction with other phases

### P13 (closures)

The `has_fclosure` bail that blocked P13+P12 combinations was removed as part of P12.2.
Closures inside generators work via the vref spill/restore mechanism described above.

### P14 (try/catch)

The C catch stack IS spilled at each yield/await site. See OP_yield implementation above.

---

## Performance impact

- EarleyBoyer: Significant — uses generator-style coroutine patterns
- General async code: Opens JIT to the large class of async state machines
- Yield overhead: ~N memory writes per yield for local spill — negligible since
  `yield` itself is the expensive operation
