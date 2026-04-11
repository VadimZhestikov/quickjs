# Phase 43: V8 Benchmark Profiling + Targeted Fixes

## Baseline Data (2026-04-11, after P41)

Three runs collected. `qjs` = current build (P41, warm JIT cache).
`qjs_nojit` = interpreter only. `qjs_jit --jit-aot` = pre-P41 binary in AOT mode.

| Benchmark | Interpreter | JIT warm | JIT AOT (old) | Node v24 | Interp gap | JIT gap |
|-----------|-------------|----------|--------------|----------|-----------|---------|
| Richards | 1058 | 1056 | 1030 | 28175 | 27× | 27× |
| DeltaBlue | 941 | 933 | 889 | 63268 | 67× | 68× |
| Crypto | 1245 | 1216 | 1195 | 36072 | 29× | 30× |
| RayTrace | 1322 | 1362 | 1259 | 67043 | 51× | 49× |
| EarleyBoyer | **1707** | **1632** | 1631 | 54824 | 32× | 34× |
| RegExp | 441 | 435 | 448 | 8397 | 19× | 19× |
| Splay | 2648 | **2825** | 2573 | 29883 | 11× | 11× |
| **Score** | **1184** | **1184** | **1146** | **34595** | **29×** | **29×** |

*Cold JIT (2026-04-10, fresh cache):* 993 overall — 16% below warm due to GCC
 compilation competing with the benchmark main thread during the 1000 ms window.

## Root Cause Analysis

### Finding 1: Warm JIT score = interpreter score exactly (1184 = 1184)

The JIT provides **zero net benefit** on the V8 benchmark suite when the cache
is warm. IC overhead in the JIT path roughly cancels the savings from skipping
bytecode dispatch. Per-benchmark detail:

| Benchmark | JIT delta | Explanation |
|-----------|-----------|-------------|
| RayTrace | **+3%** | Float arithmetic: JIT_T_NUMBER paths remove some JSValue boxing |
| Splay | **+7%** | Property IC hits 100% (single shape), P41.1 closure call bypass |
| Richards | **−0%** | Tied — IC overhead ≈ dispatch savings |
| DeltaBlue | **−1%** | IC overhead marginally worse than interpreter on poly sites |
| Crypto | **−2%** | Surprise: JIT slightly slower. See Finding 3. |
| RegExp | **−1%** | Dominated by regexp engine, JIT irrelevant |
| EarleyBoyer | **−4%** | JIT slower — see Finding 4 |

### Finding 2: Splay warm-cache regression is fully resolved

Cold JIT scored 1964 (−18% vs interpreter). Warm JIT scores 2825 (+7% vs
interpreter). This confirms the cold-start regression was **entirely a
measurement artifact** of GCC background compilation competing with the main
thread during the 1000ms timing window. No code fix needed for Splay.

### Finding 3: Crypto JIT is 2% slower than interpreter (warm cache)

This is unexpected given Crypto uses integer bit operations where `JIT_T_INT`
should produce native int64_t arithmetic. Possible causes:
- The hot functions exceed the JIT threshold but are not the innermost loops
- `safe_add` and similar functions are called from an outer loop that IS
  JIT-compiled, but the JIT call overhead (IC + arg dup) offsets the savings
- Some bit operations may not be hitting the `JIT_T_INT` fast path

**Action required:** Inspect generated JIT `.c` for Crypto's hot functions
(Step 3 of implementation). If the fast paths are correct, the issue is
call-site overhead and P43.2 (IC) is the fix.

### Finding 4: EarleyBoyer JIT is 4% slower than interpreter (warm cache)

EarleyBoyer scores 1632 JIT vs 1707 interpreter (−4%). This is a real
regression, not a cold-start artifact. EarleyBoyer exercises:
- Heavy closure creation and calls (P41.1 helps, but call IC overhead remains)
- List traversal via property access on cons-cell-style objects
- String operations

The likely cause: EarleyBoyer's functions are moderately sized (20–50 opcodes),
not tiny. For medium-sized functions the call setup overhead is amortized, but
the bimorphic IC on list traversal (`car`, `cdr` on different object types) goes
megamorphic and falls back to `_RT->get_prop()` on every access after 2 shapes.
The interpreter's IC handles this gracefully; the JIT's does not.

**This makes P43.2 (quadrimorphic IC) the highest-priority fix** — it directly
addresses both the EarleyBoyer regression and the DeltaBlue stall.

### Finding 5: JIT AOT mode is consistently slower than warm JIT

`qjs_jit --jit-aot` scores 1146 vs warm JIT 1184 (−3%). AOT mode pre-compiles
all functions before the benchmark starts (no cold-start overlap). Despite this,
it scores lower. This suggests the old `qjs_jit` binary (pre-P41) generates
slightly worse code — P41.1 + P40 improvements in the current `qjs` binary
actually matter even for non-closure-heavy workloads.

### Finding 6: RegExp gap is the regexp engine, not the JIT

JIT: 435, interpreter: 441, Node: 8397. The JIT cannot help the regexp engine
(`libregexp.c`). **P43 does not target RegExp.**

## Sub-phases

### P43.1 — Two-Pass Measurement Harness

**Problem:** Cold-first-run numbers conflate JIT compilation overhead with
benchmark performance. Splay's "regression" disappears on a warm cache.

**Solution:** Write a shell script and a modified `run_qjs.js` that:
1. **Pass 1:** run the full suite once at a reduced iteration ceiling to trigger
   JIT compilation of all hot functions; discard scores
2. **Pass 2:** run the full suite again with the `.so` files cached; report scores

**Files:**
- `jit_perf_tests/v8bench/run_bench.sh` — shell wrapper
- `jit_perf_tests/v8bench/run_qjs_warm.js` — modified runner with
  `BenchmarkSuite.runCount = 2; BenchmarkSuite.reportRun = 2;`

**Expected improvement:** Splay goes from 1964 → ≥ 2407 (≥ baseline).
Overall JIT score rises from ~993 to ~1200–1400 (warm cache reflects true JIT speed).

---

### P43.2 — Trimorphic IC for Property Access

**Problem:** Bimorphic IC (2 shape slots) is insufficient for polymorphic call
sites in DeltaBlue. Constraint objects of different types appear at the same
access site. After 2 distinct shapes, every access misses and calls
`_RT->get_prop()` (slow path). The interpreter never fully megamorphizes — it
re-probes on every miss with a shape cache that covers 4 slots.

**Solution:** Extend `JSJITICEntry2` from 2 to 4 shape slots (quadrimorphic).

```c
/* quickjs-jit.h — before */
typedef struct {
    JSJITICEntry e[2];
    int n;
} JSJITICEntry2;

/* quickjs-jit.h — after */
typedef struct {
    JSJITICEntry e[4];  /* P43.2: quadrimorphic */
    int n;
} JSJITICEntry2;
```

The generated IC hot path extends from 2 arms to 4:
```c
/* Generated C — before (2 slots): */
if      (JIT_IC_CHECK_FAST(_o, &_ic.e[0])) { fast-path with e[0]; }
else if (_ic.n >= 2 && JIT_IC_CHECK_FAST(_o, &_ic.e[1])) { fast-path with e[1]; }
else    { slow-path + fill; }

/* Generated C — after (4 slots): */
if      (JIT_IC_CHECK_FAST(_o, &_ic.e[0])) { fast-path with e[0]; }
else if (_ic.n >= 2 && JIT_IC_CHECK_FAST(_o, &_ic.e[1])) { fast-path with e[1]; }
else if (_ic.n >= 3 && JIT_IC_CHECK_FAST(_o, &_ic.e[2])) { fast-path with e[2]; }
else if (_ic.n >= 4 && JIT_IC_CHECK_FAST(_o, &_ic.e[3])) { fast-path with e[3]; }
else    { slow-path + fill; }
```

`js_jit_ic2_fill_get` / `js_jit_ic2_fill_put` must be updated to fill up to 4
slots before going megamorphic (`_ic.n = 5` = megamorphic sentinel).

The emitter also needs updating: currently emits a hardcoded 2-arm chain;
instead emit a loop over `n_slots` (set to 4 at emit time).

**Files changed:** `quickjs-jit.h` (struct), `quickjs-jit.c` (emitter + fill
functions), `quickjs.c` (fill implementations).

**Expected improvement:** DeltaBlue: +15–25%. Richards: +5–10%.
(DeltaBlue has the most polymorphic access sites.)

**Struct size cost:** Each `JSJITICEntry2` grows from 2×(~80 bytes) + 4 = 164 B
to 4×80 + 4 = 324 B. With 265 JIT functions and ~5 property-access ICs each,
total static data: ~265 × 5 × 324B ≈ 430 KB (vs 218 KB before). Acceptable.

---

### P43.3 — Get-Field IC: Eliminate DupValue for Integer-Typed Values

**Problem:** After an IC hit on `OP_get_field`, the JIT always calls
`JS_DupValue(ctx, _r)` before returning the result. For integer-valued
properties (counters, loop indices, sizes), this is a no-op at runtime
(integers have no refcount) but still executes the conditional branch:

```c
/* JS_DupValue for an integer JSValue (always taken-not branch): */
if (JS_VALUE_HAS_REF_COUNT(v)) { /* tag < 0: not taken for ints */
    p->ref_count++;
}
```

The branch is correctly predicted but still burns ~2 cycles.

**Solution:** After an IC hit, check if the loaded value is known-integer
at type-inference time. If `gen_st[d-1] == JIT_T_INT` after the field load
(which requires the IC fill to capture type), skip the DupValue.

Actually this is hard to know statically. A better approach: make the
`JIT_IC_CHECK_FAST` fast path emit the value load + type check, and skip
DupValue inline if the tag is not < 0. Specifically:

```c
/* Generated C — after P43.3: */
if (JIT_IC_CHECK_FAST(_o, &_ic.e[0])) {
    _r = _ic.e[0].prop_arr[_ic.e[0].slot];
    if (JS_VALUE_HAS_REF_COUNT(_r)) JS_DupValue(ctx, _r);  /* inlined */
    ...
}
```

This is already what `JS_DupValue` expands to, but when inlined in the
generated C file GCC can hoist the tag check across loop iterations when
the type is constant. The key is ensuring GCC sees the full inline expansion
rather than a function call that it cannot optimize across.

**Note:** This optimization is already mostly present if `JS_DupValue` is
an inline function in `quickjs.h`. Verify it's actually inlining.
If GCC is emitting a real call to `JS_DupValue` in the `.so`, adding
`__attribute__((always_inline))` to `JS_DupValue` will fix this.

**Files changed:** `quickjs.h` (inline attribute check), potentially
`quickjs-jit.c` (emit explicit inline expansion).

**Expected improvement:** 2–5% on property-access heavy benchmarks.

---

### P43.4 — Crypto + EarleyBoyer: Fix JIT_T_INT Propagation and IC Regressions

**Problem:** Warm-cache measurements (2026-04-11) show Crypto JIT is **2% slower**
than interpreter (1216 vs 1245), and EarleyBoyer JIT is **4% slower** (1632 vs 1707).
These are genuine regressions with the JIT active, not cold-start artifacts.

For Crypto, the hot functions use patterns like:

```js
function safe_add(x, y) {
    var lsw = (x & 0xFFFF) + (y & 0xFFFF);
    var msw = (x >> 16) + (y >> 16) + (lsw >> 16);
    return (msw << 16) | (lsw & 0xFFFF);
}
```

The JIT type inference tracks `JIT_T_INT` through `OP_add`, `OP_sub`, etc.
But `& 0xFFFF`, `>> 16`, `<< 16`, `|` operations use opcodes
`OP_and`, `OP_or`, `OP_shl`, `OP_sar` which convert operands to int32
(ToInt32 semantics). These should all push `JIT_T_INT`.

**Audit goal:** Confirm the JIT _actually_ emits native int64_t paths for
these opcodes (not JSValue arithmetic) by inspecting the generated `.c`
files for a Crypto-exercising function.

Expected: see `_ti{n}` variables (int64_t) rather than `_tsv{n}` (JSValue)
for the hot inner loops.

If any op emits the slow path (`_RT->and_()`, `_RT->or_()` etc.) for
integer-typed operands, those are bugs to fix by ensuring the correct
`JIT_T_INT` push in the type-inference scan pass.

**Files changed:** `quickjs-jit.c` (scan pass type rules for OP_and,
OP_or, OP_shl, OP_sar, OP_sar1, OP_lnot, OP_not).

**Expected improvement:** 2–5% on Crypto if bit ops are currently missing
the fast path; fixing EarleyBoyer IC megamorphism is shared with P43.2.

**Note:** For EarleyBoyer the fix is primarily P43.2 (quadrimorphic IC).
EarleyBoyer's cons-cell objects (`car`, `cdr`) appear in 3–5 shapes at
the same access site — exactly the megamorphic IC miss scenario.

---

### P43.5 — Splay: ✓ Confirmed Not a Problem

Warm-cache measurement (2026-04-11) shows Splay JIT = 2825 vs interpreter
2648 (+7%). The cold-start regression (1964, −18%) was entirely a measurement
artifact. **No code change needed for Splay.**

---

### P43.6 — Tests + Docs + Benchmark Recording

1. Record warm-JIT V8 benchmark scores in a new file
   `jit_perf_tests/RESULTS_P43.md`
2. Add a `make v8bench` target to `jit_perf_tests/v8bench/Makefile` that
   runs the two-pass harness
3. Add correctness sanity tests to `jit-tests/js/` that verify the V8
   benchmark suite produces correct output (not just correct scores)

---

## Actual Warm-Cache Results (2026-04-11)

Collected after P41, JIT cache already warm (second run):

| Benchmark | Interpreter | JIT warm | JIT vs Interp | Node v24 | Gap vs Node |
|-----------|-------------|----------|--------------|----------|-------------|
| Richards | 1058 | 1056 | −0% | 28175 | 27× |
| DeltaBlue | 941 | 933 | −1% | 63268 | 68× |
| Crypto | 1245 | 1216 | **−2%** | 36072 | 30× |
| RayTrace | 1322 | 1362 | +3% | 67043 | 49× |
| EarleyBoyer | 1707 | 1632 | **−4%** | 54824 | 34× |
| RegExp | 441 | 435 | −1% | 8397 | 19× |
| Splay | 2648 | 2825 | +7% | 29883 | 11× |
| **Score** | **1184** | **1184** | **0%** | **34595** | **29×** |

**Key result: JIT = interpreter on the geometric mean.** The IC overhead in the
JIT path exactly cancels the bytecode dispatch savings. Two benchmarks show real
JIT regressions (Crypto −2%, EarleyBoyer −4%); two show real wins (RayTrace +3%,
Splay +7%).

## Expected Results After P43 Fixes

Starting from the warm-cache baseline (1184):

| Benchmark | JIT warm (now) | After P43.2 | After P43.4 | Node v24 |
|-----------|---------------|------------|------------|---------|
| Richards | 1056 | ~1150 | ~1150 | 28175 |
| DeltaBlue | 933 | ~1100 | ~1100 | 63268 |
| Crypto | 1216 | ~1250 | ~1350 | 36072 |
| RayTrace | 1362 | ~1400 | ~1400 | 67043 |
| EarleyBoyer | 1632 | ~1800 | ~1800 | 54824 |
| RegExp | 435 | ~435 | ~435 | 8397 |
| Splay | 2825 | ~2900 | ~2900 | 29883 |
| **Score** | **1184** | **~1380** | **~1440** | **34595** |

P43.2 (quadrimorphic IC) is expected to be the dominant improvement (~17%
overall) by fixing the megamorphic IC miss on EarleyBoyer and DeltaBlue.
P43.4 (Crypto bit-op audit) is expected to add ~5% on Crypto specifically.

The remaining ~24× gap vs Node after P43 is structural:
1. **Object allocation**: QuickJS uses reference-counted GC; Node uses
   a generational heap with bump-pointer young-space allocation
2. **Unboxed value representation**: V8 uses tagged small integers (31-bit
   smi) stored directly in property slots; QuickJS uses 16-byte JSValues
3. **Native IC**: V8's ICs are machine code stubs; QuickJS JIT ICs are
   GCC-compiled C with one additional indirection layer
4. **Type specialization**: V8 compiles separate versions per observed type;
   QuickJS JIT emits a single version handling all types via branches

These require architectural changes beyond P43 scope.

## Priority Order for Implementation

1. **P43.1** (measurement harness) — `run_bench.sh` two-pass script; done
   conceptually since we now have warm-cache data
2. **P43.2** (quadrimorphic IC) — highest impact: fixes EarleyBoyer −4%
   regression + DeltaBlue stall; ~17% overall gain expected
3. **P43.4** (Crypto + EarleyBoyer bit-op audit) — investigate Crypto −2%;
   inspect generated `.c` for vtable calls where `_ti*` should appear
4. **P43.3** (DupValue inlining) — verify `JS_DupValue` inlines in `.so`
5. **P43.6** (tests + results recording) — always last
