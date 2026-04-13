# Phase 46 — Warm-IC Recompile: gen_st=INT for get_array_el

## Background

P45b added the two-phase JIT warm-IC recompile for `OP_get_field`.  P46 extends
the same mechanism to `OP_get_array_el` (array element reads), reusing the same
`__jit_vt_HASH[]` BSS export and warm-recompile infrastructure.

Array element reads in INT-typed arrays (e.g. `s += a[i]` in a sum loop)
account for the most DupValue/FreeValue overhead after property reads.  With
gen_st=INT, the element fast path eliminates all refcount operations and enables
the `_bn` (both-int) path in downstream arithmetic.

---

## Architecture

### `__jit_vt_HASH[]` array extension

P45b allocated `n_gf` entries (one per `OP_get_field`).  P46 extends this to
`n_gf + n_ae` entries:

```
__jit_vt_HASH[0 .. n_gf-1]       ← get_field  val_tag hints (P45b)
__jit_vt_HASH[n_gf .. n_gf+n_ae-1] ← get_array_el val_tag hints (P46)
```

`n_ae` is stored in `JSFunctionBytecode.jit_n_ae` (uint8_t, previously
`_jit_p45b_pad`).  This field is set by `js_jit_gen_c` during the pre-pass.

### Warm trigger

The warm recompile condition changes from `b->jit_n_gf > 0` to:

```c
(b->jit_n_gf > 0 || b->jit_n_ae > 0)
```

`js_jit_schedule_warm_recompile` reads `n_hints = n_gf + n_ae` entries from
`__jit_vt_HASH` via `dlsym`.

### Code generation

`gen_preamble` receives `(n_gf, n_ae)` and emits:
```c
uint8_t __jit_vt_HASH[n_gf + n_ae];
```

`gen_body` receives `(vt_hints, n_gf, n_ae)` and tracks `int ae_idx = 0`.

#### Main switch — `OP_get_array_el` dual-path (P46)

Only applies when `_idx_typed` (index is native int64_t from prior op):

```c
int _p46_use_int = (_idx_typed && vt_hints
                    && (n_gf + ae_idx) < (n_gf + n_ae)
                    && vt_hints[n_gf + ae_idx] == 0);
```

**Fast path with INT hint** (`_p46_use_int`):
```c
_r = values[_ai];
if (likely(JS_VALUE_GET_TAG(_r) == JS_TAG_INT)) {
    _ti{d-2} = (int64_t)JS_VALUE_GET_INT(_r);
    /* no DupValue — INT is refcount-free */
    goto _aok;
}
/* else: fall through to slow path */
```

The tag check in the fast path is essential: if an element changes type after
the warm compile, the fast path falls through to the slow path rather than
extracting garbage from `JS_VALUE_GET_INT` on a non-INT value.

**Slow path with INT hint**:
```c
_r = RT->get_array_el(ctx, obj, idx);
_CHK(_r);
_ti{d-2} = (JS_VALUE_GET_TAG(_r) == JS_TAG_INT)
           ? (int64_t)JS_VALUE_GET_INT(_r)
           : (JS_FreeValue(ctx,_r), 0LL);
```

Speculative: if element is non-INT, result is 0 (wrong but safe, no crash).

**Without INT hint** (cold path or non-INT hint): updates BSS tag slot and
stores to `_tsv{d-2}` (existing P38.2 behaviour).

#### Secondary gen_st switch — `OP_get_array_el` (P46)

```c
case OP_get_array_el: {
    int _idx_is_int = (gen_sp >= 1 && gen_st[gen_sp-1] == JIT_T_INT);
    int _p46_gst = (_idx_is_int && vt_hints
                    && (n_gf + ae_idx) < (n_gf + n_ae)
                    && vt_hints[n_gf + ae_idx] == 0)
                   ? JIT_T_INT : JIT_T_JSVAL;
    _gs_drop = 2; _gs_push = _p46_gst;
    ae_idx++;
    break;
}
```

Pushing `JIT_T_INT` when the hint is INT causes downstream `OP_add`, `OP_mul`
etc. to use the `_bn`/`_ti` fully-typed fast path.

### Interaction with non-typed-index paths

When `!_idx_typed`, the index is unboxed from `_tsv` at runtime.  P46 does
NOT apply INT hint optimisation in this case — the array fast path requires
`_idx_typed` for the inner-loop pattern where the index is already a native
`int64_t`.  Non-typed-index paths still record `val_tag` in the BSS array
for future use.

---

## Files Changed

### `quickjs.c`

- `JSFunctionBytecode.jit_n_ae` (uint8_t): replaces `_jit_p45b_pad`.
  Stores the count of `OP_get_array_el` sites for warm hint indexing.
- Accessors: `js_jit_fb_get_n_ae`, `js_jit_fb_set_n_ae`.
- Warm trigger: `(b->jit_n_gf > 0 || b->jit_n_ae > 0)` in both JIT call paths.

### `quickjs-jit.h`

- `js_jit_fb_get_n_ae` / `js_jit_fb_set_n_ae` declarations.

### `quickjs-jit.c`

- `gen_preamble`: `n_hints = n_gf + n_ae`; BSS array = `uint8_t __jit_vt_HASH[n_hints]`.
- `gen_body`: `int n_ae` param; `int ae_idx = 0`; `OP_get_array_el` dual-path
  codegen with `_p46_use_int` flag and tag check in fast path.
- Secondary gen_st switch: `OP_get_array_el` case pushes `JIT_T_INT` when
  `_idx_is_int && hint==INT`; increments `ae_idx`.
- `js_jit_gen_c`: pre-pass counts `n_ae`; calls `js_jit_fb_set_n_ae`.
- `js_jit_schedule_warm_recompile`: `n_hints = n_gf + n_ae`.

---

## Correctness Notes

- Fast path tag check prevents garbage extraction when element changes type
  after warm compile (type-change test in test_jit_p46_warm_arr.js).
- Slow path is speculative: returns 0 for non-INT element, no crash.
- `_idx_typed` requirement limits the optimization to the common loop pattern
  (`for (let i = 0; ...) s += a[i]`) where i is already a native int64_t.

---

## Sub-phases

### P46.1 — `jit_n_ae` field + accessors ✅ Done
### P46.2 — Warm trigger condition update ✅ Done
### P46.3 — `gen_preamble` n_hints = n_gf + n_ae ✅ Done
### P46.4 — `gen_body` dual-path OP_get_array_el codegen ✅ Done
### P46.5 — Secondary gen_st switch ae_idx increment ✅ Done
### P46.6 — `js_jit_gen_c` pre-pass counts n_ae ✅ Done
### P46.7 — Fast-path tag check correctness fix ✅ Done
### P46.8 — Correctness tests ✅ Done

**Tests:** `jit-tests/js/test_jit_p46_warm_arr.js` — 8 tests, all pass.

Tests cover:
- INT array sum survives 350 calls (warm recompile fires at 200+100=300 total)
- Arithmetic on INT array elements (multiply+add)
- Element type change INT→string after warm recompile (no crash/throw)
- Dot product of two INT arrays
- INT comparison on array element (count > threshold)
- Nested array read (matrix sum)
- Mixed get_field + get_array_el in same function

### P46.9 — Performance measurement ✅ Done (see RESULTS_P46.md)
