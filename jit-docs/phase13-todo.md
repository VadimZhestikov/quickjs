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

### Root cause

The interpreter creates closures via:

```c
CASE(OP_fclosure):
    JSValue bfunc = JS_DupValue(ctx, b->cpool[get_u32(pc)]);
    *sp++ = js_closure(ctx, bfunc, var_refs, sf, FALSE);
```

`js_closure` → `js_closure2` builds the inner function's `var_refs[]` by calling
`get_var_ref(ctx, sf, local_idx, is_arg)`, which does:

```c
pvalue = &sf->var_buf[local_idx];   /* live slot for this local */
var_ref->pvalue = pvalue;           /* JSVarRef points INTO the live slot */
```

In the JIT, locals are individual C stack variables (`_jsv_x_3`, `_jsi_y_7`), not a
contiguous `JSValue var_buf[]`.  There is nothing to point `pvalue` at.

**Solution:** shadow arrays.  Captured locals live in `JSValue _cap_buf[var_count]` (an
array on the C stack, indexed by original local index).  All access to captured locals is
redirected to `_cap_buf[i]`.  `JSVarRef`s created at `OP_fclosure` time point their
`pvalue` at `_cap_buf[local_idx]`.  Before returning, the JIT heap-promotes all live
`JSVarRef`s (copies `*pvalue` to `var_ref->value`, redirects `pvalue`), so the C stack
slots can safely go out of scope.

### Data flow for a captured local `x` (local index 3)

```
                    JIT stack frame (C stack)
              ┌────────────────────────────────┐
              │ JSValue _cap_buf[3] = <value>  │ ← live slot
              │ JSVarRef *_sf_vrefs[0] = ──────┼──→ JSVarRef { pvalue=&_cap_buf[3] }
              └────────────────────────────────┘           │
                                                           ↓ (inner closure reads via pvalue)
                                  inner closure's var_refs[i] == the same JSVarRef

On function exit (js_jit_close_caps):
  var_ref->value  = JS_DupValue(*var_ref->pvalue);  /* copy to heap */
  var_ref->pvalue = &var_ref->value;                /* redirect to heap */
  var_ref->is_detached = TRUE;

→ inner closure still works correctly after the JIT frame is gone
```

### Key data structures

- `_cap_buf[var_count]` — shadow array for all locals, indexed by original local_idx.
  Only slots for captured locals are initialised and used.
- `_arg_cap_buf[arg_count]` — shadow array for captured arguments.
- `_sf_vrefs[var_ref_count]` — `JSVarRef*` cache, indexed by `vd->var_ref_idx`.
  Initially all NULL; created lazily on first `OP_fclosure` that captures the local.
  A single JSVarRef is shared across multiple closures capturing the same local.

### Scope and restrictions for P13

- `JS_CLOSURE_LOCAL` and `JS_CLOSURE_ARG` entries: fully supported (shadow + varref).
- `JS_CLOSURE_REF` / `JS_CLOSURE_GLOBAL_REF`: already have a `JSVarRef*` in `var_refs[]`;
  just increment refcount.
- `JS_CLOSURE_GLOBAL_DECL`, `JS_CLOSURE_GLOBAL`, `JS_CLOSURE_MODULE_*`: only appear in
  eval/module code; mark function as unsupported in scan pass.
- More than 64 captured locals or 64 captured args: mark unsupported (bitmask overflow).

---

### Step-by-step plan

---

#### P13.0 — Scan: detect captured locals and args

**File:** `quickjs-jit.c` (`JSJITScanResult`, `js_jit_scan()`, `scan_is_unsupported()`)

**New fields in `JSJITScanResult`:**

```c
int      has_fclosure;           /* 1 if any OP_fclosure/fclosure8 found */
uint64_t captured_local_mask;    /* bit i = local i is captured by ≥1 inner closure */
uint64_t captured_arg_mask;      /* bit i = arg i is captured by ≥1 inner closure */
```

**Changes to `scan_is_unsupported()`:**
Remove `OP_fclosure` and `OP_fclosure8` from the unsupported list.

**Changes to `js_jit_scan()`:**
For each `OP_fclosure K` or `OP_fclosure8 K` encountered:

1. Get inner bytecode: `b_inner = js_jit_cpool_get_fb(b, K)` (new accessor, §P13.1).
   - If `b_inner == NULL` (cpool entry is not a function bytecode): mark unsupported, return -1.

2. For each `cv` in `b_inner->closure_var[0..b_inner->closure_var_count-1]`:
   - `JS_CLOSURE_LOCAL(var_idx=V)`: set bit V of `captured_local_mask`.
     If V ≥ 64: mark unsupported, return -1.
   - `JS_CLOSURE_ARG(var_idx=V)`: set bit V of `captured_arg_mask`.
     If V ≥ 64: mark unsupported, return -1.
   - `JS_CLOSURE_REF` / `JS_CLOSURE_GLOBAL_REF`: no action needed.
   - `JS_CLOSURE_GLOBAL_DECL`, `JS_CLOSURE_GLOBAL`, `JS_CLOSURE_MODULE_*`:
     mark unsupported, return -1.

3. Set `sr->has_fclosure = 1`.

---

#### P13.1 — New accessors in `quickjs.c` / `quickjs-jit.h`

These are needed by both the scan pass and the codegen pass.

**New functions in `quickjs.c`:**

```c
/* Get cpool[idx] as a JSFunctionBytecode pointer.
 * Returns NULL if the entry is not a bytecode function. */
JSFunctionBytecode *js_jit_cpool_get_fb(JSFunctionBytecode *b, int cpool_idx);

/* Inner function's closure_var entry accessors */
int js_jit_fb_get_inner_cv_type(JSFunctionBytecode *b_inner, int cv_idx);
    /* returns JSClosureTypeEnum value */
int js_jit_fb_get_inner_cv_var_idx(JSFunctionBytecode *b_inner, int cv_idx);
    /* returns cv->var_idx */

/* Outer function's per-local/arg accessors for captured variable info */
int js_jit_fb_get_var_ref_count(JSFunctionBytecode *b);
    /* returns b->var_ref_count */
int js_jit_fb_get_local_var_ref_idx(JSFunctionBytecode *b, int local_idx);
    /* returns b->vardefs[b->arg_count + local_idx].var_ref_idx */
int js_jit_fb_get_arg_var_ref_idx(JSFunctionBytecode *b, int arg_idx);
    /* returns b->vardefs[arg_idx].var_ref_idx */
int js_jit_fb_is_local_captured(JSFunctionBytecode *b, int local_idx);
    /* returns b->vardefs[b->arg_count + local_idx].is_captured */
int js_jit_fb_is_arg_captured(JSFunctionBytecode *b, int arg_idx);
    /* returns b->vardefs[arg_idx].is_captured */
```

**Declarations in `quickjs-jit.h`:** add all of the above after the existing accessor
block (`js_jit_fb_get_closure_var_count`, etc.).

---

#### P13.2 — New runtime helpers in `quickjs.c`

Three standalone functions (not vtable entries — they are called from generated C
which can see the `quickjs-jit.h` declarations and is linked against `quickjs.o`).

**`js_jit_make_var_ref`** — create a JSVarRef pointing to a live slot:

```c
/* Creates a JSVarRef with refcount=1 and pvalue=slot.
 * The caller is the sole initial owner.
 * Returns NULL on OOM (sets exception on ctx). */
JSVarRef *js_jit_make_var_ref(JSContext *ctx, JSValue *slot);
```

Implementation:
```c
JSVarRef *js_jit_make_var_ref(JSContext *ctx, JSValue *slot) {
    JSVarRef *vr = js_malloc(ctx, sizeof(*vr));
    if (!vr) return NULL;
    vr->header.ref_count = 1;
    add_gc_object(ctx->rt, &vr->header, JS_GC_OBJ_TYPE_VAR_REF);
    vr->is_detached = FALSE;
    vr->is_lexical  = FALSE;
    vr->is_const    = FALSE;
    vr->var_ref_idx  = 0;     /* unused — we manage via _sf_vrefs[], not via sf */
    vr->stack_frame  = NULL;  /* NULL marks a JIT-owned var_ref in free_var_ref */
    vr->pvalue       = slot;
    return vr;
}
```

**Important:** `stack_frame = NULL` prevents `free_var_ref` from touching `sf->var_refs[]`
when the var_ref is freed while still attached. This is safe because we manage detachment
ourselves via `js_jit_close_caps` before the C stack frame goes out of scope.

Note: verify that `free_var_ref` handles `stack_frame == NULL` gracefully. If not, add a
NULL guard in `free_var_ref` (one-line change in quickjs.c).

**`js_jit_close_caps`** — heap-promote live var_refs before function exit:

```c
/* For each non-NULL vrefs[i]: copy *pvalue to var_ref->value, redirect pvalue,
 * set is_detached=TRUE.  Does NOT change refcounts.
 * Call on ALL exit paths (normal return and exception path) before the C frame
 * containing _cap_buf / _arg_cap_buf goes out of scope. */
void js_jit_close_caps(JSContext *ctx, JSVarRef **vrefs, int n);
```

Implementation:
```c
void js_jit_close_caps(JSContext *ctx, JSVarRef **vrefs, int n) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    for (int i = 0; i < n; i++) {
        if (vrefs[i] && !vrefs[i]->is_detached) {
            vrefs[i]->value  = JS_DupValueRT(rt, *vrefs[i]->pvalue);
            vrefs[i]->pvalue = &vrefs[i]->value;
            vrefs[i]->is_detached = TRUE;
        }
    }
}
```

**`js_jit_create_closure`** — build the closure function object from pre-built var_refs:

```c
/* Creates a closure function object.  bfunc is consumed (freed on error or stored).
 * pre_vrefs[0..n_vrefs-1] are pre-filled JSVarRef* (refcounts already set by caller).
 * On success, takes ownership of pre_vrefs entries (mallocs an internal copy).
 * On error, releases pre_vrefs entries via free_var_ref and returns JS_EXCEPTION. */
JSValue js_jit_create_closure(JSContext *ctx, JSValue bfunc,
                               JSVarRef **pre_vrefs, int n_vrefs);
```

Implementation mirrors `js_closure` / `js_closure2` but skips the LOCAL/ARG var_ref
allocation (already done by the caller):
1. `b_inner = JS_VALUE_GET_PTR(bfunc)` (consumed)
2. `func_obj = JS_NewObjectClass(ctx, func_kind_to_class_id[b_inner->func_kind])`
3. If n_vrefs > 0: `malloc` a copy of `pre_vrefs` and store in `p->u.func.var_refs`.
4. `p->u.func.function_bytecode = b_inner`
5. `js_function_set_properties(ctx, func_obj, b_inner->func_name, ...)`
6. If `b_inner->func_kind & JS_FUNC_GENERATOR`: set up generator prototype (copy from
   `js_closure`).
7. On any error: release `pre_vrefs` entries, free func_obj, return JS_EXCEPTION.

---

#### P13.3 — Type inference: force captured locals/args to `JIT_T_JSVAL`

**File:** `quickjs-jit.c` (`jit_infer_types()`)

After computing `local_type[]`, apply P13 override:

```c
if (sr.has_fclosure) {
    for (int i = 0; i < var_count; i++)
        if ((sr.captured_local_mask >> i) & 1)
            local_type[i] = JIT_T_JSVAL;  /* no typed opt for captured locals */
    /* captured args are not in local_type[]; handled by separate redirect in gen_body */
}
```

This ensures type-specialised access (`_jsi_`, `_jsd_`) is never emitted for captured
locals, so all generated get_loc/put_loc for those locals use the `_jsv_` / `_cap_buf`
path uniformly.

---

#### P13.4 — Preamble: shadow arrays and VarRef cache

**File:** `quickjs-jit.c` (`gen_preamble()`)

When `sr.has_fclosure`, emit after the existing local declarations:

```c
/* --- P13: shadow storage for captured locals --- */
/* _cap_buf[local_idx]: canonical slot for captured locals; JSVarRefs point here. */
/* Only the captured slots are used; others are never touched. */
JSValue _cap_buf[VAR_COUNT];
/* Explicit per-slot initialisation (safe for GC even before first use): */
_cap_buf[I0] = JS_UNDEFINED;   /* for each I in captured_local_mask */
_cap_buf[I1] = JS_UNDEFINED;
...

/* _sf_vrefs[var_ref_idx]: JSVarRef* cache, shared across all OP_fclosure calls.
 * Multiple closures capturing the same local share one JSVarRef (refcount > 1). */
JSVarRef *_sf_vrefs[VAR_REF_COUNT];
memset(_sf_vrefs, 0, sizeof(_sf_vrefs));

/* --- P13: shadow storage for captured args --- */
/* (only emitted if captured_arg_mask != 0) */
JSValue _arg_cap_buf[ARG_COUNT];
/* Init captured arg slots with a DUP of the incoming argv value: */
_arg_cap_buf[J0] = (J0 < argc) ? _DUP(argv[J0]) : JS_UNDEFINED;
...
```

**Why explicit initialisation instead of memset:** `JS_UNDEFINED` is
`JS_MKVAL(JS_TAG_UNDEFINED, 0)`.  On 64-bit targets the tag lives in the high word —
`memset(0)` would produce `JS_TAG_INT` with value 0, not `JS_UNDEFINED`.  Explicit
`_cap_buf[i] = JS_UNDEFINED` is required.

---

#### P13.5 — Redirect local and argument access for captured variables

**File:** `quickjs-jit.c` (`gen_body()`, macros `GEN_GET_LOC`, `GEN_PUT_LOC`, etc.)

For **captured locals** (bit `i` set in `captured_local_mask`), every read/write must go
through `_cap_buf[i]` instead of the named `_jsv_name_i` variable.

Since type inference (P13.3) forces `JIT_T_JSVAL` for captured locals, the only codegen
paths that fire are the JSVAL paths. But we must still redirect the LNAME macro:

```c
/* In gen_body, wherever LNAME(idx) is used for get/put/set/add loc: */
#define LNAME_OR_CAP(idx) \
    (((sr.captured_local_mask >> (idx)) & 1) ? \
     /* use _cap_buf slot */ : /* use existing LNAME(idx) */)
```

Specifically, for each of these opcode handlers, add a captured-local branch:

| Opcode | Redirect |
|---|---|
| `OP_get_loc`, `OP_get_loc_check`, `OP_get_loc8`, `OP_get_loc0..3`, `OP_get_loc_checkthis` | `_tsv{d} = _DUP(_cap_buf[i])` |
| `OP_put_loc`, `OP_put_loc_check`, `OP_put_loc8`, `OP_put_loc0..3` | `_FREE(_cap_buf[i]); _cap_buf[i] = _tsv{d-1}` |
| `OP_set_loc`, `OP_set_loc8`, `OP_set_loc0..3` | `_FREE(_cap_buf[i]); _cap_buf[i] = _DUP(_tsv{d-1})` |
| `OP_add_loc` | read/write `_cap_buf[i]` instead of `_jsv_name_i` |
| `OP_inc_loc`, `OP_dec_loc` | similarly |

For **captured args** (bit `j` set in `captured_arg_mask`):
- `OP_get_arg(j)`: `_tsv{d} = _DUP(_arg_cap_buf[j])`
- `OP_put_arg(j)`: `_FREE(_arg_cap_buf[j]); _arg_cap_buf[j] = _tsv{d-1}`

The `_jai_` / `_aim` fast path for integer arguments must be bypassed for captured args
(emit only the `_DUP(_arg_cap_buf[j])` form, never the `_jai_` read).

---

#### P13.6 — `gen_body()`: codegen for `OP_fclosure` / `OP_fclosure8`

**File:** `quickjs-jit.c` (`gen_body()`)

At codegen time, for `OP_fclosure cpool_idx` with inner function `b_inner`:

1. Read inner function's `closure_var[0..n-1]` to build the generated code.

2. For each `closure_var[i] = cv`:
   - `JS_CLOSURE_LOCAL(var_idx=V)`:
     - `var_ref_idx = js_jit_fb_get_local_var_ref_idx(b, V)`
     - Emit:
       ```c
       if (!_sf_vrefs[VRI]) {
           _sf_vrefs[VRI] = js_jit_make_var_ref(ctx, &_cap_buf[V]);
           if (!_sf_vrefs[VRI]) goto _ex;
       } else {
           _sf_vrefs[VRI]->header.ref_count++;
       }
       _vr_Npc[i] = _sf_vrefs[VRI];
       ```
   - `JS_CLOSURE_ARG(var_idx=V)`:
     - `var_ref_idx = js_jit_fb_get_arg_var_ref_idx(b, V)`
     - Same pattern but `&_arg_cap_buf[V]`
   - `JS_CLOSURE_REF(var_idx=V)` or `JS_CLOSURE_GLOBAL_REF(var_idx=V)`:
     - Emit: `var_refs[V]->header.ref_count++; _vr_Npc[i] = var_refs[V];`

3. Emit the closure creation:
   ```c
   JSVarRef *_vr_Npc[M];   /* M = b_inner->closure_var_count, N = pc */
   /* ... filled above ... */
   JSValue _bfunc = JS_DupValue(ctx, cpool[CPOOL_IDX]);
   _sp = d;                 /* exception safety: all caps still live */
   JSValue _cl = js_jit_create_closure(ctx, _bfunc, _vr_Npc, M);
   _CHK(_cl);
   _tsv{d} = _cl; _sp = d+1;
   ```

4. Type / gen-time stack tracking: push `JIT_T_JSVAL` (closure is always a JSValue).

**Refcount protocol:**
- New `_sf_vrefs[VRI]` created by `js_jit_make_var_ref`: starts at refcount=1, this IS the
  closure's ownership reference.  Do NOT increment further.
- Existing `_sf_vrefs[VRI]` (second closure capturing same local): `ref_count++` to add
  this closure's reference.
- `JS_CLOSURE_REF/GLOBAL_REF`: similarly `ref_count++` since `var_refs[V]` already has
  a reference owned by the outer function's own closure capture.
- `js_jit_create_closure` takes these counts as-is and stores them in `p->func.var_refs[]`.
  When the inner function object is eventually freed, each var_ref's refcount is decremented
  by one per entry.

---

#### P13.7 — Function exit: close caps and free shadow storage

**File:** `quickjs-jit.c` (all return paths in `gen_body()`)

Before **every** return — both `OP_return`, `OP_return_undef`, and the `_ex:` exception
handler — emit the following (when `has_fclosure`):

```c
/* Heap-promote all live JSVarRefs so inner closures remain valid after this frame exits */
js_jit_close_caps(ctx, _sf_vrefs, VAR_REF_COUNT);

/* Release the local references to captured values */
_FREE(_cap_buf[I0]);   /* for each captured local */
_FREE(_cap_buf[I1]);
...
_FREE(_arg_cap_buf[J0]);   /* for each captured arg */
...
```

The existing footer `_FREE(_jsv_name_Ii)` for captured locals is a no-op (the named
variable was never written — it stays `JS_UNDEFINED` from preamble init) and can be
left in place.

**Order matters:** `js_jit_close_caps` FIRST (reads `*pvalue = _cap_buf[i]` to make
the DupValue copy), then `_FREE(_cap_buf[i])` releases the reference.  Reversing this
order would free the value before copying it into `var_ref->value`.

**Exception handler additional requirement:** The `_ex:` handler must also call
`js_jit_close_caps` and free `_cap_buf` entries, even if some of them were never written
(JS_UNDEFINED is a no-op for `_FREE`, so this is always safe).

---

#### P13.8 — Remove from `scan_is_unsupported`, add tests

**File:** `quickjs-jit.c` — confirmed already done by P13.0.

**Tests** (`jit-tests/js/test_jit_p13_closures.js`):
- Basic counter closure: `function makeCounter() { let n=0; return ()=>n++; }`
- Multiple closures sharing a var: `function shared() { let x=0; return [()=>x++, ()=>x]; }`
- Arg capture: `function makeAdder(x) { return y => x+y; }`
- Captured local written before closure: `function f() { let x=10; x=20; return ()=>x; }`
- Transitive capture: outer → middle (JIT) → inner (interpreter) chain
- JIT compiles both the outer function AND the returned closure if the closure itself is
  hot enough (two independent JIT compilations, no shared state issues)

---

### Open questions / edge cases

1. **`free_var_ref` null stack_frame**: `free_var_ref` accesses `sf->var_refs[]` when
   `!is_detached`. With `stack_frame = NULL`, this would be a null-deref.  Fix: add
   `if (sf)` guard in `free_var_ref` before line 5788. Alternatively, always set
   `is_detached = FALSE` ONLY when the var_ref is NOT yet detached by `close_caps` — the
   var_ref should be detached by the time any closure that captured it could be freed.
   This is guaranteed as long as `close_caps` is called on ALL exit paths.

2. **GC safety during `OP_fclosure`**: `js_jit_create_closure` calls `js_malloc` which can
   trigger GC.  At that point, `_cap_buf` values are live (held by the shadow array, which
   has refcounts ≥ 1 from DUP in preamble/put_loc).  GC-correctness is maintained by
   reference counting, same as for all other JIT locals.

3. **Multiple `OP_fclosure` in one function**: all share the same `_sf_vrefs[]` and
   `_cap_buf[]`.  The cache ensures each captured local gets exactly one `JSVarRef`, with
   the refcount equal to the number of inner closures that captured it.

4. **`OP_close_loc` opcode** (if present): QuickJS emits `OP_close_loc` for `let`
   variables that go out of scope early (inside a block). Check whether `OP_close_loc`
   appears in functions with `OP_fclosure`; if so, add handling (call `close_caps` for the
   specific var_ref_idx only). Add to `scan_is_unsupported()` if not handled in P13.

5. **`OP_put_loc_check_init`**: initialises a `let` variable (TDZ → value). For captured
   `let` locals, this must write `_cap_buf[i]` and also handle the TDZ check. Existing
   codegen handles TDZ; just add the captured-local redirect.

---

### Estimated effort: ~5 days

| Sub-step | Effort |
|---|---:|
| P13.0 scan + P13.1 accessors | 1 day |
| P13.2 runtime helpers (make_var_ref, close_caps, create_closure) | 1 day |
| P13.3 type inference + P13.4 preamble | 0.5 day |
| P13.5 get/put/set/add/inc/dec_loc redirects | 1 day |
| P13.6 OP_fclosure codegen | 1 day |
| P13.7 exit paths + P13.8 tests | 0.5 day |

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

## Phase 16 — Property Deletion (`OP_delete`, `OP_delete_var`) ✅ DONE (2026-04-05)

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

## Phase 17 — Spread and Apply (`OP_apply`, `OP_apply_eval`) ✅ DONE (2026-04-05)

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
| P13 ✅ | Closure creation | `OP_fclosure`, `OP_fclosure8`, `OP_set_name` | ~5 days | Functions that define inner functions/closures |
| P14 ✅ | try/catch/finally | `OP_catch`, `OP_gosub`, `OP_nip_catch`, `OP_ret` | ~4 days | Functions with exception handling |
| P15 ✅ | Iterators / for-of | `OP_for_in_start` … `OP_iterator_call` | ~6 days | Loops over objects, arrays, generators |
| P16 ✅ | Property deletion | `OP_delete`, `OP_delete_var` | ~1 day | `delete obj.prop` patterns |
| P17 ✅ | Spread / apply | `OP_apply`, `OP_apply_eval` | ~1 day | `f(...args)`, `f.apply(this, args)` |
| P18 | with-statement | `OP_with_*` | — | Not planned |
| **Total** | | | **~17 days** | |

### Recommended order

1. **P16 + P17 first** — 2 days total, zero new infrastructure, pure unlocking value.
2. **P13** — highest real-world impact (closures everywhere in modern JS).
3. **P14** — try/catch needed for most non-trivial functions.
4. **P15** — iterators needed for for-of, generators (complements P12).
5. **P12 (generators)** — complete the async/generator story.
6. **P18** — not planned.
