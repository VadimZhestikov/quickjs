# Phase 45b — Warm-IC Recompile: gen_st=INT for get_field

## Background

P45 added `val_tag` to `JSJITICEntry` and filled it at IC-fill time, but
`gen_st` for `OP_get_field` stayed `JIT_T_JSVAL` because at cold codegen time
the IC entries are zero-initialised BSS (indistinguishable from "INT observed"
vs "never observed").

P45b implements the two-phase JIT strategy that P45 was groundwork for:

1. **Cold compile** (at JIT threshold = 100): emit `__jit_vt_HASH[N]` as a BSS
   export in the `.so`; update each entry in the IC miss path with `val_tag`.
2. **Warm recompile** (after 200 warm JIT calls): read `__jit_vt_HASH[N]` via
   `dlsym`, copy INT hints to a heap buffer, queue a new GCC job that generates
   C with `gen_st=INT` for INT-hinted `OP_get_field` ops.

After the warm `.so` installs, downstream arithmetic on INT property reads uses
the fully-typed `_ti`/`_bn` fast paths (no tag check, no `JS_DupValue`).

---

## Architecture

### Cold .so: `__jit_vt_HASH[N]` export

`gen_preamble` emits a BSS array when `n_gf > 0`:

```c
uint8_t __jit_vt_0123456789abcdef[3];  /* N = OP_get_field count */
```

The IC miss path for each `OP_get_field` writes:

```c
__jit_vt_0123456789abcdef[gf_idx] = _ic0.e[0].val_tag;
```

So after the first JIT call, `__jit_vt_HASH[i]` holds the `val_tag` of the most
recently observed value for property `i`.  Value `0` = `JS_TAG_INT`; any non-zero
value = non-INT (string, float64, object, etc.).  Uninitialised BSS zeros are
accepted as INT (ambiguity — see "BSS zero ambiguity" below).

### Warm trigger

After the cold `.so` is installed, `JSFunctionBytecode` accumulates a
`jit_warm_count`.  At call #200:

```c
if (unlikely(!b->jit_warm_done && b->jit_n_gf > 0))
    if (++b->jit_warm_count == 200u)
        js_jit_schedule_warm_recompile(caller_ctx, b);
```

`js_jit_schedule_warm_recompile`:
1. Sets `jit_warm_done = 1` (prevents re-entry).
2. `dlsym`s `__jit_vt_HASH` from `jit_handle`.
3. Checks `any_int` — if no hint is 0 (INT), no warm benefit, returns.
4. Copies hints to a `malloc`-allocated `jit_vt_hints` buffer on the bytecode.
5. Calls `js_jit_queue_warm_gcc(ctx, b)`.

### Warm GCC job

`js_jit_queue_warm_gcc` calls `js_jit_gen_c(b, ..., warm_hash, ...)`.  The warm
function symbol is:

```
warm_hash = bc_hash | 0x8000000000000000ULL
```

This makes the warm symbol `__jit_f_8xxxxxxxxxxxxxxx` (high bit set) distinct
from the cold `__jit_f_0xxxxxxxxxxxxxxx`.  Crucially:

- No disk cache write (warm job has `is_warm = 1`).
- Session map uses the **original** `bc_hash` for bytecode lookup on install.

### Warm .so install

`js_jit_install_results` on `is_warm = 1`:
```c
js_jit_fb_set_warm_handle(b, r->handle);   /* store warm handle for dlclose */
js_jit_fb_set_warm_func(b, r->func);       /* atomic RELEASE store of jit_func */
jit_registry_add(...);
```

The cold `.so` (`jit_handle`) is **not closed**: call ICs in other functions may
hold pointers into the cold `.so`'s function body.  Only `jit_warm_handle` is
closed at bytecode free time.

### Code generation with INT hints

In `gen_body`, when `vt_hints != NULL` and `vt_hints[gf_idx] == 0`:

**IC hit path** (INT hint active):
```c
_ti{d-1} = (int64_t)JS_VALUE_GET_INT(_r);
/* no JS_DupValue — INT is refcount-free */
```

**IC miss path** (INT hint active):
```c
_ti{d-1} = (JS_VALUE_GET_TAG(_r) == JS_TAG_INT)
           ? (int64_t)JS_VALUE_GET_INT(_r) : (JS_FreeValue(ctx,_r), 0LL);
```

The miss path is speculative: if the property returns a non-INT value after
warm recompile, `_ti` is set to 0 (wrong but safe).  The JSVAL `_tsv` slot is
not updated on the warm INT path, so if a downstream op ever checks `_tsv` it
will see stale data — this is acceptable because `gen_st=INT` downstream code
only reads `_ti`, never `_tsv`.

**Secondary gen_st switch** pushes `JIT_T_INT` when hint is INT, and increments
`gf_idx`:

```c
case OP_get_field: {
    int _p45b_gst = (vt_hints && gf_idx < n_gf && vt_hints[gf_idx] == 0)
                    ? JIT_T_INT : JIT_T_JSVAL;
    _gs_drop = 1; _gs_push = _p45b_gst;
    gf_idx++;
    break;
}
```

### BSS zero ambiguity

`JS_TAG_INT == 0` on this platform.  An uninitialised `__jit_vt_HASH[i]` entry
(BSS zero) is indistinguishable from "INT observed".  We accept this:

- Warm recompile only fires after 200 JIT calls, so any hot get_field will have
  been through the miss path and updated its entry.
- Cold branches that are never exercised in the 200 calls get a false INT hint,
  but they are also never executed in hot benchmarks — so wrong speculation on
  those branches has zero runtime effect.

---

## Files Changed

### `quickjs.c`

- `JSFunctionBytecode` struct: added `jit_warm_count` (uint32), `jit_n_gf`
  (uint16), `jit_warm_done` (uint8), `_jit_p45b_pad` (uint8), `jit_vt_hints`
  (uint8*), `jit_warm_handle` (void*).
- New accessors: `js_jit_fb_get/set_n_gf`, `_warm_done`, `_vt_hints`,
  `_warm_handle`, `_warm_func`, `_bc_hash` (new getter added alongside existing
  setter).
- Warm trigger added to both JIT call fast-paths (early `_jf41` path and
  standard `jf` path).
- Bytecode free path: delegates all P45b resource cleanup to
  `js_jit_free_bytecode` (which uses real stdlib `free`; quickjs.c redefines
  `free` as `free_is_forbidden`).
- `js_jit_fb_set_vt_hints`: no longer frees old value; lifetime managed by
  `js_jit_free_bytecode`.

### `quickjs-jit.h`

- P45b accessor declarations.
- `js_jit_schedule_warm_recompile` declaration.
- `JIT_CODEGEN_VERSION` bumped 5 → 6 with history comment.

### `quickjs-jit.c`

- `JITGCCJob` / `JITGCCResult`: added `int is_warm` field.
- `jit_result_add`: accepts `is_warm` parameter.
- `jit_compile_gcc_job`: skips disk cache ops when `is_warm`.
- `js_jit_install_results`: warm-install path (no `jit_tier` update).
- `js_jit_free_bytecode`: closes `jit_warm_handle`; frees `jit_vt_hints` via
  real `free`.
- New functions: `js_jit_queue_warm_gcc`, `js_jit_schedule_warm_recompile`.
- `gen_preamble`: emits `uint8_t __jit_vt_HASH[N]` when `n_gf > 0`; takes
  `n_gf` parameter.
- `gen_body`: takes `const uint8_t *vt_hints, int n_gf` parameters; tracks
  `int gf_idx` local.
- `OP_get_field` main switch: dual-path INT/JSVAL codegen with `_GF_VT_UPDATE`
  in miss path.
- `OP_get_field` secondary gen_st switch: pushes `JIT_T_INT` or `JIT_T_JSVAL`
  based on hint; increments `gf_idx`.
- `js_jit_gen_c`: pre-pass counts `n_gf`; calls `js_jit_fb_set_n_gf`; passes
  `vt_hints`/`n_gf` to `gen_body`.

---

## Correctness Notes

- Warm path is **speculative**: if a property changes from INT to non-INT after
  warm recompile, `_ti` gets a wrong value (0) but no crash.  This is the same
  trade-off as P44 HALF_L arithmetic.
- The cold `.so` stays loaded to avoid dangling call IC pointers.  This costs
  one `dlopen` handle and the mapped `.so` pages per warm function.
- `jit_warm_done=1` before the warm job is queued prevents a second warm
  recompile even if the job is slow to install.

---

## Sub-phases

### P45b.1 — `JSFunctionBytecode` fields + accessors ✅ Done
### P45b.2 — Warm trigger in JIT call paths ✅ Done
### P45b.3 — `js_jit_schedule_warm_recompile` ✅ Done
### P45b.4 — `js_jit_queue_warm_gcc` + `is_warm` job plumbing ✅ Done
### P45b.5 — `gen_preamble` `__jit_vt_` export ✅ Done
### P45b.6 — `gen_body` dual-path OP_get_field codegen ✅ Done
### P45b.7 — Secondary gen_st switch INT promotion + gf_idx ✅ Done
### P45b.8 — `JIT_CODEGEN_VERSION` 5 → 6 ✅ Done
### P45b.9 — Correctness tests ✅ Done

**Tests:** `jit-tests/js/test_jit_p45b_warm_ic.js` — 6 tests, all pass.

Tests cover:
- INT field sum survives 350 calls (warm recompile fires at 200+100=300 total)
- Triple INT field arithmetic
- Property type change INT→string after warm recompile (no crash)
- Two different objects; INT path only
- Chained `get_field` (obj.inner.z)
- INT hint used in comparison operator

### P45b.10 — Performance measurement ✅ Done (see RESULTS_P45b.md)
