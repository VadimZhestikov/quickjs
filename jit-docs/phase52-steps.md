# Phase 52 — Warm-IC Recompile: Skip Refcount Check for INT put_var_ref / set_var_ref

## Background

P48–P51 added warm-IC recompile for property reads/writes, array accesses, get_var_ref,
and OP_add.  P52 extends the same infrastructure to `OP_put_var_ref*` and
`OP_set_var_ref*` — the closure-variable write opcodes.

When the JIT gen_st of the written value is already INT (e.g. after a warm P51 add or a
P49 get_var_ref), the new value is known to be an immediate JSValue with no refcount.
However the cold-compiled write still emits:

```c
if (js_unlikely(JS_VALUE_HAS_REF_COUNT(*_vrp))) _RT->free_value(ctx, *_vrp);
```

…to handle the case where the old cell value has a refcount (e.g. a string or object
that was stored previously).  In a purely numeric hot closure (counter, accumulator,
Fibonacci) the cell holds an INT from the very first call, so the `JS_VALUE_HAS_REF_COUNT`
branch is never taken — it is pure overhead.

P52 observes the old cell tag in the BSS hint array during the cold phase and, when the
warm recompile sees tag == JS_TAG_INT (== 0), emits a bare assignment with no branch:

```c
*_vrp = JS_MKVAL(JS_TAG_INT, (int32_t)_ti);
```

---

## Architecture

### `__jit_vt_HASH[]` array extension

P52 appends `n_pv` entries at the end of the existing hint array:

```
__jit_vt_HASH[0 .. n_gf-1]                                          ← get_field (P45b)
__jit_vt_HASH[n_gf .. +n_ae-1]                                      ← get_array_el (P46)
__jit_vt_HASH[n_gf+n_ae .. +n_pf-1]                                 ← put_field (P48)
__jit_vt_HASH[n_gf+n_ae+n_pf .. +n_vr-1]                           ← get_var_ref* (P49)
__jit_vt_HASH[n_gf+n_ae+n_pf+n_vr .. +n_pa-1]                      ← put_array_el (P50)
__jit_vt_HASH[n_gf+n_ae+n_pf+n_vr+n_pa .. +n_ad*2-1]               ← OP_add operands (P51)
__jit_vt_HASH[n_gf+n_ae+n_pf+n_vr+n_pa+n_ad*2 .. +n_pv-1]         ← put/set_var_ref (P52)
```

One slot per put/set_var_ref site: slot `pv_idx` holds the observed tag of the *old*
value in the closure cell immediately before the overwrite.

`n_pv` counts ALL `OP_put_var_ref*` and `OP_set_var_ref*` opcodes (including those
whose new value is JSVAL at gen_st time — those sites never read the hint and leave
the BSS zero, which is harmless).

`n_pv` is stored in `JSFunctionBytecode.jit_n_pv` (uint16_t).

---

## Opcodes covered

- `OP_put_var_ref` / `OP_put_var_ref_check` / `OP_put_var_ref_check_init`
- `OP_put_var_ref0` / `OP_put_var_ref1` / `OP_put_var_ref2` / `OP_put_var_ref3`
- `OP_set_var_ref` / `OP_set_var_ref0` / `OP_set_var_ref1` / `OP_set_var_ref2` / `OP_set_var_ref3`

`OP_set_var_ref*` differs from `OP_put_var_ref*` in that the written value remains on
the stack (peek semantics, no `_sp--`).  The P52 optimisation applies identically;
only the `_sp=` emission is omitted in the warm path.

---

## Cold path

When `vt_hints == NULL` (first compile), the `GEN_PUT_VR` macro (INT new-value branch)
reads the old cell tag into the hint slot and then performs the full refcount check:

```c
{ JSValue _nv = JS_MKVAL(JS_TAG_INT, (int32_t)_ti{d-1});
  __jit_vt_HASH[base + pv_idx] = (uint8_t)JS_VALUE_GET_TAG(*_vrpN);  /* P52 record */
  if (js_unlikely(JS_VALUE_HAS_REF_COUNT(*_vrpN))) _RT->free_value(ctx, *_vrpN);
  *_vrpN = _nv; _sp = {d-1};
}
```

For `GEN_SET_VR` (set_var_ref, no pop) the pattern is identical with `_sp=` omitted.

---

## Warm path

After `JIT_WARM_THRESHOLD_GCC` JIT calls, `js_jit_schedule_warm_recompile` copies the
hint array (now including the n_pv slots) to `b->jit_vt_hints` and queues warm
recompile.

When the warm gen_body enters `GEN_PUT_VR` with gen_st=INT and
`vt_hints[base + pv_idx] == 0` (JS_TAG_INT), the bare assignment is emitted:

```c
*_vrpN = JS_MKVAL(JS_TAG_INT, (int32_t)_ti{d-1}); _sp = {d-1};
```

No `JS_VALUE_HAS_REF_COUNT` test, no `free_value` call.  Safe because:
- The new value is an INT immediate (no refcount to release).
- The old value is also INT (hint says so) → no heap object to free.

---

## gen_st sub-switch

`pv_idx` is incremented in the gen_st sub-switch for all put/set_var_ref* opcodes,
matching the pattern established by vr_idx (P49) and ad_idx (P51):

```c
case OP_put_var_ref:  case OP_put_var_ref_check:
case OP_put_var_ref_check_init:
case OP_put_var_ref0: case OP_put_var_ref1:
case OP_put_var_ref2: case OP_put_var_ref3:
    _gs_drop = 1; pv_idx++; break;
case OP_set_var_ref:  case OP_set_var_ref0: case OP_set_var_ref1:
case OP_set_var_ref2: case OP_set_var_ref3:
    pv_idx++; break;   /* set_var_ref is peek — no _gs_drop */
```

The main switch reads `pv_idx` before the gen_st sub-switch increments it, guaranteeing
the indices stay aligned (same as all other `*_idx` counters).

---

## Files changed

| File | Change |
|------|--------|
| `quickjs.c` | `jit_n_pv` field in `JSFunctionBytecode`; `js_jit_fb_get/set_n_pv` accessors |
| `quickjs-jit.h` | Accessor declarations; `JIT_CODEGEN_VERSION` bumped to 13 |
| `quickjs-jit.c` | Scan loop counts `n_pv`; `__jit_vt_[]` extended by `n_pv`; `GEN_PUT_VR` and `GEN_SET_VR` macros record cold tag and emit warm bare assignment; `pv_idx` counter in gen_st sub-switch; `js_jit_schedule_warm_recompile` reads `n_pv` |
| `jit-tests/js/test_jit_p52_put_var_ref_int.js` | New JS test: 5 cases (counter, two-var accumulator, set_var_ref, Fibonacci, conditional store) |
| `jit-tests/js/Makefile` | New test registered |

---

## JIT_CODEGEN_VERSION

Bumped to **13**.  The `__jit_vt_[]` array layout changed (n_pv more slots at the end),
so cached cold `.so` files from JCV 12 must be invalidated.
