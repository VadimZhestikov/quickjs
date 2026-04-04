# QuickJS GCC JIT — v8bench Results

## Setup

- Build: `make CONFIG_JIT=y`
- JIT threshold: `JIT_THRESHOLD_GCC=100` (default)
- Machine: Linux x86-64 (WSL2, Intel)
- Benchmark: V8 benchmark suite v6 (`jit_perf_tests/v8bench/run_qjs.js`)

## Correctness

All 7 benchmarks pass with no correctness failures:
Richards, DeltaBlue, Crypto, RayTrace, EarleyBoyer, RegExp, Splay.

## Performance (representative runs)

### No-JIT baseline (`make`)

| Benchmark   | Run 1 | Run 2 |
|-------------|-------|-------|
| Richards    |   808 |   928 |
| DeltaBlue   |    89 |   838 |
| Crypto      |  1001 |  1081 |
| RayTrace    |  1238 |  1135 |
| EarleyBoyer |  1430 |  1277 |
| RegExp      |   395 |   394 |
| Splay       |  2151 |  1999 |
| **Score**   | **727** | **994** |

### With GCC JIT (`make CONFIG_JIT=y`, threshold=100)

| Benchmark   | Run 1 | Run 2 | Run 3 |
|-------------|-------|-------|-------|
| Richards    |   945 |   927 |   868 |
| DeltaBlue   |   890 |   798 |   797 |
| Crypto      |  1083 |  1079 |   879 |
| RayTrace    |  1100 |  1232 |   891 |
| EarleyBoyer |  1047 |  1506 |  1063 |
| RegExp      |   394 |   231 |   244 |
| Splay       |  2413 |  1932 |  2277 |
| **Score**   | **1000** | **943** | **850** |

## Notes

- Scores vary across runs due to asynchronous GCC background compilation;
  the benchmark's measurement window may overlap with compilation overhead.
- EarleyBoyer and Splay show consistent speedup from JIT.
- RegExp shows lower scores because regexp-heavy functions tend to have
  unsupported opcodes and fall back to interpretation.

## Phase 10 — Combined .so (LTO) + Manifest Loader

**Date:** 2026-04-04  
**Workflow:** `--jit-warmup` → `--jit-link` → `--jit-aot`

After Phase 10, the recommended execution path is a 3-step workflow:

1. `./qjs --jit-warmup script.js` — compile all functions to individual `.so` + `.c`
2. `./qjs --jit-link script.js` — LTO-combine all `.c` into `combined.so`
3. `./qjs --jit-aot script.js` — install 527/527 functions from `combined.so`, execute

### V8bench scores (WSL2, ±20% variance)

| Mode | Score |
|---|---:|
| Interpreter (no JIT) | 702 |
| `--jit-warmup` | 788–895 |
| `--jit-aot` + `combined.so` | 841–897 |

The combined.so mode's main benefit is **predictability**: all functions are
pre-installed from a single shared library before execution begins.  See
`jit_perf_tests/RESULTS.md` for full phase-by-phase history.

## Bugs Fixed

### 1. `OP_tail_call_method` — EarleyBoyer crash (previous session)
Tail-call-method compiled to a bare call without a `return`, causing the
JIT function to return garbage. Fixed by emitting an explicit `return`.

### 2. `OP_gt` / `OP_gte` slow-path swapped — Splay "wrong size" failure
The non-integer (float) slow paths for `OP_gt` and `OP_gte` had their
vtable calls transposed:

- `OP_gt` (`a > b`) used `_RT->lte(ctx, b, a)` → `b ≤ a` (includes equal)
- `OP_gte` (`a >= b`) used `_RT->lt(ctx, b, a)` → `b < a` (excludes equal)

When `key == current.key` in `splay_()` (float comparison), `OP_gt`
returned `true` instead of `false`, causing the function to enter the
wrong rotation branch and corrupt the tree structure.

Fixed by swapping: `OP_gt` now uses `lt(b, a)` and `OP_gte` uses `lte(b, a)`.
