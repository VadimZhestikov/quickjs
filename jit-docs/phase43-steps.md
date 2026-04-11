# Phase 43 Implementation Steps

## Prerequisites

- P41 committed (0d25a05) — early JIT bypass + slim IC fast-call
- V8 benchmark suite at `jit_perf_tests/v8bench/` with `run_qjs.js`
- Node.js available for reference scoring: `node --version`
- Cold-JIT baseline recorded in `phase43-v8bench.md`

## Step 1 — Two-Pass Measurement Harness (P43.1)

**Goal:** Separate JIT compilation overhead from benchmark execution time.
The first run compiles all hot functions into `.so` files; the second run
measures with those files cached.

### 1a. Write the shell runner

**File:** `jit_perf_tests/v8bench/run_bench.sh`

```bash
#!/bin/bash
# run_bench.sh — two-pass V8 benchmark runner
# Usage: ./run_bench.sh [path-to-qjs] [path-to-node]
set -e

QJS="${1:-../../qjs}"
NODE="${2:-node}"
DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== V8 Benchmark Suite ==="
echo "QJS:  $QJS"
echo "Node: $NODE"
echo ""

# --- Interpreter baseline ---
echo "--- No-JIT (interpreter) ---"
QJS_NOJIT="${QJS%qjs}qjs_nojit"
if [ -x "$QJS_NOJIT" ]; then
    cd "$DIR" && "$QJS_NOJIT" run_qjs.js 2>/dev/null | grep -E "^[A-Za-z]"
else
    echo "(qjs_nojit not found — skipping)"
fi
echo ""

# --- JIT: pass 1 (warm cache) ---
echo "--- JIT pass 1 (compile) ---"
cd "$DIR" && "$QJS" run_qjs.js 2>/dev/null | grep -E "^[A-Za-z]" | sed 's/^/  /'
echo ""

# --- JIT: pass 2 (measure with warm cache) ---
echo "--- JIT pass 2 (warm cache — use these numbers) ---"
cd "$DIR" && "$QJS" run_qjs.js 2>/dev/null | grep -E "^[A-Za-z]"
echo ""

# --- Node reference ---
echo "--- Node.js reference ---"
cd "$DIR" && "$NODE" run_node.js 2>/dev/null | grep -E "^[A-Za-z]"
```

Make it executable:
```sh
chmod +x jit_perf_tests/v8bench/run_bench.sh
```

### 1b. Add Makefile target

**File:** `jit_perf_tests/v8bench/Makefile` (create if absent)

```makefile
QJS    ?= ../../qjs
NODE   ?= node

.PHONY: bench bench-jit bench-node

bench:
	@bash run_bench.sh $(QJS) $(NODE)

bench-jit:
	@echo "--- JIT pass 1 ---"
	@$(QJS) run_qjs.js 2>/dev/null | grep -E "^[A-Za-z]"
	@echo "--- JIT pass 2 (warm) ---"
	@$(QJS) run_qjs.js 2>/dev/null | grep -E "^[A-Za-z]"

bench-node:
	@$(NODE) run_node.js 2>/dev/null | grep -E "^[A-Za-z]"
```

### 1c. Run and record warm-cache baseline

```sh
cd jit_perf_tests/v8bench
bash run_bench.sh ../../qjs node
```

Record the **pass 2** JIT scores in `jit_perf_tests/RESULTS_P43.md`:

```markdown
# P43 Benchmark Results

## Date
2026-04-11

## Build
git rev: $(git rev-parse --short HEAD)
make CONFIG_JIT=y

## V8 Benchmark (warm JIT cache = pass 2)

| Benchmark | No-JIT | JIT warm | Node v24 | JIT/Interp | Node/JIT |
|-----------|--------|----------|----------|-----------|---------|
| Richards  |        |          | 28175    |           |         |
| DeltaBlue |        |          | 63268    |           |         |
| ...       |        |          |          |           |         |
```

(Fill in the numbers from the run.)

### 1d. Verify Splay regression disappears (P43.5)

Run Splay in isolation with warm cache. If pass-2 Splay score ≥ interpreter
score, the regression is confirmed as a cold-start artifact and no code
change is needed for Splay.

```sh
# Run full suite twice; check pass-2 Splay score
cd jit_perf_tests/v8bench
../../qjs run_qjs.js 2>/dev/null | grep Splay  # pass 1
../../qjs run_qjs.js 2>/dev/null | grep Splay  # pass 2 (should be ≥ 2407)
```

If pass-2 Splay is still below 2407 (interpreter baseline), go to the
optional investigation in Step 1e.

### 1e. (Conditional) Splay warm-cache investigation

Only if Step 1d shows Splay is still slower with warm JIT:

```sh
# Find which JIT functions are compiled for Splay
cat > /tmp/splay_only.js << 'EOF'
var load = __loadScript;
var alert = print;
load('base.js');
load('splay.js');
BenchmarkSuite.RunSuites({
    NotifyResult: function(n,r){ print(n+': '+r); },
    NotifyError:  function(n,e){ print('ERROR '+n+': '+e); },
    NotifyScore:  function(s)  { print('Score: '+s); }
});
EOF
rm -f ~/.cache/qjs-jit/*
../../qjs /tmp/splay_only.js 2>/dev/null
# Check generated JIT code for splay_ function:
grep -l "splay_\|splay" ~/.cache/qjs-jit/*.c
```

Look in the generated `.c` file for `SplayTree_splay_` (or the anonymous
inner function name) and verify:
- Self-recursive call uses P8.2 direct call (`__jit_f_...` extern)
  rather than the IC path (`js_jit_ic_direct_call`)
- Property accesses on `SplayTreeNode.left/right` have IC hits (single
  shape, always-hit)

If the recursive call is using the IC path (missed P8.2), investigate
why `gen_st[d]` is not `JIT_T_SELF_FUNC` at the call site.

---

## Step 2 — Quadrimorphic IC (P43.2)

**Goal:** Extend the bimorphic IC (2 shape slots) to quadrimorphic (4 slots)
to cover the polymorphic property access patterns in DeltaBlue and Richards.

### 2a. Extend JSJITICEntry2 to 4 slots

**File:** `quickjs-jit.h`

Search for:
```c
typedef struct {
    JSJITICEntry e[2];
    int n;
} JSJITICEntry2;
```

Replace with:
```c
/* P43.2: extended from 2 to 4 shape slots (quadrimorphic).
 * n == 5 signals megamorphic (all slots exhausted). */
typedef struct {
    JSJITICEntry e[4];
    int n;
} JSJITICEntry2;
```

### 2b. Update IC fill functions in quickjs.c

**File:** `quickjs.c`

Find `js_jit_ic2_fill_get` (the fill function for OP_get_field IC).
Update the megamorphic threshold from `n >= 2` to `n >= 4`:

```c
/* Before: */
if (ic->n >= 2) {
    /* megamorphic — stop caching */
    ic->n = 3;  /* sentinel */
    return;
}

/* After: */
if (ic->n >= 4) {
    /* megamorphic — stop caching */
    ic->n = 5;  /* sentinel: > 4 slots filled */
    return;
}
```

Similarly update `js_jit_ic2_fill_put`.

Verify the fill loop correctly handles indices 0–3:
```c
ic->e[ic->n].expected_shape = shape;
ic->e[ic->n].prop_arr       = JS_GetPropertyArray(p);  /* cached P39.2 */
ic->e[ic->n].slot           = slot;
ic->n++;
```

### 2c. Update the emitter to generate 4-arm IC chains

**File:** `quickjs-jit.c` — `gen_body()`, OP_get_field and OP_put_field sections

Currently the emitter hardcodes two arms. Change to emit 4 arms controlled
by a constant `JIT_IC_SLOTS = 4`:

```c
/* Define at file top or in a header */
#define JIT_IC_SLOTS 4
```

For `OP_get_field`, replace the current 2-arm emission with a loop:

```c
/* Arm 0 — always emitted, no slot-count check */
jit_buf_printf(cb,
    "      if(JIT_IC_CHECK_FAST(_o,&_ic%d.e[0])){\n"
    "        _r=_ic%d.e[0].prop_arr[_ic%d.e[0].slot];"
    " _DUP(_r); ...\n"
    "      }\n", pc, pc, pc);

/* Arms 1–3 — guarded by slot-count check */
for (int _slot = 1; _slot < JIT_IC_SLOTS; _slot++) {
    jit_buf_printf(cb,
        "      else if(_ic%d.n>=%d&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[%d])){\n"
        "        _r=_ic%d.e[%d].prop_arr[_ic%d.e[%d].slot];"
        " _DUP(_r); ...\n"
        "      }\n",
        pc, _slot + 1, pc, _slot,
        pc, _slot, pc, _slot);
}

/* Slow path */
jit_buf_printf(cb,
    "      else{ _r=_RT->get_prop(ctx,_o,%uu);"
    " js_jit_ic2_fill_get(ctx,_o,%uu,&_ic%d);"
    " _FREE(_o); _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
    atom, atom, pc, d-1, d);
```

The same pattern applies to `OP_put_field` (4-arm chain for write IC).

### 2d. Update static IC initializers in generated code

The emitter generates:
```c
static JSJITICEntry2 _ic42={NULL,NULL,...,0,NULL,...,0,0};
```

The zero-initializer must cover all 4 IC entries. Since it's a static
struct with all-zero init, `{0}` suffices:

```c
/* Change emitter to emit: */
"      static JSJITICEntry2 _ic%d={0};\n"
```

(Was previously listing fields explicitly; `{0}` is cleaner and covers
any struct size change automatically.)

### 2e. Build and verify

```sh
make CONFIG_JIT=y -j$(nproc)
# Clear cache so new IC structure takes effect
rm -f ~/.cache/qjs-jit/*
# Run P40 and P41 tests to verify no regressions
make -C jit-tests/P40 run
make -C jit-tests/P41 run
# Run V8 bench (two passes) and compare to Step 1 baseline
cd jit_perf_tests/v8bench
bash run_bench.sh ../../qjs node
```

Verify DeltaBlue and Richards scores improved. If the IC struct change
causes build errors, check that all references to `_ic.n >= 2` (megamorphic
check in fill functions) were updated to `>= 4`.

### 2f. Write P43.2 regression test

**File:** `jit-tests/js/test_p43_ic.js`

```js
// P43.2: quadrimorphic IC — 4 distinct shapes must all hit
function test_quad_ic() {
    function A(v) { this.a = v; }
    function B(v) { this.b = v; this.a = v * 2; }  // different shape
    function C(v) { this.c = v; this.a = v * 3; }
    function D(v) { this.d = v; this.a = v * 4; }

    var objs = [new A(1), new B(1), new C(1), new D(1)];
    var sum = 0;
    // Same access site, 4 different shapes — must not go megamorphic
    for (var i = 0; i < 200; i++) {
        for (var j = 0; j < objs.length; j++) sum += objs[j].a;
    }
    if (sum !== (1 + 2 + 3 + 4) * 200) throw new Error("wrong: " + sum);
    print("P43.2 quad IC: OK");
}
test_quad_ic();
```

Run:
```sh
./qjs jit-tests/js/test_p43_ic.js
```

---

## Step 3 — Crypto Bit-Op Audit (P43.4)

**Goal:** Verify (and fix if needed) that all integer bit operations emit
native `int64_t` code in the JIT output.

### 3a. Create an isolated Crypto-style benchmark

```sh
cat > /tmp/crypto_probe.js << 'EOF'
function safe_add(x, y) {
    var lsw = (x & 0xFFFF) + (y & 0xFFFF);
    var msw = (x >> 16) + (y >> 16) + (lsw >> 16);
    return (msw << 16) | (lsw & 0xFFFF);
}
var s = 0;
for (var i = 0; i < 1000000; i++) s = safe_add(s, i);
print(s);
EOF
rm -f ~/.cache/qjs-jit/*
./qjs /tmp/crypto_probe.js
```

### 3b. Inspect generated JIT code for the safe_add function

```sh
# Find the file for safe_add (look for &, >>, <<, | operations)
grep -l "0xFFFF\|lsw\|msw\|<<\|>>" ~/.cache/qjs-jit/*.c | head -3
cat ~/.cache/qjs-jit/<hash>.c
```

**Expected (good — all int64_t):**
```c
_ti0 = (int64_t)(int32_t)(_ti1 & 0xFFFF);   /* & with int fast path */
_ti2 = _ti0 + _ti3;                           /* native int add */
_ti4 = (int64_t)(int32_t)(_ti5 >> 16);        /* sar with int fast path */
```

**If any bit-op emits JSValue path (bad):**
```c
JSValue _r = _RT->and_(ctx, _tsv0, _tsv1);    /* vtable call — slow */
```

### 3c. Fix missing JIT_T_INT propagation (if needed)

**File:** `quickjs-jit.c` — scan pass (the pre-pass that assigns `local_type[]`)

For each affected opcode in the `_TI_SCAN` loop, ensure it pushes `JIT_T_INT`:

```c
/* OP_and, OP_or, OP_xor: ToInt32 result → always INT */
case OP_and:
case OP_or:
case OP_xor:
    _TI_POP(); _TI_POP(); _TI_PUSH(JIT_T_INT); break;

/* OP_shl, OP_sar: ToInt32 result → always INT */
case OP_shl:
case OP_sar:
    _TI_POP(); _TI_POP(); _TI_PUSH(JIT_T_INT); break;

/* OP_sar1 (unsigned right shift, >>>): ToUint32 → INT */
case OP_sar1:
    _TI_POP(); _TI_POP(); _TI_PUSH(JIT_T_INT); break;
```

Also verify the **code generator** (gen_body) emits the fast path when both
operands are JIT_T_INT:

```c
case OP_and:
    if (gen_st[d-1] == JIT_T_INT && gen_st[d-2] == JIT_T_INT) {
        _P94_ENSURE(d-2); /* should already be int; this is a no-op */
        jit_buf_printf(cb,
            "    { _ti%d=(int64_t)(int32_t)(_ti%d&_ti%d); _sp=%d; }\n",
            d-2, d-2, d-1, d-1);
        /* push JIT_T_INT */
    } else {
        /* slow path: _RT->and_() */
    }
```

### 3d. Build, verify, benchmark

```sh
make CONFIG_JIT=y -j$(nproc)
rm -f ~/.cache/qjs-jit/*
./qjs /tmp/crypto_probe.js
# Inspect generated code again — should now show _ti* everywhere
grep "_tsv\|_RT->" ~/.cache/qjs-jit/*.c | grep -v "^.*:" | wc -l
# Should be 0 vtable calls for integer-only functions
```

Run V8 bench Crypto specifically:
```sh
cat > /tmp/crypto_only.js << 'EOF'
var load = __loadScript;
var alert = print;
load('base.js'); load('crypto.js');
BenchmarkSuite.RunSuites({
    NotifyResult: function(n,r){ print(n+': '+r); },
    NotifyError:  function(n,e){ print('ERROR: '+e); },
    NotifyScore:  function(s){ print('Score: '+s); }
});
EOF
cd jit_perf_tests/v8bench
../../qjs /tmp/crypto_only.js 2>/dev/null  # pass 1
../../qjs /tmp/crypto_only.js 2>/dev/null  # pass 2 (warm)
```

---

## Step 4 — Write Correctness Tests (P43.6)

**File:** `jit-tests/js/test_p43_v8bench.js`

Add a quick sanity test that each V8 benchmark produces the expected result:

```js
// Sanity: verify V8 benchmarks produce correct output after JIT
// (Scores are timing-dependent; correctness is not.)
// Run from quickjs/ with: ./qjs jit-tests/js/test_p43_v8bench.js

var load = __loadScript;
var alert = print;

// Each benchmark suite exposes a .Setup() and a single .run() iteration.
// We call it a few times and check results don't throw.
load('jit_perf_tests/v8bench/base.js');
load('jit_perf_tests/v8bench/richards.js');
load('jit_perf_tests/v8bench/deltablue.js');
load('jit_perf_tests/v8bench/crypto.js');
load('jit_perf_tests/v8bench/raytrace.js');
// (earley-boyer and regexp are slow to setup; skip in quick test)

var passed = 0, failed = 0;
BenchmarkSuite.suites.forEach(function(suite) {
    try {
        suite.benchmarks.forEach(function(b) {
            b.Setup();
            b.run();
            b.run();
            b.TearDown();
        });
        print("OK: " + suite.name);
        passed++;
    } catch(e) {
        print("FAIL: " + suite.name + " — " + e);
        failed++;
    }
});
print("P43 sanity: " + passed + " passed, " + failed + " failed");
if (failed > 0) throw new Error("P43 sanity failed");
```

---

## Step 5 — Record Results and Commit

### 5a. Record warm-cache results

Fill in `jit_perf_tests/RESULTS_P43.md` with the pass-2 scores from
Step 1c plus the post-optimization scores from Steps 2–3.

### 5b. Update `phase43-v8bench.md`

Replace the "Expected Results" table with actual measurements.

### 5c. Commit

```sh
git add jit_perf_tests/v8bench/run_bench.sh \
        jit_perf_tests/v8bench/Makefile \
        jit_perf_tests/RESULTS_P43.md \
        jit-tests/js/test_p43_ic.js \
        jit-tests/js/test_p43_v8bench.js \
        quickjs-jit.h quickjs-jit.c quickjs.c \
        jit-docs/phase43-v8bench.md \
        jit-docs/phase43-steps.md

git commit -m "jit: P43 — V8 bench profiling: quadrimorphic IC + Crypto int audit"
```

---

## Verification Checklist

After each step, verify:

| Check | Command |
|-------|---------|
| Build clean | `make CONFIG_JIT=y -j$(nproc) 2>&1 \| grep error:` |
| P40 tests pass | `make -C jit-tests/P40 run` |
| P41 tests pass | `make -C jit-tests/P41 run` |
| P43 IC test passes | `./qjs jit-tests/js/test_p43_ic.js` |
| Splay warm ≥ 2407 | Two-pass run, check Splay pass-2 score |
| DeltaBlue improved | Warm-pass score > 900 |
| Crypto improved | Warm-pass score > 1200 |
| No new slowdowns | All benchmark scores ≥ their Step-1 baselines |

## Implementation Notes

### Why quadrimorphic IC?

DeltaBlue's constraint graph has `Constraint` objects of 4+ concrete types
(BinaryConstraint, UnaryConstraint, EditConstraint, etc.) all inheriting
from the same prototype. Method dispatch on `this.execute()` sees 4+ shapes
at the same call site. The bimorphic IC fills after 2 shapes and goes
megamorphic, paying `JS_GetPropertyInternal` on every subsequent call.

V8 handles this with a fully polymorphic inline cache (PIC) backed by a
hash table — it caches up to 8 shapes. Our quadrimorphic IC covers the
most common case with minimal code size increase (2 extra shape slots =
2 × 80B = 160B per IC entry, acceptable).

### Why not larger than 4 slots?

4 slots covers the DeltaBlue case. Going to 8 would double the struct size
and increase the generated C code size for each IC site. The diminishing
returns kick in quickly: if a site sees > 4 shapes, it's probably
legitimately megamorphic and the slow path is correct.

### Cold-start vs warm measurement

The V8 benchmark framework was designed for V8 where JIT compilation is
synchronous and transparent. QuickJS's async GCC compilation creates a
cold-start overhead that the framework doesn't account for. The two-pass
approach is the correct fix — it matches how QuickJS would behave in a
server scenario where functions are JIT-compiled after startup.

### Splay regression prediction

With a warm JIT cache, Splay should score ~2600–2800 (vs interpreter 2407
and Node 29883). The JIT benefits from:
- P40: `_vrp0` direct var_ref access (no vtable call)
- P41.1: early bypass for small recursive calls
- Property IC hitting 100% of the time (all SplayTreeNode objects
  have the same shape)

The remaining gap vs Node (29883) is mostly from object allocation speed
and GC overhead — every `SplayTreeNode` allocation pays the JS allocator.
