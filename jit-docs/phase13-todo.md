# Phase 13+ — Remaining Unsupported Opcode Groups

## Overview

Five opcode groups are currently excluded from the JIT by `scan_is_unsupported()` or the
eligibility check in `js_jit_is_eligible()`.  Each group requires different infrastructure.
This document plans them in order of implementation complexity and real-world impact.

```
Group                 Opcodes                              Phase  Effort
──────────────────────────────────────────────────────────────────────────
Closure creation      OP_fclosure, OP_fclosure8            P13    ~5 days
try/catch/finally     OP_catch, OP_gosub, OP_nip_catch     P14    ~4 days
Iterators / for-of    OP_for_in_start … OP_iterator_call   P15    ~6 days
Property deletion     OP_delete, OP_delete_var             P16    ~1 day
Spread / apply        OP_apply, OP_apply_eval              P17    ~1 day
with-statement        OP_with_*                            P18    not planned
```

---

## Phase 13 — Closure Creation (`OP_fclosure`, `OP_fclosure8`)

### Why it's excluded

The interpreter creates closures via:

```c
CASE(OP_fclosure):
    JSValue bfunc = JS_DupValue(ctx, b->cpool[get_u32(pc)]);
    *sp++ = js_closure(ctx, bfunc, var_refs, sf, FALSE);
```

`js_closure` needs two things the JIT currently doesn't provide:
- `var_refs` — the current function's own captured-variable array (already passed to JIT)
- `sf` — a live `JSStackFrame*` so that `get_var_ref(sf, idx)` can produce `JSVarRef`
  pointers that reference the **current function's locals**

The `sf` problem is the root issue: in the interpreter `sf->var_buf[i]` is the live slot
for local variable `i`, and a `JSVarRef` is a pointer into it.  In the JIT, locals are C
stack variables (`_li_i`, `_ld_y`, `JSValue _lv_acc`) — there is no `JSValue var_buf[]`
to point into.

### Root cause: captured variable aliasing

When an inner closure captures `x` from the outer function, the interpreter creates a
`JSVarRef` whose `pvalue` points to `sf->var_buf[x_idx]`.  While the outer function is
running, both the function body and the inner closure read/write `x` through that pointer.
When the outer function returns, `close_var_refs()` copies the value to the `JSVarRef`'s
own storage and makes `pvalue` point there instead (heap-promotion).

The JIT must replicate this in C:

1. Maintain a `JSValue _cap[N]` **shadow array** on the C stack (or heap) for any local
   that may be captured by an inner closure.
2. Create `JSVarRef`s whose `pvalue = &_cap[i]`, so the inner closure reads/writes through
   that pointer.
3. Keep `_cap[i]` in sync with the corresponding typed local (`_li_i`, `_ld_y`) throughout
   the function body (or use `_cap[i]` directly for captured vars, forgoing the typed
   optimization for those vars).
4. On function exit, call the equivalent of `close_var_refs` to heap-promote.

### Step-by-step plan

#### P13.0 — Scan: detect captured-variable indices

**Files:** `quickjs-jit.c` (`scan_body()`)

Remove `OP_fclosure` / `OP_fclosure8` from `scan_is_unsupported()`.  Instead, during the
scan pass, walk the inner bytecode's `closure_var[]` array to collect which of the outer
function's local indices are captured.  Store as a bitmask or index list in
`JSJITScanResult`:

```c
uint64_t captured_var_mask;   /* bit i = local i is captured by a closure */
int      has_fclosure;        /* 1 if OP_fclosure appears */
```

If `captured_var_mask` would exceed 64 bits (>64 locals captured), fall back to excluding
the function for now.

#### P13.1 — Emit `_cap[]` shadow array in preamble

**Files:** `quickjs-jit.c` (`gen_preamble()`)

For functions with `has_fclosure`, emit a shadow array for each captured local:

```c
JSValue _cap[N];   /* shadow slots for captured locals: indices { i₀, i₁, … } */
memset(_cap, 0, sizeof(_cap));
```

**Important**: captured locals must NOT be typed (`_li_`, `_ld_`) — they are always
represented as `JSValue _cap[k]` and accessed through the pointer in the `JSVarRef`.
The type inference pass (`jit_infer_types`) must mark captured locals as `JIT_T_JSVAL`
to prevent the typed-local optimization being applied to them.

#### P13.2 — JIT closure helper

**Files:** `quickjs.c` or `quickjs-jit.c`, `quickjs-jit.h`

Add a new vtable entry or standalone helper:

```c
/* Creates a closure over inner bytecode bfunc.
 * cap: array of JSValue* pointers to the outer JIT function's _cap slots.
 * n_cap: number of captured variables.
 * outer_var_refs: the outer JIT function's own var_refs (for transitive captures).
 */
JSValue js_jit_make_closure(JSContext *ctx,
                             JSValue bfunc,
                             JSValue **cap_ptrs,    /* &_cap[k] per captured var */
                             int n_cap,
                             JSVarRef **outer_var_refs);
```

Internally, this calls `js_closure()` after constructing a temporary array of `JSVarRef`s
that each hold `pvalue = cap_ptrs[k]`.  The `JSVarRef`s are reference-counted and owned by
the resulting closure object.

#### P13.3 — Generated code for `OP_fclosure`

**Files:** `quickjs-jit.c` (`gen_body()`)

```c
/* OP_fclosure <cpool_idx> */
JSValue _bfunc = JS_DupValue(ctx, _cpool[IDX]);
JSValue *_cap_ptrs[] = { &_cap[0], &_cap[1], /* ... */ };
JSValue _closure = js_jit_make_closure(ctx, _bfunc, _cap_ptrs, N_CAP, _var_refs);
JS_FreeValue(ctx, _bfunc);
if (JS_IsException(_closure)) goto _exception;
_tsv0 = _closure;
```

The `_cap_ptrs[]` array is generated statically per opcode based on the captured-variable
mapping collected in P13.0.

#### P13.4 — Close captured vars on function exit

**Files:** `quickjs-jit.c` (`gen_body()`, all return paths)

Before every `return` in the generated function, emit:

```c
/* Heap-promote captured vars (equivalent to close_var_refs) */
js_jit_close_caps(ctx, _cap, N_CAP, _my_var_refs);
```

Where `_my_var_refs` is a new array (alongside `_var_refs`) holding the `JSVarRef*`s for
this function's own captured-out locals.  `js_jit_close_caps` copies each `_cap[k]` into
its `JSVarRef->value` and sets `pvalue = &var_ref->value`.

This must also be emitted on exception paths (the `_exception:` label handler).

### Expected impact

Enables JIT compilation of any function that returns or passes inner functions — closures,
module patterns, functional programming idioms.  Currently any such function falls back to
interpreter.

### Estimated effort: ~5 days

---

## Phase 14 — try/catch/finally (`OP_catch`, `OP_gosub`, `OP_nip_catch`)

### Why it's excluded

Exception handling in QuickJS bytecode uses three opcodes:

| Opcode | Role |
|---|---|
| `OP_catch` | Push an exception handler frame; jump target for exception dispatch |
| `OP_gosub` | `finally`-block entry: push return address, jump to finally body |
| `OP_nip_catch` | Pop exception handler frame after normal flow through try block |

The interpreter's exception mechanism uses `setjmp`/`longjmp` (or equivalent `goto`-based
dispatch via `exception:` label) and a per-frame handler stack.  The JIT's `_CHK` macro
currently propagates exceptions immediately (`goto _exception`) with no catch.

### Plan

#### P14.0 — Exception handler stack in generated function

Emit a local exception-handler stack:

```c
JSJITExcHandler _exc_stack[MAX_HANDLERS];
int _exc_depth = 0;
```

Where `JSJITExcHandler` is:
```c
typedef struct { int resume_pc; } JSJITExcHandler;
```

#### P14.1 — `OP_catch` — push handler

```c
/* OP_catch <target_pc> */
_exc_stack[_exc_depth++].resume_pc = TARGET_PC_LABEL_IDX;
/* push JS_UNDEFINED (placeholder for exception value) */
_tsv0 = JS_UNDEFINED;
```

On exception, the `_exception:` label checks `_exc_depth > 0` and jumps to the handler:

```c
_exception:
if (_exc_depth > 0) {
    int _hpc = _exc_stack[--_exc_depth].resume_pc;
    _tsv0 = JS_GetException(ctx);   /* push exception value onto stack */
    switch (_hpc) {
    case 0: goto _catch_L42;
    case 1: goto _catch_L87;
    /* ... */
    }
}
return JS_EXCEPTION;
```

#### P14.2 — `OP_nip_catch` — pop handler on normal flow

```c
/* OP_nip_catch */
if (_exc_depth > 0) _exc_depth--;
/* also pops the exception-value stack slot */
```

#### P14.3 — `OP_gosub` / `OP_ret` — finally blocks

`OP_gosub` pushes a return address (as a JSValue int) and jumps to the finally block.
`OP_ret` at the end of finally pops that address and jumps back.  In generated C:

```c
/* OP_gosub <finally_label> */
_tsv_ret_addr = JS_NewInt32(ctx, RETURN_LABEL_IDX);
goto _finally_L99;

_finally_return_N:   /* OP_ret dispatches here */
```

### Estimated effort: ~4 days

---

## Phase 15 — Iterators and for-in/for-of

### Why it's excluded

`OP_for_in_start` / `OP_for_of_start` create iterator objects and push them onto the value
stack, where they persist **across loop headers**.  This violates the P9.2 assumption that
the value stack is empty at every basic-block boundary, breaking the stackless IR.

Additionally, `OP_iterator_get_value_done` and similar opcodes require complex protocol
interaction with the iterator object.

### Plan

#### P15.0 — Detect for-in/for-of in scan; allocate named iterator slots

Instead of treating the iterator as a transient stack value, allocate it as a named
`JSValue _iter_N` local that persists across the loop:

```c
JSValue _iter_0 = JS_UNDEFINED;   /* for-in/of object, lives across loop */
```

The scan pass detects `OP_for_in_start` / `OP_for_of_start` and assigns each a unique
`_iter_N` slot.

#### P15.1 — `OP_for_in_start` / `OP_for_of_start`

```c
/* OP_for_in_start */
_iter_0 = _RT->for_in_start(ctx, <object>);
if (JS_IsException(_iter_0)) goto _exception;
```

#### P15.2 — `OP_for_in_next` / `OP_for_of_next`

```c
/* OP_for_in_next — pushes (key, done_flag) */
JSValue _key;
int _done;
if (_RT->for_in_next(ctx, &_iter_0, &_key, &_done)) goto _exception;
_tsv0 = _key;
_tsv1 = JS_NewBool(ctx, _done);
```

#### P15.3 — Iterator protocol opcodes

`OP_iterator_check_object`, `OP_iterator_get_value_done`, `OP_iterator_close`,
`OP_iterator_next`, `OP_iterator_call`, `OP_for_await_of_next` — each gets a vtable entry
delegating to the existing interpreter helper.

### Estimated effort: ~6 days

---

## Phase 16 — Property Deletion (`OP_delete`, `OP_delete_var`)

### Why it's excluded

`OP_delete obj.prop` and `OP_delete_var name` require property-system access (`JS_DeleteProperty`)
plus special handling of strict mode errors.  They were excluded as rare and complex.

### Plan

These are the simplest group to add — both map directly to existing runtime functions:

```c
/* OP_delete — delete obj[key] */
int _res = JS_DeleteProperty(ctx, <obj>, <key>, JS_PROP_THROW_STRICT);
if (_res < 0) goto _exception;
_tsv0 = JS_NewBool(ctx, _res);

/* OP_delete_var — delete global variable by name */
int _res = JS_DeleteProperty(ctx, ctx->global_obj, <atom>, 0);
if (_res < 0) goto _exception;
_tsv0 = JS_NewBool(ctx, _res);
```

No new infrastructure needed.  Remove from `scan_is_unsupported()` and add two cases to
`gen_body()`.

### Estimated effort: ~1 day

---

## Phase 17 — Spread and Apply (`OP_apply`, `OP_apply_eval`)

### Why it's excluded

`OP_apply` implements `f.apply(this, args_array)` and `f(...spread)`.  The argument count
is dynamic (determined at runtime from the array length), which doesn't fit the fixed
`argc` model the JIT assumes.

### Plan

Both opcodes can delegate entirely to the vtable — no fast path needed initially:

```c
/* OP_apply — f.apply(this, args) */
_tsv0 = _RT->apply(ctx, <func>, <this>, <args_array>);
if (JS_IsException(_tsv0)) goto _exception;

/* OP_apply_eval — eval(...) with spread */
_tsv0 = _RT->apply_eval(ctx, <args_array>);
if (JS_IsException(_tsv0)) goto _exception;
```

Remove from `scan_is_unsupported()`.  Add vtable entries wrapping the existing
`js_function_apply` and `js_spread_call` implementations in `quickjs.c`.

### Estimated effort: ~1 day

---

## Phase 18 — `with`-statement (`OP_with_*`) — NOT PLANNED

The `with` statement (`with (obj) { ... }`) introduces **dynamic variable scoping**:
variable lookups must first check `obj` before falling back to the scope chain.  This is
fundamentally incompatible with the JIT's static variable model — the JIT assumes that each
variable reference maps to a known local, argument, or closure slot.

`with` is forbidden in strict mode, deprecated in ES5, and absent from all modern code.
None of the supported benchmarks use it.  **This group will remain excluded indefinitely.**

---

## Summary table

| Phase | Group | Opcodes | Effort | Unlock |
|---|---|---|---:|---|
| P13 | Closure creation | `OP_fclosure`, `OP_fclosure8` | ~5 days | Functions that define inner functions/closures |
| P14 | try/catch/finally | `OP_catch`, `OP_gosub`, `OP_nip_catch` | ~4 days | Functions with exception handling |
| P15 | Iterators / for-of | `OP_for_in_start` … `OP_iterator_call` | ~6 days | Loops over objects, arrays, generators |
| P16 | Property deletion | `OP_delete`, `OP_delete_var` | ~1 day | `delete obj.prop` patterns |
| P17 | Spread / apply | `OP_apply`, `OP_apply_eval` | ~1 day | `f(...args)`, `f.apply(this, args)` |
| P18 | with-statement | `OP_with_*` | — | Not planned |
| **Total** | | | **~17 days** | |

### Recommended order

1. **P16 + P17 first** — 2 days total, zero new infrastructure, pure unlocking value.
2. **P13** — highest real-world impact (closures everywhere in modern JS).
3. **P14** — try/catch needed for most non-trivial functions.
4. **P15** — iterators needed for for-of, generators (complements P12).
5. **P12 (generators)** — complete the async/generator story.
6. **P18** — not planned.
