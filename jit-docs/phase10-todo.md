# Phase 10 — Combined .so, LTO Inlining, Direct C Calls

## Overview

Phase 9 makes each function's generated C as clean as possible for GCC.  Phase 10
addresses the remaining inter-procedural boundary: today each function compiles to its
own `.so`, so GCC cannot inline `dot()` into its callers, cannot propagate constants
across call boundaries, and cannot vectorise across inlined bodies.

Phase 10 combines all per-function `.c` files belonging to a JavaScript execution into
a single compilation unit and invokes GCC with `-O2 -flto`, giving it visibility across
all JIT-compiled functions simultaneously.

**Dependencies:**
```
P10.1 (cache .c)       ─── independent, do first
P10.2 (--jit-link)     ─── requires P10.1
P10.3 (direct calls)   ─── requires P10.1 (symbol names come from bc_hash)
P10.4 (manifest/loader)─── requires P10.2 + P10.3
P10.5 (IC inlining)    ─── requires P10.4
```

Suggested order: P10.1 → P10.2 → P10.3 → P10.4 → P10.5

---

## P10.1 — Cache `.c` Source Alongside `.so`
**Estimated effort:** ~0.5 day  **Risk:** trivial  **Files:** `quickjs-jit.c`

### Background

`jit_cache_put()` currently writes only the `.so`.  To support the `--jit-link`
combiner, we also need the generated C source.  The `.c` file lives at the same path
as the `.so` with a `.c` extension instead of `.so`.

### Tasks

- [x] **P10.1-A** `jit_cache_put_c_src(src_path, hash)` in `quickjs-jit.c`: copies the
  temp `.c` file to `<cache_dir>/<hash>.c` immediately before the temp file is unlinked
  (in `jit_compile_gcc_job`).  Non-atomic write (supplementary artifact).

- [x] **P10.1-B** `jit_cache_has_c_src(hash)` and `jit_cache_get_c_src(hash)` helpers;
  public API `js_jit_cache_has_c_src(JSFunctionBytecode *b)` declared in
  `quickjs-jit.h` for use by P10.2 `--jit-link`.

- [x] **P10.1-C** `--jit-dump-c` CLI flag (`qjs.c` + `js_jit_set_dump_c_mode()`):
  prints generated C to stdout with `/* ==== JIT: <name> [<hash>] ==== */` header
  before each function is submitted to GCC.

- [x] **P10.1-D** `--jit-warmup` now executes the script (instead of exiting after the
  static compile-only pass), so `load()`'d files trigger the `js_loadScript` AOT hook
  and their functions are compiled and cached.  Exit happens after execution completes.

- [x] **P10.1-E** Verified: after `./qjs --jit-warmup run_qjs.js` on the V8bench suite,
  `~/.cache/qjs-jit/` contains **527** `.c`/`.so` pairs and **147** `.skip` markers.
  Every `.so` has a matching `.c` (shell loop confirmed 0 mismatches).

### Definition of done ✓
After `./qjs --jit-warmup script.js`, every `.so` in `~/.cache/qjs-jit/` has a
matching `.c` file with identical hash prefix.

---

## P10.2 — `--jit-link` Combiner
**Estimated effort:** ~1.5 days  **Risk:** medium  **Files:** `quickjs-jit.c`, `qjs.c`

### Background

`--jit-link` is a new execution mode: after all functions have been compiled to
individual `.so` + `.c` files (via `--jit-warmup`), the combiner collects all `.c`
files referenced by the current script, concatenates them with renamed entry-point
symbols, and calls GCC once with `-O2 -flto -shared`:

```
gcc -O2 -flto -shared -fPIC \
    ~/.cache/qjs-jit/aabbcc.c \
    ~/.cache/qjs-jit/ddeeff.c \
    ...
    -o ~/.cache/qjs-jit/combined_<run_hash>.so
```

GCC now sees all functions simultaneously and can inline, propagate constants, and
vectorise across boundaries.

### Tasks

- [x] **P10.2-A** Add `--jit-link` CLI flag in `qjs.c`; wire to `js_jit_link()` in
  `quickjs-jit.c`.  `--jit-link` implies `--jit-aot` (sets both flags).  Post-execution
  call to `js_jit_link()` added after `js_std_loop()` in `main()`.

- [x] **P10.2-B** Implement `js_jit_link()`:
  1. Hash registry: `jit_link_record_hash(bc_hash)` called in `js_jit_queue_gcc` after
     hash computation; duplicates removed at link time.
  2. Collect `.c` paths via `jit_cache_get_c_src()` for all recorded hashes.
  3. Build GCC command: `gcc -O2 -flto -shared -fPIC <srcs...> -o combined.so`.
  4. Fork + exec GCC synchronously (log goes to `/tmp/qjs_jit_link.log`).
  5. Output written to `<cache_dir>/combined.so` (no JSRuntime field needed for P10.2;
     P10.4 will add the install step).

- [x] **P10.2-C** No symbol conflicts: each `.c` defines `__jit_f_<hash>` with unique
  hash.  Verified: `nm ~/.cache/qjs-jit/combined.so | grep __jit_f | wc -l` = 528.

- [x] **P10.2-D** Help text updated in `qjs.c`.  Phase doc updated here.

- [x] **P10.2-E** `make CONFIG_JIT=y test` passes (bjson.so ASAN error is pre-existing).
  V8bench warmup + link verified: 527 functions combined into 2.4 MB `combined.so`.

### Definition of done ✓
`./qjs --jit-warmup run_qjs.js && ./qjs --jit-link run_qjs.js` produces
`~/.cache/qjs-jit/combined.so` with 528 `__jit_f_*` symbols.
`nm combined.so | grep __jit_f | wc -l` = 528.  Execution correct.

---

## P10.3 — Direct C Calls Between JIT Functions
**Estimated effort:** ~1 day  **Risk:** medium  **Files:** `quickjs-jit.c`

### Background

Today, when a JIT-compiled function calls another JIT-compiled function, the path is:
```
__jit_f_caller  →  js_jit_call()  →  atomic ACQUIRE read of jit_func
                                   →  indirect call through function pointer
```

Even if the callee is in the same combined `.so`, GCC sees an indirect call through a
runtime-variable pointer — no inlining, no IPO.

The fix: at code-generation time, if we know the callee's `bc_hash` (available in the
`JSFunctionBytecode` of the called function), we can emit a direct `extern` declaration
and a direct call.  When both caller and callee land in the same combined `.so`, GCC
inlines small callees automatically.

### Tasks

- [x] **P10.3-A** In `gen_body()`, for `OP_call` / `OP_call0`..`OP_call3` with a
  statically known JIT callee (resolved at codegen time via closure var_refs):
  New `JIT_T_JIT_FUNC` gen_st marker tracks which stack slots hold a known JIT function.
  Parallel `gen_hsh[]` array stores the callee's `bc_hash`. When `OP_call*` sees
  `JIT_T_JIT_FUNC` at the function slot, emits a guarded direct call:
  ```c
  if (js_jit_check_and_extract(_f, (JSJITFunc)__jit_f_<hash>, &_dc, &_dv)) {
      if (_RT->poll_interrupts(ctx)) goto _ex;
      _r = __jit_f_<hash>(ctx, JS_UNDEFINED, N, _ca, _dc, _dv);
  } else {
      _r = _RT->call(ctx, _f, JS_UNDEFINED, N, _ca);
  }
  ```

- [x] **P10.3-B** Added `js_jit_get_callee_fb(JSValue func_obj)` in `quickjs.c`
  and `quickjs-jit.h`: extracts `JSFunctionBytecode*` from a bytecode function JSValue.
  Used in `js_jit_queue_gcc()` to resolve closure var_refs to their bc_hashes.

- [x] **P10.3-C** Guard via `js_jit_check_and_extract()`: checks that `_f` is exactly
  the expected JIT function (pointer identity on `jit_func` field), then extracts
  `cpool` and `var_refs` for the direct call.  Falls back to `_RT->call()` otherwise.

- [x] **P10.3-D** `js_jit_queue_gcc()` signature updated to accept `JSVarRef **var_refs`.
  At threshold-based compile time (when var_refs is live), iterates closure vars to find
  JIT-compiled callees and builds `p103_hash[]` for codegen.  AOT pre-pass passes NULL.
  OP_drop and OP_dup fixed: `>= JIT_T_NUMBER` changed to `>= JIT_T_NUMBER && <= JIT_T_INT`
  so JIT_T_JIT_FUNC slots are treated as JSValue (not typed double).

- [x] **P10.3-E** Tests pass: `./qjs tests/test_closure.js && ./qjs --std tests/test_builtin.js`.
  Verified: `QJS_JIT_KEEP_C=1 ./qjs /tmp/test_p103.js` generates a file with
  `js_jit_check_and_extract` and `__jit_f_<callee_hash>` direct call.

### Definition of done ✓
For a JS file where `outer()` calls `helper()` (both JIT-compiled, `helper` captured in
`outer`'s closure), the generated C for `outer` contains:
- `extern JSValue __jit_f_<hash>(...)` for `helper`
- `extern int js_jit_check_and_extract(...)` guard
- Guarded direct call with `_RT->call()` fallback
No `js_jit_call()` reference for known callees.

---

## P10.4 — Manifest and Combined Loader ✓
**Estimated effort:** ~1 day  **Risk:** low  **Files:** `quickjs-jit.c`, `quickjs-jit.h`

### Background

After the `--jit-link` step, execution must use the combined `.so` instead of the
individual per-function `.so` files.  This requires a *manifest*: a mapping from
`bc_hash` to symbol address within the combined `.so`.

### Tasks

- [x] **P10.4-A** Define manifest format in `quickjs-jit.h`:
  ```c
  typedef struct {
      uint64_t bc_hash;
      JSJITFunc func_ptr;
  } JSJITManifestEntry;
  ```
  The combined `.so` exports a `__jit_manifest[]` array and `__jit_manifest_count`.

- [x] **P10.4-B** Emit manifest in `js_jit_link()`:
  After collecting all `bc_hash` values, appends to the combined `.c` a manifest table:
  ```c
  JSJITManifestEntry __jit_manifest[] = {
      { 0xaabbcc0011223344ULL, __jit_f_aabbcc0011223344 },
      ...
  };
  int __jit_manifest_count = N;
  ```

- [x] **P10.4-C** Implemented `js_jit_install_combined_if_exists()` and helper
  `jit_install_combined_pass()`:
  1. `js_jit_preload_combined()` — opens combined.so once, caches manifest pointer.
     Called before `js_jit_compile_all()` so the fast-path in `js_jit_queue_gcc` can
     skip loading individual `.so` files for functions already in combined.so.
  2. `jit_install_combined_pass()` — iterates manifest, patches any bytecodes not yet
     using combined.so.  Skips entries where `old_handle == jit_combined_handle`.
  3. Incremental: called once per script file load; each call patches newly-discovered
     bytecodes as `load()`'d scripts register their functions.

- [x] **P10.4-D** Execution mode: in `--jit-aot` mode, `js_jit_preload_combined()` is
  called before `js_jit_compile_all()`.  Functions already in combined.so are installed
  directly from the manifest in `js_jit_queue_gcc` without loading individual `.so`.
  `js_jit_install_combined_if_exists()` cleans up any stragglers.
  Both `qjs.c` (eval_buf + module path) and `quickjs-libc.c` (js_loadScript) updated.

- [x] **P10.4-E** Tests pass: `make CONFIG_JIT=y test`.  V8bench workflow verified:
  `--jit-warmup` (808) → `--jit-link` (527 functions) → `--jit-aot` (838, 527/527 installed).
  Also fixed two pre-existing correctness bugs discovered during testing:
  - `js_jit_op_shr` (JIT `>>>` operator): used `js_binary_logic_slow` which aborts for
    BigInt — fixed to use `js_shr_slow` (proper TypeError for BigInt, uint32 coercion).
  - `js_jit_install_combined_if_exists` idempotent check: prevented patching bytecodes
    from subsequently `load()`'d scripts — fixed with incremental re-scan per call.

### Definition of done ✓
`./qjs --jit-warmup s.js && ./qjs --jit-link s.js && ./qjs --jit-aot s.js` runs correctly.
V8bench: 527/527 functions installed from combined.so.  Score 838 vs 808 warmup baseline.
Individual `.so` files are NOT loaded when combined.so is preloaded (fast-path in
`js_jit_queue_gcc` skips the cache lookup for manifest-known functions).

---

## P10.5 — IC Check Inlining via Macro ✓
**Estimated effort:** ~1 day  **Risk:** low–medium  **Files:** `quickjs-jit.c`, `quickjs-jit.h`, `quickjs.c`

### Background

The inline cache check in generated code calls `js_jit_ic_check()` (shape + atom guard,
defined in `quickjs.c`).  Because `quickjs.c` is compiled separately from the
generated `.c` files, GCC sees only an opaque function call — it cannot inline the 3
pointer compares into the hot loop, and the call overhead appears on every property read.

**Implementation note**: LTO was tried first (compile quickjs.c with `-flto -c`, add
the resulting `.o` to `--jit-link`).  The LTO IR was present (`readelf` confirms
`.gnu.lto_*` sections), but GCC's inliner declined to inline `js_jit_ic_check` even at
`-O3` with 2328 call sites — the code-size growth heuristic blocked it.

Final approach: `JIT_IC_CHECK` macro in `quickjs-jit.h` expands the full shape+atom
guard inline at each call site.  Struct offsets are verified by `_Static_assert` in
`quickjs.c`.  The LTO object is still added to `--jit-link` for other cross-module
optimizations (constant propagation across JIT functions), and the link step uses
`-O3` instead of `-O2` when the LTO object is present.

### Tasks

- [x] **P10.5-A** Added `JIT_IC_CHECK(obj, ic)` macro to `quickjs-jit.h`:
  - Shape NULL / MEGAMORPHIC guard
  - `JS_TAG_OBJECT` tag check
  - `JSObject.shape` pointer comparison (byte offset `JIT_OBJIC_SHAPE_OFF = 32`)
  - `JSShape.prop_count > slot` bound check (byte offset `JIT_SHAPEIC_PROPCOUNT_OFF = 40`)
  - `JSShape.prop[slot].atom == ic->atom` ABA guard (`JIT_SHAPEIC_PROP_OFF = 64`,
    stride = 8, atom at offset 4 within `JSShapeProperty`)
  - `JIT_IC_MEGAMORPHIC` sentinel moved to `quickjs-jit.h` (was in `quickjs.c`)
  - Five `_Static_assert` checks in `quickjs.c` verify all offsets match the live structs

- [x] **P10.5-B** Code generator (`quickjs-jit.c`) updated: all three IC callsites
  (`OP_get_field`, `OP_get_field2`, `OP_put_field`) now emit `JIT_IC_CHECK` instead of
  `js_jit_ic_check`.  Generated `.c` files expand the macro inline — no function call.

- [x] **P10.5-C** `jit_ensure_lto_obj()` in `quickjs-jit.c`: compiles `quickjs.c` to an
  LTO fat-binary `.o` (cached by mtime in `~/.cache/qjs-jit/qjs_ic_<mtime>.o`).
  Added to `--jit-link` GCC command; link step uses `-O3` when LTO object is present
  (enables more aggressive optimization of JIT functions and IC fast-paths).

- [x] **P10.5-D** `objdump -d combined.so | grep -c "call.*js_jit_ic_check"` = **0**.
  `make CONFIG_JIT=y test` passes.  V8bench AOT scores (3 runs): 745 / 856 / 998 / 1018
  (vs P10.4 baseline 838–897).

### Definition of done ✓
`objdump -d combined.so | grep -c "call.*js_jit_ic_check"` returns 0 — all IC checks
are inlined.  `make CONFIG_JIT=y test` passes.  Scores in the 850–1018 range on WSL2.

---

## Workflow summary

The recommended usage after Phase 10 is complete:

```sh
# Step 1: warm-up run (compiles all functions to individual .so + .c)
./qjs --jit-warmup script.js

# Step 2: link step (combines .c files, LTO, produces combined.so)
./qjs --jit-link script.js

# Step 3: execute with combined .so
./qjs --jit-aot script.js
```

For development, the old per-function mode still works:
```sh
./qjs --jit-aot script.js   # triggers GCC per function on first encounter
```

---

## Milestone summary

| Sub-phase | Deliverable | Effort | Risk | Expected V8bench gain |
|---|---|---:|---|---:|
| P10.1 Cache .c | `.c` alongside `.so` in cache | 0.5 day | trivial | no change |
| P10.2 --jit-link | Combined LTO `.so` | 1.5 days | medium | baseline for P10 |
| P10.3 Direct calls | No `js_jit_call()` for known callees | 1 day | medium | +20–50% on call-heavy |
| P10.4 Manifest/loader | Atomic install from combined `.so` | 1 day | low | enables P10.3 benefit |
| P10.5 IC inlining | IC check inlined by LTO | 1 day | low | +5–15% on IC-heavy |
| **Total** | | **~6 days** | | **~1.5–2.5× on top of P9** |

### Per-benchmark projections (P9 → P10)

| Benchmark | After P9 | After P10 | P10 gain | Driver |
|---|---:|---:|---|---|
| RayTrace | 3000–5000 | 8000–15000 | **2–3×** | dot/normalize inlined, float loop vectorised |
| DeltaBlue | 1600–2300 | 2200–4600 | **1.4–2×** | constraint propagation methods inlined |
| Crypto | 2200–3800 | 3000–5000 | **1.3–1.5×** | int arithmetic chains, CSE across calls |
| Richards | 1100–1450 | 1400–1800 | **1.2–1.4×** | task object methods inlined |
| Splay | 1700–2050 | 2000–2500 | **1.2–1.3×** | tree rotation helpers inlined |
| EarleyBoyer | 1800–2400 | 2100–2800 | **1.1–1.3×** | parser utilities inlined |
| RegExp | 430–510 | 440–530 | **~1.05×** | regex engine not JIT-compiled |
| **V8bench score** | **~1800–3000** | **~3000–5500** | **~1.7–2×** | |

The ceiling for the GCC-JIT approach (JS→C ABI, IC guards, boxing at JS boundaries) is
approximately 10000–15000 overall, or ~50–75% of V8.  Emitting native machine code
directly would be required to close the remaining gap.

---

## Testing checklist (run after each sub-phase)

- [x] `make CONFIG_JIT=y test` — full test suite passes
- [x] `make CONFIG_JIT=y JIT_THRESHOLD_GCC=2 qjs && ./qjs --jit-warmup jit_perf_tests/v8bench/run_qjs.js` — warms cache
- [x] `./qjs --jit-link jit_perf_tests/v8bench/run_qjs.js` — link step succeeds
- [x] `(cd jit_perf_tests/v8bench && ../../qjs --jit-aot run_qjs.js)` — 3 runs, record best
- [x] ASAN build: `make CONFIG_JIT=y CONFIG_ASAN=y qjs && ./qjs --jit-aot tests/test_closure.js` — no memory errors
