# Phase 12 — Generator and Async Function JIT Support

## Motivation

Generator functions (`function*`) and async functions (`async function`, `async function*`)
are currently excluded from the JIT by the eligibility gate in `js_jit_is_eligible()`:

```c
/* quickjs-jit.c: js_jit_is_eligible() */
if (js_jit_fb_func_kind(b) != JS_JIT_FUNC_NORMAL)
    return 0;
```

This is a broad exclusion: **any** function with `func_kind != 0` is rejected:

| `func_kind` value      | Category         |
|------------------------|------------------|
| `JS_FUNC_GENERATOR`    | `function*`      |
| `JS_FUNC_ASYNC`        | `async function` |
| `JS_FUNC_ASYNC_GENERATOR` | `async function*` |
| 0 (normal)             | plain functions — already JIT-compiled |

In real-world JS, hot loops frequently live inside generators (e.g., coroutine-style
iteration, async state machines).  EarleyBoyer and similar benchmarks use generator-heavy
patterns.  This phase removes the restriction.

---

## Root cause: the suspend/resume problem

The interpreter handles `OP_yield` / `OP_await` by saving the entire execution state into
a heap-allocated `JSAsyncFunctionState` and returning to the caller.

### Key data structures

```c
/* quickjs.c ~line 752 */
typedef struct JSAsyncFunctionState {
    JSGCObjectHeader header;
    JSValue this_val;          /* 'this' argument */
    int argc;
    BOOL throw_flag;           /* throw on next resume */
    BOOL is_completed;
    JSValue resolving_funcs[2]; /* async functions only */
    JSStackFrame frame;
    /* Immediately after (single malloc): */
    /* JSValue arg_buf[max(arg_count, argc)] */
    /* JSValue var_buf[var_count]            */
    /* JSValue stack_buf[stack_size]         */
    /* JSVarRef* var_refs[var_ref_count]     */
} JSAsyncFunctionState;
```

The `JSStackFrame` embedded in `JSAsyncFunctionState` has:
- `sf->cur_pc` — bytecode PC (saved/restored across yield)
- `sf->cur_sp` — value stack pointer into `stack_buf`
- `sf->arg_buf` → `(JSValue*)(s + 1)` (args + vars + stack in one alloc)
- `sf->var_buf` → `sf->arg_buf + arg_buf_len`

### Resume protocol in `JS_CallInternal`

`async_func_resume()` calls `JS_CallInternal` with a fake `func_obj`:

```c
func_obj = JS_MKPTR(JS_TAG_INT, s);   /* s = JSAsyncFunctionState* */
ret = JS_CallInternal(ctx, func_obj, s->this_val, JS_UNDEFINED,
                      s->argc, sf->arg_buf, JS_CALL_FLAG_GENERATOR);
```

`JS_CallInternal` detects `JS_CALL_FLAG_GENERATOR` (line ~18463), extracts `s` from the
`func_obj` pointer, restores `sf->cur_pc` and `sf->cur_sp`, then jumps to `restart`.
**It does not currently check `b->jit_func`** in this path.

### Yield value handshake in `js_generator_next`

Before calling `async_func_resume`, `js_generator_next` loads the `.next(v)` argument:

```c
sf->cur_sp[-1] = ret;            /* the value passed to .next(v) */
sf->cur_sp[0]  = JS_NewInt32(ctx, magic);  /* GEN_MAGIC_NEXT / THROW / RETURN */
sf->cur_sp++;
```

After `async_func_resume` returns:

```c
ret = sf->cur_sp[-1];       /* yield value placed here by OP_yield handler */
sf->cur_sp[-1] = JS_UNDEFINED;
```

### The JIT problem

The JIT emits **straight-line C functions** whose locals live on the **C stack**.
The C stack vanishes on `return`.  There is no `cur_pc` to save — the resume point is a C
label, not an integer.

---

## Solution: explicit spill / resume-dispatch CPS transform

At each `OP_yield` / `OP_await` site, the generated C:
1. **Spills** all live typed locals to a side-buffer in a `JSJITGeneratorFrame` structure
   stored in the generator's `JSAsyncFunctionState`.
2. **Tags** the resume point with an integer index.
3. **Returns** the yield value to the caller by writing it to `sf->cur_sp[-1]` then
   returning `JS_NewInt32(ctx, FUNC_RET_YIELD)`.

At function entry, a `switch(_gf->resume_idx)` dispatches to the correct resume label.

---

## Step-by-step plan

### P12.0 — Add `jit_gen_frame` field to `JSAsyncFunctionState`

**Files:** `quickjs.c`

Add one pointer field to the existing struct:

```c
typedef struct JSAsyncFunctionState {
    JSGCObjectHeader header;
    JSValue this_val;
    int argc;
    BOOL throw_flag;
    BOOL is_completed;
    JSValue resolving_funcs[2];
    JSStackFrame frame;
#ifdef CONFIG_JIT
    void *jit_gen_frame;   /* JSJITGeneratorFrame*, NULL for interpreter */
#endif
    /* arg_buf / var_buf / stack_buf / var_refs follow */
} JSAsyncFunctionState;
```

Initialize `s->jit_gen_frame = NULL` in `async_func_init` (already zeroed via `memset`).

Free it in `async_func_free_frame` if non-NULL:

```c
#ifdef CONFIG_JIT
    if (s->jit_gen_frame) {
        JSJITGeneratorFrame *gf = s->jit_gen_frame;
        js_free_rt(rt, gf->saved_lv);
        js_free_rt(rt, gf->saved_ld);
        js_free_rt(rt, gf->saved_li);
        js_free_rt(rt, gf);
        s->jit_gen_frame = NULL;
    }
#endif
```

### P12.1 — Define `JSJITGeneratorFrame`

**Files:** `quickjs-jit.h`

```c
typedef struct JSJITGeneratorFrame {
    int      resume_idx;   /* 0 = first call; N = resume after yield N */
    int      n_li;         /* count of spilled int64 locals */
    int      n_ld;         /* count of spilled double locals */
    int      n_lv;         /* count of spilled JSValue locals */
    int64_t *saved_li;     /* heap array [n_li], NULL if none */
    double  *saved_ld;     /* heap array [n_ld], NULL if none */
    JSValue *saved_lv;     /* heap array [n_lv], NULL if none */
} JSJITGeneratorFrame;
```

Allocation helper (called from JIT preamble on first entry):

```c
JSJITGeneratorFrame *js_jit_gen_frame_alloc(JSContext *ctx,
    JSAsyncFunctionState *s, int n_li, int n_ld, int n_lv);
void js_jit_gen_frame_free(JSRuntime *rt, JSJITGeneratorFrame *gf);
```

### P12.2 — Hook JIT into the `JS_CALL_FLAG_GENERATOR` path

**Files:** `quickjs.c` (`JS_CallInternal`)

In the `if (flags & JS_CALL_FLAG_GENERATOR)` block (line ~18463), after extracting `s`
and `b`, check for a JIT function:

```c
if (flags & JS_CALL_FLAG_GENERATOR) {
    JSAsyncFunctionState *s = JS_VALUE_GET_PTR(func_obj);
    sf = &s->frame;
    p  = JS_VALUE_GET_OBJ(sf->cur_func);
    b  = p->u.func.function_bytecode;
    ctx = b->realm;
    var_refs = p->u.func.var_refs;
    
#ifdef CONFIG_JIT
    {
        JSJITFunc jf = __atomic_load_n(&b->jit_func, __ATOMIC_ACQUIRE);
        if (jf) {
            JSJITGeneratorFrame *gf = s->jit_gen_frame;
            /* Allocate frame on first call */
            if (!gf) {
                gf = js_jit_gen_frame_alloc(ctx, s, ...);
                if (!gf) return JS_EXCEPTION;
                s->jit_gen_frame = gf;
            }
            sf->cur_sp = NULL;  /* mark as running */
            sf->prev_frame = rt->current_stack_frame;
            rt->current_stack_frame = sf;
            JSValue ret = jf(ctx, s->this_val, s->argc, sf->arg_buf,
                             b->cpool, var_refs);
            rt->current_stack_frame = sf->prev_frame;
            /* ... handle ret (see P12.6) ... */
            return ret;
        }
    }
#endif
    
    /* existing interpreter path below */
    local_buf = arg_buf = sf->arg_buf;
    ...
}
```

**Important**: The n_li/n_ld/n_lv counts must be embedded in the `JSFunctionBytecode`
(or derived from the scan) so `js_jit_gen_frame_alloc` knows the sizes.  Add them to
`JSJITScanResult` and store in the bytecode's JIT metadata block.

### P12.3 — Eligibility and scan changes

**Files:** `quickjs-jit.c`

1. Remove the `func_kind` gate in `js_jit_is_eligible()`.
2. Keep `OP_for_await_of_start` and `OP_for_await_of_next` excluded until async iterator
   support is confirmed working.
3. In `scan_is_unsupported()`, do **not** mark yield/await opcodes as unsupported.
   Instead set a `sr->has_yield = 1` flag.
4. Count yield sites: `sr->yield_count` incremented for each
   `OP_initial_yield`, `OP_yield`, `OP_yield_star`, `OP_async_yield_star`, `OP_await`.

Opcodes to handle: `OP_initial_yield`, `OP_yield`, `OP_yield_star`,
`OP_async_yield_star`, `OP_await`, `OP_return_async`.

### P12.4 — Generator function signature

**Files:** `quickjs-jit.c` (`gen_preamble`)

Generated function signature is **unchanged** — it always has the 6-arg form:

```c
JSValue __jit_f_HASH(JSContext *ctx, JSValue this_val, int argc, JSValue *argv,
                     JSValue *cpool, JSVarRef **var_refs)
```

The `JSJITGeneratorFrame *_gf` is retrieved at the top of the function from the current
stack frame's `JSAsyncFunctionState`:

```c
/* Generator preamble (only emitted when has_yield) */
JSJITGeneratorFrame *_gf = NULL;
{
    JSStackFrame *_sf = ctx->rt->current_stack_frame;
    JSAsyncFunctionState *_gas =
        (JSAsyncFunctionState *)((char*)_sf - offsetof(JSAsyncFunctionState, frame));
    _gf = (JSJITGeneratorFrame *)_gas->jit_gen_frame;
}
```

This avoids changing the calling convention; the generator frame is always accessible via
`ctx->rt->current_stack_frame`.

### P12.5 — Resume dispatch table

**Files:** `quickjs-jit.c` (`gen_preamble`)

Immediately after `_gf` retrieval, emit the dispatch table:

```c
if (_gf->resume_idx != 0) {
    /* restore spilled int64 locals */
    _li_0 = _gf->saved_li[0];
    /* ... all _li_N locals ... */
    /* restore spilled double locals */
    _ld_0 = _gf->saved_ld[0];
    /* restore spilled JSValue locals (no dup needed — ownership transferred at spill) */
    _tsv_0 = _gf->saved_lv[0];
    /* ... */
    switch (_gf->resume_idx) {
    case 1: goto _resume_1;
    case 2: goto _resume_2;
    /* ... one case per yield/await site ... */
    }
}
```

The number of cases equals `sr->yield_count` (minus `OP_initial_yield` which is case 0).

### P12.6 — `OP_initial_yield`

`OP_initial_yield` is always the first opcode in a generator body.  It signals to the
runtime that the generator is suspended at start (no body has run yet).

```c
/* OP_initial_yield */
{
    JSStackFrame *_sf = ctx->rt->current_stack_frame;
    _sf->cur_sp = argv;        /* save sp (points to arg area, nothing live yet) */
    _sf->cur_pc = pc_sentinel; /* not used in JIT; set to NULL or start of bc */
    _gf->resume_idx = 0;       /* resume_idx 0 → fall through to _resume_0 */
}
return JS_NewInt32(ctx, FUNC_RET_INITIAL_YIELD);
_resume_0:   /* first .next() call lands here */
```

No local spill needed — no locals are live at `OP_initial_yield`.

### P12.7 — `OP_yield` and `OP_await`

At each yield site (assigned sequential index N starting from 1):

```c
/* OP_yield (top of stack has the value to yield) */
{
    JSStackFrame *_sf = ctx->rt->current_stack_frame;
    /* Spill live locals */
    _gf->saved_li[0] = _li_0;
    /* ... all live int64 locals ... */
    _gf->saved_ld[0] = _ld_0;
    /* ... all live double locals ... */
    /* Transfer ownership of JSValues (no dup — caller now owns them) */
    _gf->saved_lv[0] = _tsv_M;
    /* ... all live JSValue stack slots that aren't the yield value ... */
    _gf->resume_idx = N;
    /* Write yield value to sf->cur_sp[-1] (where js_generator_next reads it) */
    _sf->cur_sp = argv + 1;     /* point past arg area; use slot at [-1] */
    _sf->cur_sp[-1] = _tsv_TOP; /* the value being yielded */
}
return JS_NewInt32(ctx, FUNC_RET_YIELD);   /* FUNC_RET_AWAIT for OP_await */

_resume_N:
/* Reload locals (already done in dispatch table at top) */
/* The value passed to .next(v) arrives at _tsv_TOP position */
/* js_generator_next placed it at sf->cur_sp[-1], then incremented sp */
/* In JIT terms: the result of the yield expression = argv[...] or stack slot */
```

**Stack slot for .next(v) value**: `js_generator_next` writes the `.next(v)` value to
`sf->cur_sp[-1]` before calling `async_func_resume`.  In the JIT, after resume dispatch,
this value is available as a specific spilled `_tsv` slot that represented the top of the
stack right before the yield.

**`throw_flag` handling**: If `gen.throw(e)` is called, `s->throw_flag` is set.
The existing `if (s->throw_flag) goto exception;` in `JS_CallInternal`'s GENERATOR path
handles this before reaching the JIT call.  The JIT sees it as a normal exception entry.

### P12.8 — `OP_return_async`

Marks the end of an async function body.  The return value is on top of the JIT stack.

```c
/* OP_return_async */
{
    JSValue _rv = _tsv_TOP;   /* value being returned */
    JSStackFrame *_sf = ctx->rt->current_stack_frame;
    _sf->cur_sp = argv;       /* clear stack (is_completed path in async_func_resume
                                  reads sf->cur_sp[-1] only when NOT undefined) */
    /* Write return value so async_func_resume can retrieve it */
    _sf->cur_sp = argv + 1;
    _sf->cur_sp[-1] = _rv;
    _gf->resume_idx = -1;     /* sentinel: exhausted */
}
return JS_UNDEFINED;   /* async_func_resume detects JS_IsUndefined → completed */
```

### P12.9 — `OP_yield_star` and `OP_async_yield_star`

`yield*` delegates to an inner iterable.  The interpreter implements this with a loop that
calls `.next()` on the inner iterator while `FUNC_RET_YIELD_STAR` is returned.

For the initial JIT implementation, **fall back to a C helper**:

```c
/* quickjs-jit.h / quickjs.c */
JSValue js_jit_yield_star(JSContext *ctx, JSValue inner_iter, JSJITGeneratorFrame *gf);
```

This helper runs the delegation loop synchronously (similar to the interpreter's inner
loop in `js_generator_next`) and returns the final result.  Yield points *within* the
delegate are handled by the interpreter, not the JIT.

This is correct and safe; optimize with a full JIT delegation in P12.10 if needed.

### P12.10 — `OP_for_await_of_start` / `OP_for_await_of_next`

These are the async iterator opcodes (used in `for await (... of ...)` inside `async`
functions).  They depend on `await` being correctly implemented.  Leave excluded from the
JIT until P12.7 (`OP_await`) is confirmed working, then follow the same approach as P15's
`for_of_start`/`for_of_next` (P15 is the sync iterator phase).

---

## Interaction with the generator machinery

### `async_func_resume` → `JS_CallInternal` → JIT

After P12.2, the call chain is:

```
js_generator_next()
  └─ async_func_resume(ctx, s)
       └─ JS_CallInternal(ctx, JS_MKPTR(JS_TAG_INT,s), ..., JS_CALL_FLAG_GENERATOR)
            └─ [P12.2 hook] detects jit_func → calls JIT directly
                 └─ __jit_f_HASH(ctx, this_val, argc, argv, cpool, var_refs)
                      ├─ retrieves _gf from current_stack_frame's async_func container
                      ├─ dispatch on _gf->resume_idx
                      ├─ ... runs until OP_yield ...
                      └─ returns JS_NewInt32(ctx, FUNC_RET_YIELD)
       └─ async_func_resume checks: JS_TAG_INT and value == FUNC_RET_YIELD → not completed
```

### `async_func_resume` return value handling

```c
/* async_func_resume (P12.2 JIT path) */
JSValue ret = jf(ctx, s->this_val, s->argc, sf->arg_buf, b->cpool, var_refs);
rt->current_stack_frame = sf->prev_frame;
sf->cur_sp = NULL;  /* was set to NULL to mark running; leave NULL */

if (JS_IsException(ret)) {
    s->is_completed = TRUE;
    close_var_refs(rt, b, sf);
    async_func_free_frame(rt, s);
    return ret;
}
if (JS_IsUndefined(ret)) {
    /* OP_return_async path: normal completion */
    ret = sf->cur_sp[-1];
    sf->cur_sp[-1] = JS_UNDEFINED;
    s->is_completed = TRUE;
    close_var_refs(rt, b, sf);
    async_func_free_frame(rt, s);
    return ret;
}
/* yield/await: JS_TAG_INT with FUNC_RET_* value — return as-is */
return ret;
```

---

## Interaction with P13 (closures) and P14 (try/catch)

### P13 (closures inside generators)

Generators can capture variables in closures (`var_refs`).  The JIT already handles
`var_refs` in P13.  For generators, `sf->var_refs` is allocated in `async_func_init`
(via the `p->u.func.var_refs` pointer on the object).  The JIT accesses them via the
`var_refs` parameter which is already passed correctly.

**Key difference**: In generators, `var_refs` persists across yield boundaries inside the
`JSAsyncFunctionState` allocation.  The JIT doesn't need to do anything special — variable
refs are heap-allocated already.

### P14 (try/catch inside generators)

The P14 catch stack (`_catch_stack`) is a C local array.  On yield, the catch stack is
empty at the yield point (you cannot yield from inside a catch handler in the current
QuickJS bytecode).  Verify this invariant with the scanner: if `OP_yield`/`OP_await` is
found inside a try block, verify the catch depth is zero or bail out to interpreter.

If this invariant holds, no spill of the catch stack is needed.

---

## Bytecode pattern reference

For `function* f() { yield 1; yield 2; }`, QuickJS emits:

```
OP_initial_yield
OP_push_i8 1
OP_yield           ; suspends, returns FUNC_RET_YIELD
OP_drop            ; discard .next(v) result
OP_push_i8 2
OP_yield           ; suspends, returns FUNC_RET_YIELD
OP_drop
OP_undefined
OP_return          ; generator exhausted
```

`OP_yield` in the bytecode: pops the yielded value, suspends, and on resume pushes the
`.next(v)` value onto the stack.

---

## Expected impact

| Benchmark | Effect |
|---|---|
| EarleyBoyer | Likely significant — uses generator-style iteration patterns |
| RegExp | No effect (regex engine is C, not JS generators) |
| General JS apps | Opens JIT to large class of previously-excluded functions |

Performance of the JIT-compiled generator body should match plain function JIT performance.
The spill/restore at yield sites adds ~N memory writes per yield (N = live local count),
which is negligible since `yield` itself is the expensive operation.

---

## Estimated effort

| Step | Effort |
|---|---|
| P12.0 `jit_gen_frame` field in `JSAsyncFunctionState` | 0.5 day |
| P12.1 `JSJITGeneratorFrame` struct + alloc/free helpers | 0.5 day |
| P12.2 Hook JIT into `JS_CALL_FLAG_GENERATOR` path | 1 day |
| P12.3 Eligibility + scan changes | 0.5 day |
| P12.4 Signature / preamble `_gf` retrieval | 0.5 day |
| P12.5 Resume dispatch table codegen | 1 day |
| P12.6 `OP_initial_yield` | 0.5 day |
| P12.7 `OP_yield` / `OP_await` + live-var spill analysis | 2 days |
| P12.8 `OP_return_async` | 0.5 day |
| P12.9 `OP_yield_star` fallback helper | 0.5 day |
| P12.10 `OP_for_await_of_*` | 1 day |
| Integration + testing | 1.5 days |
| **Total** | **~10 days** |
