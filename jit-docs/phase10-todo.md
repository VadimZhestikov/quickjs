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

- [ ] **P10.3-A** In `gen_body()`, for `OP_call` / `OP_call_method` with a statically
  known callee (resolved at JIT-compile time via `js_jit_get_callee_fb()`):
  ```c
  // Emit direct-call path if callee bc_hash is known
  jit_buf_printf(cb,
      "extern JSValue __jit_f_%016llx"
      "(JSContext*,JSValue,int,JSValue*,JSValue*,JSValue**);\n",
      callee_hash);
  jit_buf_printf(cb,
      "    _r = __jit_f_%016llx(ctx,_this,%d,_argv,_cpool,_var_refs);\n",
      callee_hash, argc);
  ```

- [ ] **P10.3-B** Add `js_jit_get_callee_fb(JSContext *ctx, JSValue func_obj)`
  in `quickjs.c` / `quickjs-jit.h`:
  - Extract `JSFunctionBytecode *b` from `func_obj` (if JS_CLASS_BYTECODE_FUNCTION)
  - Return `b` (or NULL if not a bytecode function / not yet JIT-compiled)
  - Used at code-gen time to read `b->bc_hash` for the `extern` declaration

- [ ] **P10.3-C** Guard: if callee is not a known bytecode function, fall back to the
  existing `js_jit_call()` indirect path.  Never emit a direct call to an unknown target.

- [ ] **P10.3-D** Run `make CONFIG_JIT=y test`.  Test with `fib` (self-recursive already
  handled by P8.2) and a benchmark with mutual calls (`DeltaBlue`, `RayTrace`).

### Definition of done
For a JS file `const r = dot(a,b)` where `dot` is JIT-compiled, the generated C for the
caller contains `extern JSValue __jit_f_<hash>(...)` and a direct call — no
`js_jit_call()` reference.  GCC `-S` shows the callee body inlined into the caller.

---

## P10.4 — Manifest and Combined Loader
**Estimated effort:** ~1 day  **Risk:** low  **Files:** `quickjs-jit.c`, `quickjs-jit.h`

### Background

After the `--jit-link` step, execution must use the combined `.so` instead of the
individual per-function `.so` files.  This requires a *manifest*: a mapping from
`bc_hash` to symbol address within the combined `.so`.

### Tasks

- [ ] **P10.4-A** Define manifest format:
  ```c
  typedef struct {
      uint64_t bc_hash;
      JSJITFunc func_ptr;
  } JSJITManifestEntry;
  ```
  The combined `.so` exports a `__jit_manifest[]` array and `__jit_manifest_count`.

- [ ] **P10.4-B** Emit manifest in `js_jit_link()`:
  After collecting all `bc_hash` values, append to the combined `.c` a manifest table:
  ```c
  JSJITManifestEntry __jit_manifest[] = {
      { 0xaabbcc0011223344ULL, __jit_f_aabbcc0011223344 },
      { 0xddeeff0055667788ULL, __jit_f_ddeeff0055667788 },
  };
  int __jit_manifest_count = 2;
  ```

- [ ] **P10.4-C** Implement `js_jit_install_combined(JSRuntime *rt, const char *so_path)`:
  1. `dlopen(so_path)` → get `__jit_manifest` and `__jit_manifest_count`
  2. For each entry, find the `JSFunctionBytecode` with matching `bc_hash` and call
     `js_jit_fb_set_func(b, entry.func_ptr)` (atomic RELEASE store)
  3. Call this at the end of `--jit-link` mode before execution begins

- [ ] **P10.4-D** Execution mode: `--jit-link` + `--jit-aot` installs all combined
  functions atomically before the script runs.  Individual `.so` files are no longer
  opened; the combined `.so` is `dlopen`'d once.

- [ ] **P10.4-E** Run `make CONFIG_JIT=y test`.  Run V8bench with `--jit-warmup` +
  `--jit-link` + `--jit-aot` workflow and compare to P9 baseline.

### Definition of done
`./qjs --jit-warmup s.js && ./qjs --jit-link --jit-aot s.js` runs correctly.
`lsof` shows only one `combined_*.so` `dlopen`'d, not individual per-function `.so` files.

---

## P10.5 — IC Check Inlining via LTO
**Estimated effort:** ~1 day  **Risk:** low–medium  **Files:** `quickjs-jit.c`, `Makefile`

### Background

The inline cache check in generated code calls `js_jit_ic_check()` (shape + atom guard,
defined in `quickjs-jit.c`).  Because `quickjs-jit.c` is compiled separately from the
generated `.c` files, GCC sees only an opaque function call — it cannot inline the 3
pointer compares into the hot loop, and the call overhead appears on every property read.

Extracting the IC helpers into a dedicated `quickjs-jit-ic.c` and adding it to the
`--jit-link` GCC invocation is the final step to make every `get_field` hot path a
flat inlined sequence with no function call overhead.

### Tasks

- [ ] **P10.5-A** Extract the IC-related helpers into a separate translation unit
  `quickjs-jit-ic.c` (or use a `.h` with `static inline` definitions):
  - `js_jit_ic_check()` — shape + atom guard
  - `js_jit_ic_fill_get()` — IC miss handler
  - `js_jit_ic_read_f64()` — typed float64 slot read (P9.4)

- [ ] **P10.5-B** Include `quickjs-jit-ic.c` in the GCC command line for `--jit-link`:
  ```sh
  gcc -O2 -flto -shared -fPIC \
      ~/.cache/qjs-jit/*.c quickjs-jit-ic.c \
      -o combined.so
  ```

- [ ] **P10.5-C** Verify GCC inlines the IC check: `gcc -S combined.c` should show the
  shape-guard compare inline in the hot loop, not a `call js_jit_ic_check` instruction.

- [ ] **P10.5-D** Run `make CONFIG_JIT=y test`.  Run V8bench and compare to P10.4 baseline.

### Definition of done
`objdump -d combined.so | grep -c "call.*js_jit_ic_check"` returns 0 — all IC checks
are inlined.  V8bench DeltaBlue and RayTrace improve vs P10.4.

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

- [ ] `make CONFIG_JIT=y test` — full test suite passes
- [ ] `make CONFIG_JIT=y JIT_THRESHOLD_GCC=2 qjs && ./qjs --jit-warmup jit_perf_tests/v8bench/run_qjs.js` — warms cache
- [ ] `./qjs --jit-link jit_perf_tests/v8bench/run_qjs.js` — link step succeeds
- [ ] `(cd jit_perf_tests/v8bench && ../../qjs --jit-aot run_qjs.js)` — 3 runs, record best
- [ ] `lsof | grep qjs-jit` — only `combined_*.so` open during execution (P10.4+)
- [ ] ASAN build: `make CONFIG_JIT=y CONFIG_ASAN=y qjs && ./qjs --jit-aot tests/test_closure.js` — no memory errors
