# Phase 43 Implementation Steps

## Status (2026-04-12) — PHASE 43 COMPLETE

| Step | Sub-phase | Status |
|------|-----------|--------|
| Measurement harness | P43.1 | **Obsoleted** — `__jit_drain` builtin added instead |
| Quadrimorphic IC | P43.2 | **Done** (84f18cc) |
| DupValue inlining audit | P43.3 | **Done** (no-op — already fully inlined) |
| Crypto bit-op audit (INT type prop) | P43.4 | **Done** (d7e56be) |
| _tsv=JS_UNDEFINED safety fix | P43.5 | **Done** (50e10e0) |
| INT over-promotion fix + P9.4 shuffles | P43.6 | **Done** (c8c60f1) |
| Splay investigation | — | **Resolved** — not a real regression |
| bench_runner.js fix | —  | **Done** (02d04ab) — `__jit_drain` + `js_jit_install_results` |
| Tests + docs + results | P43.6 | **Done** (c8c60f1) |

## Warm-Cache Baseline (2026-04-11, after P41)

| Benchmark | Interpreter | JIT warm | Delta |
|-----------|-------------|----------|-------|
| Richards | 1058 | 1056 | −0% |
| DeltaBlue | 941 | 933 | −1% |
| Crypto | 1245 | 1216 | **−2%** |
| RayTrace | 1322 | 1362 | +3% |
| EarleyBoyer | 1707 | 1632 | **−4%** |
| RegExp | 441 | 435 | −1% |
| Splay | 2648 | 2825 | +7% |
| **Score** | **1184** | **1184** | **0%** |

## Remaining Steps (priority order)

---

### Step 1 — Re-measure V8 Benchmarks After P43.2 + P43.4

**Goal:** Confirm P43.2 (quadrimorphic IC, 84f18cc) and P43.4 (INT type
propagation for bit ops, d7e56be) improved Crypto/EarleyBoyer/DeltaBlue
as expected, and collect the updated warm-cache baseline.

```sh
# Clear cache so new IC struct takes effect everywhere
rm -rf ~/.cache/qjs-jit

# Pass 1 — compile
cd jit_perf_tests/v8bench && ../../qjs run_qjs.js 2>/dev/null

# Pass 2 — measure (warm cache)
../../qjs run_qjs.js 2>/dev/null
```

Record results in `jit_perf_tests/RESULTS_P43.md` under heading
`## After P43.2 (quadrimorphic IC)`.

---

### Step 3 — DupValue Inlining Audit (P43.3)

**Goal:** Verify `JS_DupValue` inlines in generated JIT `.so` files.
If GCC emits a real call (not inlined), the tag-check branch cannot be
hoisted across loop iterations and costs ~2 cycles per property read.

```sh
# Check a generated .so for DupValue calls
objdump -d ~/.cache/qjs-jit/<hash>.so | grep -A2 "call.*DupValue\|bl.*DupValue"
# Expected: no output (fully inlined)
```

If calls appear: add `__attribute__((always_inline))` to `JS_DupValue`
in `quickjs.h` and verify the generated `.so` no longer has call sites.

**Expected gain:** 1–3% on property-access heavy benchmarks (Richards,
DeltaBlue, EarleyBoyer) if currently not inlining.

---

### Step 4 — Tests and Result Recording (P43.6)

**Goal:** Lock in the improvements with tests and update docs.

#### 4a. Re-run bench_runner.js and record numbers

```sh
rm -rf ~/.cache/qjs-jit
./qjs --jit-threshold-gcc=1 jit_perf_tests/bench_runner.js
```

#### 4b. Write V8 correctness sanity test

**File:** `jit-tests/js/test_p43_v8bench.js`

Runs a few iterations of each V8 benchmark sub-suite and verifies no
exceptions are thrown (correctness, not scoring).

#### 4c. Update docs

- `jit-docs/phase43-v8bench.md`: replace "Expected Results" table with
  actual post-P43.2 measurements
- `jit_perf_tests/RESULTS_P43.md`: fill in all measured scores

#### 4d. Add to jit-tests Makefile

Add `test_p43_v8bench.js` to `jit-tests/js/Makefile` JS_TESTS list.

---

## What Was Dropped / Changed vs Original Plan

| Original step | Outcome |
|---------------|---------|
| P43.1 — two-pass shell harness | **Dropped**: `__jit_drain` JS builtin makes single-pass bench_runner.js work correctly even cold. A shell wrapper adds no value now. |
| P43.2 — quadrimorphic IC | **Done** (84f18cc). Also fixed P39.2 prop_arr correctness bug discovered during implementation. |
| P43.4 — Crypto bit-op audit | **Done** (d7e56be). INT type propagation for and/or/xor/shl/sar/not. |
| P43.5 — _tsv safety fix | **Done** (50e10e0). Initialize `_tsv` slots to `JS_UNDEFINED` for safe exception cleanup in INT fast paths. |
| P43.5 — Splay investigation | **Resolved**: warm-cache Splay = +7% vs interpreter. No code change needed. |
