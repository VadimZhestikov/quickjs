# P45 Performance Results — IC val_tag Groundwork

## Setup

- **Baseline (P44)**: `qjs` binary built with CONFIG_JIT=y including P44 changes
- **P45**: `qjs_jit_p45` binary built with CONFIG_JIT=y including P45 changes
- **JIT threshold**: default (100 calls)
- **Measurement**: fresh-cache runs (each clears `~/.cache/qjs-jit/` before run)
- **Platform**: Linux (WSL2), x86-64

## V8 Benchmark Results (fresh-cache, 3 runs each)

Note: fresh-cache measurements include GCC `.so` compilation time, which dominates
the benchmark duration (~20–25s per run vs ~2s warm-cache). This causes high
variance between runs and makes scores lower than warm-cache measurements.

### P44 baseline (qjs) — 3 fresh-cache runs

| Benchmark   | Run 1 | Run 2 | Run 3 |
|-------------|-------|-------|-------|
| Richards    | 724   | 563   | 447   |
| DeltaBlue   | 490   | 473   | 184   |
| Crypto      | 620   | 463   | 409   |
| EarleyBoyer | 670   | 721   | 471   |
| Splay       | 1404  | 1358  | 1172  |
| **Score**   | **605** | **558** | **392** |

### P45 (qjs_jit_p45) — 3 fresh-cache runs

| Benchmark   | Run 1 | Run 2 | Run 3 |
|-------------|-------|-------|-------|
| Richards    | 544   | 332   | 338   |
| DeltaBlue   | 528   | 441   | 315   |
| Crypto      | 606   | 590   | 403   |
| EarleyBoyer | 678   | 706   | 505   |
| Splay       | 1399  | 1672  | 1267  |
| **Score**   | **586** | **555** | **426** |

### Summary

| | P44 median score | P45 median score | Δ |
|---|---|---|---|
| V8 Score | 558 | 555 | ~0% |

**P45 has no measurable performance impact**, as expected from the implementation
analysis (see below).

## Analysis

### Why P45 shows no V8 speedup

P45's actual implementation (see `jit-docs/phase45-steps.md` P45.3 notes) is:

1. **val_tag field added to `JSJITICEntry`**: Records `JS_VALUE_GET_TAG(value)`
   at IC fill time. This is pure infrastructure — no runtime behaviour change.

2. **DupValue elision for INT**: The IC hit path now emits:
   ```c
   if(js_likely(JS_VALUE_GET_TAG(_r)!=JS_TAG_INT)) JS_DupValue(ctx,_r);
   ```
   instead of unconditional `JS_DupValue(ctx,_r)`. Since `JS_DupValue` for INT
   (tag=0, non-negative) is already a branch-with-no-effect (inlined, one check
   that's always false), GCC optimizes both forms to the same machine code. **Zero
   runtime savings.**

3. **gen_st stays `JIT_T_JSVAL`**: The planned `gen_st=INT` propagation for
   get_field — which would eliminate downstream tag-check overhead in arithmetic
   — was NOT implemented. See P45.3 notes for the detailed analysis of why
   this requires a "warm-IC recompile" mechanism that doesn't exist yet.

### What P45 provides

- **val_tag infrastructure**: Every `js_jit_ic_fill_get` call now records the
  value's type tag. This data is available at runtime in the IC entry for any
  future optimization that can read it.

- **JIT_CODEGEN_VERSION 4 → 5**: Invalidates all P44 caches, ensuring P45 code
  is regenerated cleanly.

- **Correctness tested**: All 10 tests in `jit-tests/js/test_jit_p45_ic_int.js`
  pass, including type-change-mid-run, polymorphic IC, negative INT, and
  overflow edge cases.

## Known Issue: Crypto Second-Run Failure

When run a second time with cached `.so` files, the Crypto benchmark fails
("Crypto operation failed"). This is a **pre-existing bug** present in all
JIT versions (P43, P44, P45) and is unrelated to P45 changes. The `qjs_nojit`
binary (interpreter-only) passes Crypto consistently.

Root cause under investigation. All fresh-cache runs pass Crypto correctly.

## Next Steps (P45b)

To realize the full performance benefit originally planned for P45, a
**warm-IC recompile** mechanism is needed:

1. Run function N times with JIT to fill ICs with `val_tag` data
2. Re-JIT-compile the function reading `val_tag` at codegen time
3. Set `gen_st=INT` where `val_tag==JS_TAG_INT` → downstream arithmetic
   uses the fully-typed INT fast path

Expected improvement on P45b: +5–20% on benchmarks with INT property arithmetic
(Richards, DeltaBlue where object properties drive loop arithmetic).
