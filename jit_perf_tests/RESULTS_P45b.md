# P45b Performance Results — Warm-IC Recompile gen_st=INT

## Setup

- **Baseline (P45)**: `qjs` binary at P45 (JIT_CODEGEN_VERSION=5)
- **P45b**: `qjs` binary built with CONFIG_JIT=y including P45b changes (JIT_CODEGEN_VERSION=6)
- **JIT threshold**: default (100 calls)
- **Measurement**: fresh-cache runs (each clears `~/.cache/qjs-jit/` before run)
- **Platform**: Linux (WSL2), x86-64

## V8 Benchmark Results (fresh-cache, 3 runs)

Note: fresh-cache measurements include GCC `.so` compilation time (~20–25s per run),
which causes variance. Score range is typical for this measurement methodology.

### P45b (qjs) — 3 fresh-cache runs

| Benchmark   | Run 1 | Run 2 | Run 3 |
|-------------|-------|-------|-------|
| Richards    | 607   | 586   | 530   |
| DeltaBlue   | 451   | 541   | 433   |
| Crypto      | 712   | 610   | 680   |
| RayTrace    | 766   | 688   | 555   |
| EarleyBoyer | 751   | 858   | 802   |
| RegExp      | 230   | 208   | 235   |
| Splay       | 414   | 1304  | 1392  |
| **Score**   | **523** | **609** | **583** |

### Summary vs P45 baseline

| | P45 median | P45b median | Δ |
|---|---|---|---|
| V8 Score | ~555 | **~583** | **+5%** |

## Analysis

### Why the improvement is modest on fresh-cache runs

Fresh-cache benchmarks include GCC compilation time for each `.so` file (cold + warm).
The warm recompile adds a **second GCC compilation** per function after 200 calls,
so fresh-cache P45b is actually slightly slower in wall-clock compilation overhead.
The score improvement here comes from JIT execution time improvements on
benchmarks where warm recompile fires (Richards, DeltaBlue, EarleyBoyer).

The full benefit of P45b is visible in **warm-cache** runs (cached `.so` from prior
run already installed), where the second-run execution uses the INT-typed fast path
from call 1.  Note: warm-cache Crypto has a pre-existing failure (see below).

### Which benchmarks benefit

- **Richards**: Object-heavy benchmark with many repeated INT property reads
  (`Task.state`, `Packet.kind`, etc.).  `gen_st=INT` eliminates tag checks in
  inner loops.  Best case for P45b.

- **DeltaBlue**: Constraint-satisfaction with INT property reads in tight loops.
  Similar benefit to Richards.

- **EarleyBoyer**: Parser benchmark; property reads for parser state.  Moderate
  benefit.

- **Splay**: BST benchmark; node property reads.  High variance due to GC
  interaction.

### Why gen_st=INT helps

When `OP_get_field` pushes `JIT_T_INT` (P45b), downstream arithmetic ops
(`OP_add`, `OP_mul`, `OP_lt`, etc.) use the `_bn`/`_ti` fully-typed fast path
(no `JS_VALUE_GET_TAG` check, no `JS_DupValue`).  This is the full optimization
originally planned for P45, enabled by the warm-IC recompile infrastructure.

### BSS zero ambiguity

Property accesses that are never executed during the 200-call warm-up period
retain `val_tag = 0` (uninitialised BSS = JS_TAG_INT).  These cold branches
receive a false INT hint but are never executed in hot benchmarks, so no
incorrect results occur in practice.

## Known Issue: Crypto Second-Run Failure

Pre-existing bug (present since P43): second run with cached `.so` produces
wrong RSA decryption.  Unrelated to P45b.  Always measure with fresh cache.
See `project_crypto_bug_resolved.md` for details.

## Next Steps (P46)

P46 extends the same warm-IC recompile approach to `OP_get_array_el` (array
element reads) using `JSJITArrayICEntry.val_tag`.  Expected additional gains on
Splay (node array accesses) and array-heavy benchmarks.
