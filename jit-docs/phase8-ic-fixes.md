# Phase 8 — Inline Property Cache Correctness Fixes

Commit `27f5b8e`.  These fixes landed after Phase 6.2 introduced the IC but
before P8.4/P8.5; they are documented here because they unblocked all subsequent
measurement.

---

## Bug 1 — `likely` undefined in generated code

### Symptom

Every function containing `OP_get_field` or `OP_put_field` silently failed to
JIT-compile.  GCC exited with an undefined-symbol error and the `.so` was
discarded; the function continued running in the interpreter.  The IC fast path
was therefore never reached in practice.

### Root cause

Phase 6.2 emitted `likely(js_jit_ic_check(...))` in the generated C.  `likely()`
is defined in `cutils.h` via `__builtin_expect`; it is **not** included in
generated JIT translation units (which only include `quickjs.h` and
`quickjs-jit.h`).

### Fix

Replace every occurrence of `likely(...)` in IC codegen strings with
`js_likely(...)`, which is defined in `quickjs.h` and always available to
generated code.  Three sites in `gen_body()` (OP_get_field, OP_get_field2,
OP_put_field).

---

## Bug 2 — ABA false hit: `prop_count` guard insufficient

### Symptom

The Splay v8bench printed `ERROR Splay: Error: Splay tree has wrong size` —
the IC returned a value from the wrong property slot, corrupting the tree.

### Root cause

The IC entry stored `shape*` + `slot` (property index).  The check was:

```c
return (void *)p->shape == ic->shape;
```

QuickJS reuses freed memory.  When a shape is freed and a new shape of the same
`prop_count` is allocated at the same address (ABA), the check returned a false
hit.  The SplayTree benchmark made this common: `insert()` creates nodes in two
layouts depending on whether `key > root.key`:

- **Shape A**: `[key, value, left, right]` — `left` at slot 2
- **Shape B**: `[key, value, right, left]` — `left` at slot 3

Both have `prop_count = 4`, so any `prop_count`-based guard also fails.

### Fix

Add `atom: uint32_t` to `JSJITICEntry` — the `JSAtom` of the property at the
cached slot.  Fill it in `js_jit_ic_fill_{get,put}` via `prs->atom`.  The check
becomes:

```c
/* pointer match */
if ((void *)p->shape != ic->shape) return 0;
/* ABA guard: atom at the cached slot must match */
if (ic->slot >= (uint32_t)p->shape->prop_count) return 0;
return get_shape_prop(p->shape)[ic->slot].atom == ic->atom;
```

Two shapes with the same address but different property layouts will always
differ in which atom occupies any given slot, so false hits are reliably caught.

---

## Bug 3 — No megamorphic demotion (polymorphic callsite thrash)

### Symptom

For polymorphic callsites (object comes in multiple shapes), every cache miss
called `find_own_property()` — a full hash-chain walk — to re-fill the single
IC slot.  For SplayTree (two-layout nodes), every other property access was a
miss, spending more time in the hash walk than the interpreter would have.

### Fix

Add a `JIT_IC_MEGAMORPHIC` sentinel (`(void*)1`) and demotion logic in
`js_jit_ic_fill_{get,put}`:

```c
if (ic->shape == JIT_IC_MEGAMORPHIC) return 0;   /* skip hash walk */
if (ic->shape != NULL && ic->shape != p->shape) {
    ic->shape = JIT_IC_MEGAMORPHIC;  /* second distinct shape → give up */
    return 0;
}
```

After demotion, `js_jit_ic_check` returns 0 immediately (sentinel detected),
and `js_jit_ic_fill_*` also return 0 without calling `find_own_property`.  The
callsite falls through to `_RT->get_prop` / `_RT->set_prop` every time — the
same path as having no IC at all, with zero hash-lookup overhead.

### Effect on Splay

Splay's `splay_` function accesses `left` and `right` on two-layout nodes →
megamorphic after the second node.  Property reads fall back to the vtable
immediately.  The Splay score improves relative to the pre-fix JIT (which was
doing hash walks on every miss) but remains below the interpreter (expected:
no IC benefit for megamorphic sites).

---

## Also fixed: debug code left in production

The GCC worker had `if (gcc_ok) unlink(c_path)` — the temp `.c` file was only
deleted on success, cluttering `/tmp` on every compilation failure.  Changed to
`unlink(c_path)` unconditionally.

---

## Summary of changes

| File | Change |
|---|---|
| `quickjs-jit.c` | `likely(...)` → `js_likely(...)` in 3 IC codegen strings |
| `quickjs-jit.c` | `unlink(c_path)` always, not only on `gcc_ok` |
| `quickjs-jit.h` | Add `atom: uint32_t` to `JSJITICEntry` with ABA-guard comment |
| `quickjs.c` | `js_jit_ic_check`: add atom guard + megamorphic early-out |
| `quickjs.c` | `js_jit_ic_fill_get`: fill `ic->atom`; add megamorphic demotion |
| `quickjs.c` | `js_jit_ic_fill_put`: fill `ic->atom`; add megamorphic demotion |
| `quickjs.c` | Add `#define JIT_IC_MEGAMORPHIC ((void *)(uintptr_t)1)` |

---

## Bug 4 — `OP_put_var` skipped `JS_UNINITIALIZED` check (implicit globals)

Commit `62b24d8`.

### Symptom

EarleyBoyer threw `TypeError: cannot read property 'appendJSString' of undefined`
when JIT-compiled code ran `p = SC_DEFAULT_OUT` inside `sc_display(o, p)` (called
with one argument).  `p` remained `undefined` after the assignment.

### Root cause

The JIT's `OP_put_var` / `OP_put_var_init` emitted:

```c
{ JSValue *_p = _RT->var_ref_value(var_refs[idx]);
  _FREE(*_p); *_p = _s[--_sp]; }
```

For non-lexical implicit globals (e.g. top-level `var SC_DEFAULT_OUT = ...` in a
loaded script), `*pvalue` is always `JS_UNINITIALIZED` — the interpreter always
routes reads/writes through `JS_GetPropertyInternal` / `JS_SetPropertyInternal` on
the global object.  The JIT bypassed this and wrote directly to `*pvalue`, but
`OP_get_var` still read via `get_var_slow` → `JS_GetPropertyInternal`, so it saw
the old (or absent) value.

### Fix

Added `put_var_slow` vtable entry (`js_jit_op_put_var_slow` in `quickjs.c`) that
mirrors the interpreter's `OP_put_var` slow path.  Updated `OP_put_var` /
`OP_put_var_init` codegen to emit an `JS_TAG_UNINITIALIZED` check before the
direct write; the slow path is taken when the check fires.  The only exception:
`OP_put_var_init` for a lexical variable writes directly (this is the normal `let`
initialisation path, identical to the interpreter's `goto put_var_ok`).

---

## Bug 5 — `js_jit_call` passed unpadded `argc` to JIT callee (P8.3 path)

Commit `62b24d8`.

### Symptom

`sc_display(result)` is called with one argument (`argc=1`) but `sc_display` has
`arg_count=2` (parameters `o` and `p`).  When the caller was JIT-compiled (P8.3
`js_jit_call` path), the JIT callee saw `argc=1`.  `GEN_PUT_ARG(1)` guards with
`if (1 < argc)` → false → silently discards `p = SC_DEFAULT_OUT`.  Subsequent
`GEN_GET_ARG(1)` returns `JS_UNDEFINED` (the `1 < argc` check is false), so `p`
is `undefined` when `p.appendJSString(...)` is called.

A secondary bug: naively padding with `alloca` + shallow copy caused a double-free
on shutdown (both the callee's `_FREE(padded[i])` and the caller's cleanup
decremented the same refcount), manifesting as `Assertion 'list_empty(&rt->gc_obj_list)'
failed`.

### Fix

In `js_jit_call`, when `argc < b->arg_count`: allocate a padded `JSValue` array
via `alloca`, `JS_DupValue` each provided argument into it (so the callee owns its
own reference), fill remaining slots with `JS_UNDEFINED`, call the JIT function
with `n = b->arg_count`, then `JS_FreeValue` all padded slots after the call.  The
DUP/free pattern matches what `JS_CallInternal` does for its `arg_buf` when padding
is needed.
