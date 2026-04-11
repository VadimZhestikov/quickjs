# Phase 43: V8 Benchmark Profiling + Targeted Fixes

## Baseline Data (2026-04-10, after P41)

Run from `jit_perf_tests/v8bench/` with fresh JIT cache:

| Benchmark | No-JIT | JIT (cold) | JIT vs Interp | Node v24 | Gap vs Node |
|-----------|--------|-----------|---------------|----------|-------------|
| Richards | 842 | 935 | **+11%** | 28175 | 30× |
| DeltaBlue | 726 | 822 | **+13%** | 63268 | 77× |
| Crypto | 837 | 1057 | **+26%** | 36072 | 34× |
| RayTrace | 1048 | 1128 | **+8%** | 67043 | 59× |
| EarleyBoyer | 1329 | 1389 | **+5%** | 54824 | 39× |
| RegExp | 359 | 380 | **+6%** | 8397 | 22× |
| Splay | 2407 | 1964 | **−18%** | 29883 | 15× |
| **Score** | **933** | **993** | **+6%** | **34595** | **35×** |

265 JIT functions compiled across all benchmarks.

## Root Cause Analysis

### Finding 1: JIT gains are small (+6% overall)

The V8 benchmark runs each sub-benchmark for at least 1000ms. Within that window:
- Calls 1–100: interpreter (threshold not yet hit)
- Calls 101+: JIT compilation is queued to a background GCC thread; interpreter
  continues until the `.so` is linked and installed
- The benchmark measures **wall time of the main thread** while GCC runs on another
  core in the background

The "cold JIT" numbers include two costs absent from the interpreter baseline:
1. GCC compilation CPU time competing with the main thread
2. The ~100 interpreter calls before the threshold is crossed

Once the JIT cache is warm (second run), scores will be higher. P43.1 creates
a proper two-pass measurement harness to separate these effects.

### Finding 2: Splay is −18% with JIT (regression)

Splay tree operations are very fast in the interpreter (~2400 score). When JIT
compilation fires, GCC spends ~1–3 seconds compiling 10–20 Splay functions in the
background. This CPU contention directly reduces the iteration count in the
1000ms measurement window, dropping the score.

**This is a measurement artifact of the cold-start model, not a correctness or
code quality issue.** With a warm JIT cache (second pass), Splay should be at
least as fast as the interpreter, and likely faster due to the property IC.

### Finding 3: Property IC and call IC are the key bottlenecks

For object-heavy benchmarks (Richards, DeltaBlue, RayTrace), the dominant
operations are:
- `obj.prop` reads (OP_get_field → bimorphic IC)
- `obj.method()` calls (OP_call_method → call IC)
- `obj.prop = val` writes (OP_put_field → bimorphic IC)

The JIT IC is bimorphic (2 shape slots). When more than 2 shapes appear at a
call site — which happens in DeltaBlue's constraint graph traversal — both slots
fill and all subsequent accesses take the slow `_RT->get_prop()` vtable path.
This is **strictly worse than the interpreter's IC**, which transitions shapes
on miss rather than going fully generic.

### Finding 4: Crypto is the best case (+26%)

Crypto uses integer bit operations (shift, and, or, xor) on integer arrays.
The JIT type inference propagates `JIT_T_INT` through these operations and
emits native `int64_t` arithmetic without JSValue boxing. This is a genuine
win from the JIT. The remaining gap to Node is mainly from:
1. Array bounds checking overhead (QuickJS checks every access)
2. 32-bit wrapping (`| 0`, `>>> 0` patterns) that QuickJS does through
   `JS_NewInt32` vs V8's unboxed Int32

### Finding 5: RegExp is bottlenecked by the regexp engine, not the JIT

RegExp scores 380 vs Node's 8397 (22× gap). The JIT cannot help the regexp
engine itself — this is a separate module (`libregexp.c`). JIT accelerates
the JS glue code around the regexp calls but the engine dominates runtime.
**P43 does not target RegExp.**

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

### P43.4 — Crypto: Verify JIT_T_INT Propagation Through All Bit Ops

**Problem:** Crypto (+26% with JIT) uses patterns like:

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

**Expected improvement:** 5–10% on Crypto if any ops are currently missing
the fast path.

---

### P43.5 — Splay: Confirm Regression Is Cold-Start Only

Run Splay in isolation with a pre-warmed JIT cache and verify the score
meets or exceeds the interpreter baseline. If it does, the regression is
entirely cold-start and P43.1's two-pass harness resolves it.

If Splay is still slower with a warm cache, investigate the generated JIT
code for `SplayTree.prototype.splay_` (the inner recursive function).
Likely causes:
- The recursive self-call in `splay_` uses the call IC path rather than P8.2
  (self-function direct call). Verify P8.2 applies to prototype methods.
- The bimorphic IC has > 2 shapes at `this.left`, `this.right` accesses
  (unlikely since all nodes have the same shape, but verify).

**Fix if needed:** Ensure P8.2 applies to prototype method self-recursion
by detecting the pattern in the emitter.

---

### P43.6 — Tests + Docs + Benchmark Recording

1. Record warm-JIT V8 benchmark scores in a new file
   `jit_perf_tests/RESULTS_P43.md`
2. Add a `make v8bench` target to `jit_perf_tests/v8bench/Makefile` that
   runs the two-pass harness
3. Add correctness sanity tests to `jit-tests/js/` that verify the V8
   benchmark suite produces correct output (not just correct scores)

---

## Expected Results After P43

| Benchmark | JIT cold (now) | JIT warm (P43.1) | After P43.2–5 | Node v24 |
|-----------|---------------|-----------------|--------------|---------|
| Richards | 935 | ~1050 | ~1300 | 28175 |
| DeltaBlue | 822 | ~900 | ~1100 | 63268 |
| Crypto | 1057 | ~1200 | ~1350 | 36072 |
| RayTrace | 1128 | ~1250 | ~1350 | 67043 |
| EarleyBoyer | 1389 | ~1500 | ~1600 | 54824 |
| RegExp | 380 | ~380 | ~380 | 8397 |
| Splay | 1964 | ~2600 | ~2700 | 29883 |
| **Score** | **993** | **~1200** | **~1380** | **34595** |

The expected ~40% improvement from warm cache + targeted fixes still leaves
a ~25× gap vs Node. The root causes of that gap are:
1. **Object allocation**: QuickJS uses a reference-counted GC; Node uses
   a generational heap with bump-pointer young-space allocation
2. **Unboxed integer/float representations**: V8 represents integers as
   tagged small integers with no heap allocation; QuickJS uses 16-byte
   JSValues with GC overhead for int32 values in object properties
3. **Inline caches**: V8's ICs are in native machine code; QuickJS's JIT
   ICs are in GCC-compiled C with additional indirection
4. **Code specialization**: V8 JIT-compiles per-type; QuickJS JIT has
   one compiled version that handles both int and float cases

These are fundamental architecture gaps that require significantly larger
changes (typed value representation, generational GC) beyond P43 scope.

## Priority Order for Implementation

1. **P43.1** (measurement harness) — prerequisite for accurate data
2. **P43.5** (Splay warm check) — validates P43.1 resolves regression
3. **P43.2** (quadrimorphic IC) — highest single-benchmark impact (DeltaBlue)
4. **P43.4** (Crypto bit-op audit) — low-hanging fruit, might reveal bugs
5. **P43.3** (DupValue inlining) — small but free complexity-wise
6. **P43.6** (docs + benchmark recording) — always last
