# Phase 12 — Generator and Async Function JIT Support

## Motivation

Generator functions (`function*`) and async functions (`async function`, `async function*`)
are currently excluded from the JIT by the eligibility gate in `js_jit_is_eligible()`:

```c
/* Generators/async require saved execution context (yield/await) */
if (js_jit_fb_func_kind(b) != JS_JIT_FUNC_NORMAL)
    return 0;
```

This is a broad exclusion: **any** function with `func_kind != 0` is rejected.  That
covers four categories:

| func_kind value | Category |
|---|---|
| `JS_FUNC_GENERATOR` | `function*` |
| `JS_FUNC_ASYNC` | `async function` |
| `JS_FUNC_ASYNC_GENERATOR` | `async function*` |
| (normal is 0) | plain functions — already JIT-compiled |

In real-world JS, hot loops frequently live inside generators (e.g., coroutine-style
iteration, async state machines).  EarleyBoyer and similar benchmarks use generator-heavy
patterns.  This phase removes the restriction.

---

## Root cause: the suspend/resume problem

The interpreter handles `OP_yield` / `OP_await` by saving the entire execution state into
a heap-allocated `JSAsyncFunctionState` and returning to the caller:

```c
done_generator:
    sf->cur_pc = pc;    // save bytecode program counter
    sf->cur_sp = sp;    // save value stack pointer (stack stays heap-allocated)
    // return FUNC_RET_YIELD / FUNC_RET_AWAIT to caller
```

On the next `.next()` call, `JS_CallInternal` restores `pc` and `sp` and continues the
loop.  This works because the interpreter's value stack (`local_buf[]`) lives on the heap
inside `JSAsyncFunctionState`, so it persists across invocations.

The JIT emits **straight-line C functions** whose locals live on the **C stack**.  The C
stack vanishes on `return`.  There is no `cur_pc` to save — the resume point is a C label,
not an integer.

---

## Solution: explicit spill / resume-dispatch CPS transform

At each `OP_yield` / `OP_await` site, the generated C:
1. **Spills** all live C locals to a side-array in the `JSAsyncFunctionState` heap frame.
2. **Tags** the resume point with an integer index stored in the frame.
3. **Returns** the yield value to the caller (with `FUNC_RET_YIELD` / `FUNC_RET_AWAIT`).

At function entry, a `switch(frame->resume_idx)` dispatches to the correct resume label:

```c
JSValue __jit_f_HASH(JSContext *ctx, JSValue this_val, int argc, JSValue *argv,
                      JSValue *cpool, JSVarRef **var_refs,
                      JSAsyncFunctionState *gf)   /* NEW arg for generators */
{
    /* Restore spilled locals on resume */
    int64_t  _li_i   = 0;
    JSValue  _lv_acc = JS_UNDEFINED;
    if (gf->resume_idx) {
        _li_i   = gf->saved_li[0];
        _lv_acc = gf->saved_lv[0];
        switch (gf->resume_idx) {
        case 1: goto _resume_1;
        case 2: goto _resume_2;
        }
    }

    /* OP_initial_yield: suspend before body starts */
    gf->resume_idx = 0;          /* resume will goto _resume_0 below */
    return JS_NewInt32(ctx, FUNC_RET_INITIAL_YIELD);

    /* ---- body starts after .next() re-enters ---- */
    _resume_0:
    /* ... normal JIT body ... */

    /* OP_yield x: */
    gf->saved_li[0] = _li_i;
    gf->saved_lv[0] = _lv_acc;
    gf->resume_idx  = 1;
    gf->frame.cur_sp[-1] = <yielded value>;  /* push to generator's value stack */
    return JS_NewInt32(ctx, FUNC_RET_YIELD);
    _resume_1:
    /* ... continue after yield ... */
```

The generated function only needs to be registered with the generator machinery via the
existing `js_generator_next` → `async_func_resume` path, with the frame's `cur_pc` set
to a sentinel that redirects to the JIT function.

---

## Dependencies

None from Phase 11.  Phase 12 is independent; it does interact with:
- **P9.2 (stackless IR)** — stackless locals (`_li[]`, `_ld[]`, `_tsv[]`) are easier to
  spill than `JSValue _s[]` because each slot has a known type.  Phase 12 benefits from P9
  being done first but can proceed without it (spill the full `JSValue _s[]` array if not
  stackless).
- **P13 (closures)** — a generator function that also creates closures needs both P12 and
  P13.  Handle the intersection in P13.3.

---

## Step-by-step plan

### P12.0 — Eligibility and scan changes

**Files:** `quickjs-jit.c`

1. Remove the `func_kind != JS_JIT_FUNC_NORMAL` gate in `js_jit_is_eligible()`.
2. Add a new scan-result field: `int has_yield` (set if any yield/await opcode is seen).
3. In `scan_is_unsupported()`, do **not** mark yield/await opcodes as unsupported —
   instead, set `sr->has_yield = 1` and continue scanning.
4. Add the yield/await opcodes to the `gen_body()` switch (stubs first, full impl in
   P12.1–P12.6).

Opcodes affected: `OP_initial_yield`, `OP_yield`, `OP_yield_star`, `OP_async_yield_star`,
`OP_await`, `OP_return_async`.

### P12.1 — `JSJITGeneratorFrame` and function signature extension

**Files:** `quickjs-jit.h`, `quickjs-jit.c`

Define the spill area in the frame:

```c
typedef struct JSJITGeneratorFrame {
    int      resume_idx;      /* 0 = first call; N = resume at _resume_N */
    int      n_li;            /* number of int64 locals spilled */
    int      n_ld;            /* number of double locals spilled */
    int      n_lv;            /* number of JSValue locals spilled */
    int64_t *saved_li;        /* heap array [n_li] */
    double  *saved_ld;        /* heap array [n_ld] */
    JSValue *saved_lv;        /* heap array [n_lv] */
} JSJITGeneratorFrame;
```

Allocate this in `JSAsyncFunctionState` as a union or pointer field (or repurpose
`func_state->frame`'s unused fields).

Change the generated function signature **only** for functions with `has_yield`:

```c
/* plain function (unchanged) */
JSValue __jit_f_HASH(JSContext *ctx, JSValue this_val, int argc, JSValue *argv,
                      JSValue *cpool, JSVarRef **var_refs);

/* generator / async function */
JSValue __jit_f_HASH(JSContext *ctx, JSValue this_val, int argc, JSValue *argv,
                      JSValue *cpool, JSVarRef **var_refs,
                      JSJITGeneratorFrame *_gf);
```

The vtable entry `js_jit_call` (and the new `js_jit_gen_call`) must be updated to pass
`_gf` when calling a JIT-compiled generator.

### P12.2 — Resume dispatch table at function top

**Files:** `quickjs-jit.c` (`gen_preamble()`)

For functions with `has_yield`, emit at the very top of the function body:

```c
if (_gf->resume_idx) {
    /* restore spilled locals */
    _li_x = _gf->saved_li[0];
    /* ... all spilled locals ... */
    switch(_gf->resume_idx) {
    case 1: goto _resume_1;
    /* ... one case per yield site ... */
    }
}
```

The number of yield sites is known from the scan pass (count of
`OP_yield`/`OP_await`/`OP_yield_star` opcodes).

### P12.3 — `OP_initial_yield`

**Files:** `quickjs-jit.c` (`gen_body()`)

`OP_initial_yield` is always the first opcode in a generator body.  It yields control back
to the caller (returning the generator object), and real execution begins on the first
`.next()` call.

Generated code:

```c
/* OP_initial_yield */
_gf->resume_idx = 0;   /* next entry will fall through to _resume_0 */
return JS_NewInt32(ctx, FUNC_RET_INITIAL_YIELD);
_resume_0:             /* label: execution resumes here on first .next() */
```

No spill needed — no locals are live yet.

### P12.4 — `OP_yield` and `OP_await`

**Files:** `quickjs-jit.c` (`gen_body()`)

At each yield site (assigned a sequential index N starting from 1):

```c
/* OP_yield <value> — spill, tag, return */
_gf->saved_li[0] = _li_x;   /* spill all live INT locals */
_gf->saved_ld[0] = _ld_y;   /* spill all live DOUBLE locals */
_gf->saved_lv[0] = _lv_acc; /* spill all live JSValue locals */
_gf->resume_idx  = N;
/* push yielded value to generator's value-stack slot (via frame) */
_gf->frame->yield_val = <top of stack>;
return JS_NewInt32(ctx, FUNC_RET_YIELD);   /* FUNC_RET_AWAIT for await */

_resume_N:   /* re-entry point after next .next() call */
/* reload locals */
_li_x   = _gf->saved_li[0];
_ld_y   = _gf->saved_ld[0];
_lv_acc = _gf->saved_lv[0];
/* the value passed to .next(v) arrives as the result of the yield expression */
```

The live-variable set at each yield point is available from the gen-time type stack
`gen_st[]` — the same structure already used by P9.2 stackless IR.

### P12.5 — `OP_return_async`

**Files:** `quickjs-jit.c` (`gen_body()`)

Equivalent to a normal return in the async context:

```c
/* OP_return_async */
_gf->resume_idx = -1;   /* sentinel: generator is exhausted */
return JS_UNDEFINED;    /* no yield value */
```

### P12.6 — `OP_yield_star` and `OP_async_yield_star`

**Files:** `quickjs-jit.c` (`gen_body()`)

`yield*` delegates to an inner iterable.  This is more complex because it involves a loop
in the interpreter that calls `.next()` on the inner iterator repeatedly.  For the JIT,
the simplest correct approach is to **fall back to the vtable** for `yield*`:

```c
/* OP_yield_star — delegate to vtable (correctness first, optimize later) */
_r = _RT->yield_star(ctx, _gf, <inner_iterable>);
if (JS_IsException(_r)) goto _exception;
```

Mark `OP_yield_star` as "vtable-only" in the initial implementation; a direct
implementation can follow in P12.7 if needed.

---

## Interaction with the generator machinery

The existing `js_generator_next()` in `quickjs.c` calls `async_func_resume()` which calls
`JS_CallInternal()` on the generator's bytecode function.  To redirect to the JIT:

1. `JS_CallInternal` already checks `jit_func` and calls it if set.
2. For generator functions, `jit_func` has a different signature.  Use a wrapper trampoline:

```c
/* Trampoline stored as jit_func for generator functions */
JSValue js_jit_gen_trampoline(JSContext *ctx, JSValue this_val, int argc,
                               JSValue *argv, JSValue *cpool, JSVarRef **var_refs)
{
    JSAsyncFunctionState *s = /* extract from ctx->current_generator */;
    JSJITGeneratorFrame *gf = (JSJITGeneratorFrame*)s->jit_gen_frame;
    return actual_jit_gen_func(ctx, this_val, argc, argv, cpool, var_refs, gf);
}
```

Store `gf` in a new `void *jit_gen_frame` field in `JSAsyncFunctionState`.

---

## Expected impact

| Benchmark | Effect |
|---|---|
| EarleyBoyer | Likely significant — uses generator-style iteration patterns |
| RegExp | No effect (regex engine is C, not JS generators) |
| General JS apps | Opens JIT to large class of previously-excluded functions |

Performance of the JIT-compiled generator body should match plain function JIT performance.
The spill/restore at yield sites adds ~N memory writes per yield, where N is the number of
live locals — acceptable since `yield` itself is the expensive operation.

---

## Estimated effort

| Step | Effort |
|---|---|
| P12.0 eligibility + scan | 0.5 day |
| P12.1 frame struct + signature | 1 day |
| P12.2 resume dispatch | 0.5 day |
| P12.3 OP_initial_yield | 0.5 day |
| P12.4 OP_yield / OP_await | 2 days |
| P12.5 OP_return_async | 0.5 day |
| P12.6 OP_yield_star fallback | 0.5 day |
| Integration + testing | 1.5 days |
| **Total** | **~7 days** |
