# Phase 54 — Warm-IC Recompile: 1-Slot Monomorphic IC for get_field

## Background

P45b introduced warm-IC recompile: after 200 JIT calls the cold `.so` hint array
(`__jit_vt_HASH[]`) is copied to `b->jit_vt_hints` and a warm GCC job is queued.
When `vt_hints[gf_idx] == 0` (JS_TAG_INT) the warm `OP_get_field` path skips
`JS_DupValue` and reads the property slot directly as an `int64_t`.

The warm INT path was, until P54, still using:

```c
static JSJITICEntry2 _icN={{},0};
```

`JSJITICEntry2` is 200 bytes and has four sub-entries plus three dead `n>=2/3/4`
branches.  For monomorphic call sites (the vast majority of hot numeric code)
these three branches are never taken — pure overhead.

P54 replaces the warm INT `OP_get_field` path with the lighter 1-slot IC:

```c
static JSJITICEntry _icN={0};   /* 48 bytes, 1 slot */
```

`JSJITICEntry` is 48 bytes: `void *shape`, `uint32_t slot`, `uint32_t atom`,
`uint8_t kind`, `uint8_t val_tag`, `uint32_t shape_gen`, `uint32_t rt_gen`,
`void *rt`.  A single `JIT_IC_CHECK_FAST` check suffices.  The miss path calls
`js_jit_ic_fill_get(ctx, _o, atom, &_icN)` (single-slot fill, already in the
runtime) and updates the BSS hint: `__jit_vt_HASH[gf_idx] = _icN.val_tag`.

The cold path and all JSVAL warm paths are **unchanged** — they still use
`JSJITICEntry2`.

---

## What Changes in the Warm C Source

### Before P54 (warm INT get_field, `top_borrowed=0`)

```c
{ static JSJITICEntry2 _ic14={{},0};
  JSValue _o=_tsv3, _r;
  if (js_likely(JIT_IC_CHECK_FAST(_o,&_ic14.e[0]))){
      JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);
      _r=_pp[_ic14.e[0].slot];
      _ti14=(int64_t)JS_VALUE_GET_INT(_r); _FREE(_o); _sp=3; }
  else if (js_likely(_ic14.n>=2&&JIT_IC_CHECK_FAST(_o,&_ic14.e[1]))){
      ...  /* dead for monomorphic sites */
  else if (js_likely(_ic14.n>=3&&JIT_IC_CHECK_FAST(_o,&_ic14.e[2]))){
      ...
  else if (js_likely(_ic14.n>=4&&JIT_IC_CHECK_FAST(_o,&_ic14.e[3]))){
      ...
  else { _r=_RT->get_prop(ctx,_o,(JSAtom)613u);
         js_jit_ic2_fill_get(ctx,_o,(JSAtom)613u,&_ic14);
         __jit_vt_HASH[0]=_ic14.e[0].val_tag;
         ...
  } }
```

### After P54 (warm INT get_field, `top_borrowed=0`)

```c
{ static JSJITICEntry _ic14={0};        /* P54: 1-slot mono */
  JSValue _o=_tsv3, _r;
  if (js_likely(JIT_IC_CHECK_FAST(_o,&_ic14))){
      JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);
      _r=_pp[_ic14.slot];
      _ti14=(int64_t)JS_VALUE_GET_INT(_r); _FREE(_o); _sp=3; }
  else { _r=_RT->get_prop(ctx,_o,(JSAtom)613u);
         js_jit_ic_fill_get(ctx,_o,(JSAtom)613u,&_ic14);
         __jit_vt_HASH[0]=_ic14.val_tag;
         _FREE(_o); _sp=2; _CHK(_r);
         _ti14=(int64_t)(JS_VALUE_GET_TAG(_r)==JS_TAG_INT
                         ?JS_VALUE_GET_INT(_r):0);
         JS_FreeValue(ctx,_r); _sp=3; } }
```

The `top_borrowed=1` variant (preceding `get_loc` skipped `JS_DupValue`) differs
only in the hit path: `_r=_pp[_ic14.slot]; _ti14=...` with no `_FREE(_o)`.

---

## Changes Scope

Only two code generation branches in `js_jit_gen_body()` are changed.  Both are
in the `OP_get_field` handling inside the `_p45b_use_int` block (~line 5852):

1. `top_borrowed == 1` arm — already had a separate warm INT branch
2. `top_borrowed == 0` arm — same change

No new runtime functions, no new structures, no `JIT_CODEGEN_VERSION` bump (the
warm `.so` is never disk-cached, so no invalidation is needed).

---

## IC Size Comparison

| Type             | Size    | Slots | Branches in warm INT path |
|------------------|---------|-------|---------------------------|
| `JSJITICEntry2`  | 200 B   | 4     | 5 (hit×4 + miss)          |
| `JSJITICEntry`   | 48 B    | 1     | 2 (hit + miss)            |

Per get_field site in the warm `.so`:
- 152 bytes of BSS saved (static IC object)
- 3 dead conditional branches eliminated from the hot loop

---

## Files Changed

| File | Change |
|------|--------|
| `quickjs-jit.c` | `OP_get_field` warm INT path: `JSJITICEntry2` → `JSJITICEntry`; `js_jit_ic2_fill_get` → `js_jit_ic_fill_get`; `_ic.e[0].slot` → `_ic.slot`; `_ic.e[0].val_tag` → `_ic.val_tag`; extra `n>=2/3/4` branches removed (both `top_borrowed` arms) |
| `jit-tests/js/test_jit_p54_mono_ic.js` | New JS test: 5 cases covering monomorphic warm correctness, varying INT values, miss-path VT update, and nested functions |
| `jit-tests/js/Makefile` | `test_jit_p54_mono_ic.js` registered |
| `jit-docs/phase54-steps.md` | This document |

---

## JIT_CODEGEN_VERSION

**No bump required.** The warm `.so` is always compiled fresh from `b->jit_vt_hints`
and is never written to the disk cache.  Only cold `.so` files are cached; their
layout is unchanged.
