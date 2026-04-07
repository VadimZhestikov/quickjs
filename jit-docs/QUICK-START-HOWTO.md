# QuickJS GCC JIT — Quick-Start How-To

## Prerequisites

- GCC (any recent version, ≥ 9 recommended)
- Linux x86-64 (WSL2 is fine)
- Standard build tools (`make`, `cc`)

---

## Step 1 — Build with JIT enabled

```sh
cd quickjs/
make CONFIG_JIT=y
```

This produces `./qjs` with the GCC JIT tier compiled in.  The default hot-function
threshold is 100 calls before JIT compilation is triggered.

To change the threshold at build time:

```sh
make CONFIG_JIT=y JIT_THRESHOLD_GCC=50
```

Or override it at runtime without rebuilding:

```sh
./qjs --jit-threshold-gcc=10 my_script.js   # compile after 10 calls
./qjs --jit-threshold-gcc=1  my_script.js   # compile on first call
./qjs --jit-threshold-gcc=0  my_script.js   # AOT pre-pass before execution
```

---

## Step 2 — Run a script (on-demand JIT)

```sh
./qjs my_script.js
```

Functions called ≥ 100 times are queued for GCC compilation in a background thread.
Compiled `.so` files are cached in `~/.cache/qjs-jit/`.  Subsequent runs reuse the
cache and skip recompilation.

To watch what the JIT is doing:

```sh
./qjs --jit-dump-c       my_script.js 2>/dev/null | head -60  # print generated C
./qjs --jit-save-sources my_script.js                         # save <hash>.js per function
ls ~/.cache/qjs-jit/*.js                                      # see which functions were compiled
```

---

## Step 3 — Three-step workflow for best performance

Use this when you want all functions compiled ahead-of-time with LTO optimization.

**Step 3a — Warmup** (populate the cache):

```sh
./qjs --jit-warmup my_script.js
```

Executes the script normally, compiles every eligible function to an individual `.so`
and saves the generated C source to `~/.cache/qjs-jit/`.  Exits after execution.

**Step 3b — Link** (combine all functions into one optimised `.so`):

```sh
./qjs --jit-link my_script.js
```

Combines all cached `.c` files into `~/.cache/qjs-jit/combined.so` using
`gcc -O3 -flto -shared`.  GCC sees all JIT functions simultaneously and can
inline across call boundaries.  Also compiles `quickjs.c` to an LTO object for
additional cross-module optimization.

The first `--jit-link` run takes a few extra seconds to compile the LTO helper
object (`quickjs.c`).  The object is cached by source mtime and reused in all
subsequent runs.

**Step 3c — Execute from combined `.so`**:

```sh
./qjs --jit-aot my_script.js
```

Pre-installs all 527 (or however many) functions from `combined.so` before
execution begins.  No GCC compilation happens during the run.

---

## Step 4 — Verify the cache

```sh
ls ~/.cache/qjs-jit/ | head -10
# <hash>.c   — generated C source
# <hash>.so  — individually compiled .so
# <hash>.skip — function ineligible or unsupported opcodes; skipped permanently
# combined.so — LTO-combined library (after --jit-link)
```

Check how many functions ended up in the combined library:

```sh
nm ~/.cache/qjs-jit/combined.so | grep -c '^.\{17\} T __jit_f_'
```

Verify the IC check is inlined (should be 0):

```sh
objdump -d ~/.cache/qjs-jit/combined.so | grep -c 'call.*js_jit_ic_check'
```

---

## Step 5 — Clear the cache

If you change the script or rebuild `qjs`, clear the cache so stale `.so` files
are not reused:

```sh
rm -rf ~/.cache/qjs-jit/
```

Or point to a different cache directory:

```sh
QJS_JIT_CACHE=/tmp/my-jit-cache ./qjs --jit-warmup my_script.js
```

---

## Typical V8bench workflow

```sh
cd jit_perf_tests/v8bench

# Warmup (compiles 527 functions; takes ~30 s on first run)
../../qjs --jit-warmup run_qjs.js

# Link (combines into combined.so with -O3 LTO; ~20 s on first run)
../../qjs --jit-link run_qjs.js

# Measure (3 runs)
../../qjs --jit-aot run_qjs.js
../../qjs --jit-aot run_qjs.js
../../qjs --jit-aot run_qjs.js
```

---

## Environment variables

| Variable | Default | Description |
|---|---|---|
| `QJS_JIT_CACHE` | `~/.cache/qjs-jit` | Cache directory for `.so`, `.c`, `.js`, `.skip` files |

---

## Build flags summary

| Flag | Default | Description |
|---|---|---|
| `CONFIG_JIT=y` | off | Enable the GCC JIT tier |
| `JIT_THRESHOLD_GCC=N` | 100 | Calls before a function is queued for GCC |
| `JIT_INCLUDE_DIR=path` | current dir | `-I` path used when GCC compiles JIT functions |
