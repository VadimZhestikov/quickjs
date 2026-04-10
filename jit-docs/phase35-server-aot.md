# Phase 35 — Server-Side Pre-Compiled AOT Pipeline

## Motivation

The existing JIT pipeline is designed for **long-running interpreted processes** where
functions become hot over time and GCC compiles them in the background.  This model
has inherent limitations for **high-performance server-side code**:

1. **Cold-start penalty** — every new server process begins at tier 0; JIT warm-up takes
   seconds to minutes before full performance is reached.
2. **Runtime compilation overhead** — GCC runs in production, consuming CPU and memory
   that should serve requests.
3. **Pathological data files** — scripts consisting of large static data (e.g., Unicode
   mapping tables) can block the GCC worker for hundreds of minutes, stalling compilation
   of genuinely hot functions.
4. **Non-deterministic startup** — the moment a function reaches tier 2 depends on
   traffic patterns; latency spikes occur at threshold crossings.

Phase 35 addresses all four with a suite of five sub-phases targeting server deployments
where code is **compiled at build time, not at runtime**.

---

## Overview

```
Source code                   Build step (CI/CD)             Production runtime
───────────                   ──────────────────             ──────────────────
server.js                                                     ./server   (P35.4)
 ├─ module_a.js    ──►  qjsc / qjs (P35.1–P35.5)  ──►  or:
 ├─ module_b.js         offline GCC (-O3 / LTO)         ./qjs --jit-aot server.js
 └─ data.js (big)       ↓
                    app.so / server binary            All functions at tier 2
                                                      from the very first request
```

---

## P35.1 — Bytecode Size Cap (prerequisite for all sub-phases) ✓ DONE

**Goal:** Prevent runaway GCC compilation of data-initialization functions during both
warmup and runtime.  A prerequisite that unblocks the rest of P35.

**Problem:** `string-upper-lower-mapping.js` (3.2 MB, 65 536 Unicode mapping pairs)
generates a single 34 MB C function that takes **340 minutes** to compile at `-O2`.
The function runs once, has 2 branches, and the JIT provides zero benefit.

**Solution:** Skip JIT for any function whose bytecode exceeds a configurable limit.
`jit_no_compile` is already set before the check fires, so the function is never
re-queued.  Cap of 0 disables the limit entirely.

### Implementation

- **P35.1-A** `JIT_MAX_BC_LEN = 32768` added to `quickjs-jit.h` as a `#ifndef`-guarded
  macro (default 32768; override at compile time with `-DJIT_MAX_BC_LEN=N`).

- **P35.1-B** Size check in `js_jit_queue_gcc` (after `js_jit_fb_set_no_compile` and
  worker check):
  ```c
  if (jit_max_bc_len > 0) {
      int _bc_len;
      js_jit_fb_get_bytecode(b, &_bc_len);
      if (_bc_len > jit_max_bc_len) return;
  }
  ```

- **P35.1-C** Runtime override: `js_jit_set_max_bc_len(n)` / `js_jit_get_max_bc_len()`
  in `quickjs-jit.c`; declared in `quickjs-jit.h`.

- **P35.1-D** `--jit-max-bc=N` CLI flag in `qjs.c` and `qjsc.c`.

- **P35.1-E** C harness: `jit-tests/P35/test_p35_1.c` — 4 subtests:
  - A: cap set just below actual bc_len → function stays tier 0, `jit_no_compile=1`
  - B: large cap → function reaches tier 2 normally
  - C: cap=0 disables limit → function reaches tier 2 regardless of size
  - D: `js_jit_get_max_bc_len()` returns `JIT_MAX_BC_LEN` (32768)

**Files changed:** `quickjs-jit.h`, `quickjs-jit.c`, `qjs.c`, `qjsc.c`,
`jit-tests/P35/test_p35_1.c`, `jit-tests/P35/Makefile`, `jit-tests/Makefile`

---

## P35.2 — Whole-Application AOT Compilation (`qjsc --jit-hybrid-app`) ✓ DONE

**Goal:** Extend the P34 per-module hybrid format to the entire application module
graph.  All reachable functions are compiled to native code at build time; no GCC
runs at production startup.

**Concept:**

```
server.js
 ├─ import './module_a.js'
 └─ import './module_b.js'
        │
        ▼  qjsc --jit-hybrid-app -o app.c server.js
        │
app.c   (all modules: bytecodes + JIT function bodies + per-module dispatch tables)
        │
        ▼  gcc -O3 -flto -march=native -shared -fPIC -DCONFIG_JIT -o app.so app.c
        │
app.so  ← loaded at startup; all functions at tier 2 immediately
```

No representative traffic needed — all statically reachable functions are compiled.

### Implementation

**New public APIs** (declared in `quickjs-jit.h`, implemented in `quickjs.c`):

- **`js_jit_walk_module_graph(ctx, entry_module_val, cb, opaque)`** — walks the import
  graph from `entry_module_val` (a `JS_TAG_MODULE` value). Follows
  `JSModuleDef.req_module_entries[]` recursively; deduplicates by module pointer.
  Works with both COMPILE_ONLY modules (imports unresolved → only entry walked) and
  fully evaluated modules (full graph walked).  Handles both pre-evaluation
  (`JS_TAG_FUNCTION_BYTECODE`) and post-evaluation (`JS_CLASS_BYTECODE_FUNCTION` object)
  states of `JSModuleDef.func_obj`.

- **`js_jit_walk_all_modules(ctx, cb, opaque)`** — iterates `ctx->loaded_modules` and
  calls `js_jit_walk_bytecodes` on each module body.  Used by the generated
  `js_init_app(ctx)`.  Works after module evaluation because it extracts bytecodes
  from `JS_CLASS_BYTECODE_FUNCTION` objects as well as raw `JS_TAG_FUNCTION_BYTECODE`.

**`qjsc --jit-hybrid-app`** (`OUTPUT_C_HYBRID_APP` mode in `qjsc.c`):
- Accepts one or more `.js` input files, each compiled with `COMPILE_ONLY`
- Walks bytecodes of each module; applies P35.1 size cap; deduplicates by bc_hash
- Emits: JIT function bodies + flat dispatch table + `js_init_app(ctx)`:
  ```c
  void js_init_app(JSContext *ctx) {
  #ifdef CONFIG_JIT
      js_jit_walk_all_modules(ctx, _install_app_cb, NULL);
  #endif
  }
  ```

**Build chain:**
```sh
# Build step (CI/CD):
./qjsc --jit-hybrid-app -o app.c server.js module_a.js module_b.js
gcc -O3 -flto -shared -fPIC -DCONFIG_JIT -I. -o app.so app.c

# Runtime:
void *h = dlopen("./app.so", RTLD_NOW);
void (*init_app)(JSContext *) = dlsym(h, "js_init_app");
// After modules are loaded:
init_app(ctx);  /* all eligible functions now at tier 2 */
```

**Tests:** `jit-tests/P35/test_p35_2.c` — 4 subtests:
- A: `js_jit_walk_all_modules` finds all bytecodes (module body + inner functions) after evaluation
- B: After two-module load (entry imports lib), finds bytecodes from both modules
- C: `js_jit_walk_module_graph` on a COMPILE_ONLY module visits its inner functions
- D: `js_jit_walk_module_graph` is a no-op for non-module JSValues

**Files changed:** `quickjs.c`, `quickjs-jit.h`, `qjsc.c`,
`jit-tests/P35/test_p35_2.c`, `jit-tests/P35/Makefile`

---

## P35.3 — Static Function Enumeration (`--jit-compile-all`) ✓ DONE

**Goal:** Compile every statically reachable function to native code without executing
the script.  Eliminates the need for representative traffic during the build step.
Complements P35.2 for single-file scripts that do not use ES modules.

**Difference from P35.2:** P35.2 targets multi-module apps; P35.3 targets single-file
scripts or CommonJS-style code where the module graph isn't available statically.
P35.3 works by parsing and walking the bytecode tree without evaluating anything.

### Implementation

**`qjs.c`** — two new flags + `eval_buf` branches:

- **`jit_compile_all_mode`** (set by `--jit-compile-all`):
  - Module path: extends the existing `if (jit_aot_mode)` block to also trigger on
    `jit_compile_all_mode`; calls `js_jit_compile_all` + `js_jit_drain()` +
    `js_jit_install_results()` (no combined `.so`); respects `jit_exit_mode`.
  - Global-script path: new `else if (jit_compile_all_mode)` branch — compile-only
    eval → `js_jit_compile_all` → `js_jit_drain()` → `js_jit_install_results()` →
    optional `jit_exit_mode` skip → `JS_EvalFunction`.

- **`jit_exit_mode`** (set by `--jit-exit`): after compile+install, free the bytecode
  value and return 0 without executing.  Useful as a pure CI build step that has no
  side-effects from the script.

```sh
# Compile all functions into cache, then run normally:
./qjs --jit-compile-all server.js

# Pure build step — compile cache, skip execution:
./qjs --jit-compile-all --jit-exit server.js
```

**No combined LTO `.so`**: unlike `--jit-aot`, `--jit-compile-all` installs each
function from its own cached `.so` via `js_jit_install_results()`.  The P35.1 size
cap applies automatically inside `js_jit_compile_all` → `js_jit_queue_gcc`.

**Tests:** `jit-tests/P35/test_p35_3.c` — 4 subtests:
- A: `js_jit_compile_all` + drain + install puts all 3 inner functions at tier 2
  BEFORE `JS_EvalFunction` is called
- B: After installation the functions execute correctly (add, fib)
- C: `--jit-exit` equivalent — compile+install, skip `JS_EvalFunction`, side-effect
  never runs, yet functions are at tier 2
- D: P35.1 size cap integration — small function reaches tier 2, large function
  stays at tier 0 due to cap

**Files changed:** `qjs.c`, `jit-tests/P35/test_p35_3.c`, `jit-tests/P35/Makefile`

---

## P35.4 — Standalone Binary (`qjsc --standalone`) ✓ DONE

**Goal:** Produce a self-contained executable that embeds the QuickJS runtime,
bytecode, and JIT-compiled native code.  No `.js` files, no `.so` files, no `qjs`
binary required at deployment.  Single-binary distribution.

**Concept:**

```
server.js
    │
    ▼  qjsc --standalone -o server server.js
    │  (generates /tmp/outNNN.c, then compiles it)
    │
server.c   (bytecodes + JIT bodies + _eval_blob helper + main())
    │
    ▼  gcc -O2 -DCONFIG_JIT -I<quickjs_dir> -o server server.c libquickjs.a -lm -ldl -lpthread
    │
./server   ← self-contained; runs without any .js files
```

### Implementation

**`qjsc.c`** — new `OUTPUT_STANDALONE` mode:

- **`g_jit_module_hook`** / **`g_jit_module_hook_opaque`** (globals, CONFIG_JIT-guarded):
  Set during standalone compilation so `jsc_module_loader` notifies the JIT walker
  whenever a dependency module is recursively compiled.  This gives JIT coverage
  for the full import graph, not just explicitly listed files.

- **`standalone_module_walk(root_bc, opaque)`** (static, after `output_hybrid_app`):
  Walker hooked into `jsc_module_loader`; adds `root_bc` to the AppWalkState roots
  list (to exclude module body from JIT) and walks inner functions.

- **`jsc_module_loader` hook** (inside `#ifdef CONFIG_JIT`): after
  `output_object_code` for each JS dependency module, calls `g_jit_module_hook`
  if set.

- **`OUTPUT_STANDALONE` block in `main()`**:
  1. Opens a temp file (`jit_tmp`) for JIT function bodies.
  2. Compiles each listed input file:  
     bytecode blob → `fo` (main output) via `output_object_code`;  
     JIT function bodies → `jit_tmp` via `AppWalkState`.  
     Dependency modules are handled by the `jsc_module_loader` hook.
  3. After compilation: copies `jit_tmp` into `fo`, emits dispatch table +
     `_install_cb` (inside `#ifdef CONFIG_JIT`).
  4. Emits `_eval_blob(ctx, buf, len, load_only)` static helper — mirrors
     `js_std_eval_binary` but injects `js_jit_walk_bytecodes(b, _install_cb, NULL)`
     before evaluation.
  5. Emits `JS_NewCustomContext` (loads dependency modules with `_eval_blob(..., 1)`)
     and `main()` (executes entry scripts with `_eval_blob(..., 0)` + `js_std_loop`).
  6. Compiles the C file to a binary via `output_executable` with `-DCONFIG_JIT`
     passed as an extra compiler flag.

- **`output_executable` signature extended**: added `const char **extra_cflags`
  parameter (NULL for all existing callers); for `--standalone`, passes
  `{"-DCONFIG_JIT", NULL}` so the `#ifdef CONFIG_JIT` blocks in `_eval_blob` and
  the dispatch table are active.

**Module loader**: the standalone binary installs `js_module_loader` from
`quickjs-libc` so dynamic `import()` still works at runtime for modules not
embedded at build time.  All statically-imported modules are pre-loaded in
`JS_NewCustomContext` and resolve without filesystem access.

**Usage:**
```sh
# Build (libquickjs.a must be built with CONFIG_JIT=y):
./qjsc --standalone -o server server.js

# Multi-file (entry imports lib; lib is auto-embedded via jsc_module_loader):
./qjsc --standalone -o server server.mjs

# Deploy:
./server   # runs with no .js files, all bytecodes embedded
```

**Tests:** `jit-tests/P35/test_p35_4.sh` — 4 subtests:
- A: simple global script; binary runs without source file present
- B: functions produce correct results (fib, add)
- C: ES module with `import`; binary runs after source files are removed
- D: std library (print, scriptArgs) available in standalone binary

**Files changed:** `qjsc.c`, `jit-tests/P35/test_p35_4.sh`, `jit-tests/P35/Makefile`

---

## P35.5 — Profile-Guided Optimization (PGO)

**Goal:** Two-phase build: first collect a call-count + branch-weight profile; then
compile with per-function GCC optimization level selected by hotness.  Hot functions
get `-O3`; cold ones get `-O0` (fast compile time); very cold ones are skipped.

**Concept:**

```
Phase 1 — profile collection (CI, with representative traffic):
  ./qjs --jit-profile=profile.json server.js < sample_requests.ndjson
  → profile.json: { "hash": call_count, ... }

Phase 2 — optimized AOT build:
  qjsc --jit-hybrid-app --jit-pgo=profile.json -o app.c server.js
  gcc ... -o app.so app.c
  → hot functions:  -O3 -fprofile-use (branch weights from profile)
  → warm functions: -O2
  → cold functions: -O0
  → very cold:      skipped (interpreter)
```

### Steps

- **P35.5-A** ✓ DONE — Profile format + writer API.

  Profile format (`profile.json`):
  ```json
  { "functions": [
      { "hash": "39aff7310a2a1d1e", "calls": 142857, "name": "fib" },
      { "hash": "abcd1234abcd1234", "calls": 12,     "name": "init" }
  ] }
  ```

  Implementation:
  - `js_jit_fb_get_call_count(b)` added to `quickjs.c` (symmetric accessor for
    the existing `js_jit_fb_inc_count`); declared in `quickjs-jit.h`.
  - `js_jit_write_profile(JSContext *ctx, const char *path)` added to
    `quickjs-jit.c`; walks all live module bytecodes via
    `js_jit_walk_all_modules()` and emits JSON for every function with
    `jit_call_count > 0`.  Returns 0 on success, -1 if `fopen` fails.
  - Only module-based bytecodes are captured; global-script bytecodes freed by
    `JS_EvalFunction` are not (by design — P35 targets module-based server code).
  - Tests: `jit-tests/P35/test_p35_5a.c` (4 subtests: JSON structure, call
    count accuracy, zero-call filter, `get_call_count` unit test).

- **P35.5-B** Add `--jit-profile=<file>` flag to `qjs.c`.  During execution, record
  `(bc_hash, call_count)` for every function that has `jit_call_count > 0`.  Write
  `profile.json` on clean exit.  Calls `js_jit_write_profile(ctx, path)` after
  `js_std_loop` returns.

- **P35.5-C** Add `--jit-pgo=<file>` flag to `qjsc.c`.  During P35.2/P35.3 C
  generation, look up each function's `bc_hash` in the profile.  Select optimization
  level:

  | calls | GCC flag |
  |---|---|
  | ≥ 10 000 | `-O3 -march=native` |
  | ≥ 1 000  | `-O2` |
  | ≥ 100    | `-O1` |
  | ≥ 1      | `-O0` |
  | 0 / absent | skipped (no C emitted) |

- **P35.5-D** Per-function optimization levels: emit `#pragma GCC optimize("O3")`
  before each JIT function body in the generated C and `#pragma GCC optimize("O2")`
  after, so all functions can be compiled in a single GCC invocation without needing
  separate per-function compilation units.

- **P35.5-E** Tests in `jit-tests/P35/`: collect profile from a benchmark; verify that
  hot function is at tier 2 with higher optimization; cold function is absent from the
  dispatch table.

**Estimated effort:** ~7 days (P35.5-A done; ~5.5 days remaining)  
**Risk:** medium — per-function optimization selection, GCC pragma injection  
**Dependencies:** P35.2 or P35.3 (AOT C generation), P35.1 (size cap)  
**Files:** `qjs.c`, `qjsc.c`, `quickjs-jit.c`, `quickjs-jit.h`

---

## Dependency graph

```
P35.1 (size cap)              ← prerequisite for all; implement first
    │
    ├─► P35.3 (compile-all)   ← 2 days; single-file scripts, no traffic needed
    │
    ├─► P35.2 (hybrid-app)    ← 5 days; multi-module, static compilation
    │       │
    │       ├─► P35.4 (standalone binary)   ← 6 days; single-binary deploy
    │       │
    │       └─► P35.5 (PGO)                 ← 7 days; maximum performance
    │
    └─► (P35.5 also usable with P35.3 for single-file scripts)
```

## Summary

| Sub-phase | What it solves | Effort | Risk |
|---|---|---|---|
| P35.1 size cap | Pathological data files; unblocks all others | 0.5 day | trivial |
| P35.2 hybrid-app | Whole-app AOT; no traffic needed | 5 days | medium |
| P35.3 compile-all | Single-file static compilation | 2 days ✓ | low |
| P35.4 standalone | Single-binary deployment | 6 days ✓ | medium-high |
| P35.5 PGO | Per-function optimization levels | 7 days | medium |

**Minimum viable**: P35.1 + P35.3 (~2.5 days total) gives a usable server-side
pre-compilation workflow for single-file scripts with no traffic requirement.

**Full pipeline**: P35.1 + P35.2 + P35.5 (~12.5 days) gives the complete
profile-guided whole-application AOT system for production server deployments.
