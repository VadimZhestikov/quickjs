# P44 Performance Results — Mixed-Type Arithmetic Fast Paths

## Setup

- **Baseline (P43)**: `qjs_jit` binary (pre-P44 build)
- **P44**: `qjs` binary built with `CONFIG_JIT=y` including P44 changes
- **JIT threshold**: `--jit-threshold-gcc=1` (measure warm-cache JIT performance)
- **Platform**: Linux (WSL2), x86-64

## bench_runner.js Results (warm cache)

| Benchmark | P43 (ms) | P44 (ms) | Speedup | Notes |
|-----------|----------|----------|---------|-------|
| fib(30) | 35 | 34 | 1.0x | No change — INT path same speed |
| sum_loop(1e6) | 41 | 19 | **2.2x** | P44.4: `i < n` INT comparison avoids double |
| sum_sq(1e6) | 40 | 13 | **3.1x** | P44.4: `i <= n` INT comparison + INT mul/add |
| arr_sum(10000) x1e3 | 55 | 54 | 1.0x | `s` stays JSVAL-typed; minimal P44 effect |

## P44-Specific Microbenchmarks (warm cache)

| Benchmark | P43 (ms) | P44 (ms) | Speedup | Key Optimization |
|-----------|----------|----------|---------|-----------------|
| arr_sum (1000 reps) | 84 | 89 | ~1.0x | P44.2: JSVAL+JSVAL add (accumulator stays JSVAL) |
| count_to(1e6) (100 reps) | 183 | 68 | **2.7x** | P44.4: INT < JSVAL comparison — INT fast path |
| dot_product(10k) (1000 reps) | 182 | 185 | ~1.0x | Both operands NUMBER-typed; already on fast path |

## Analysis

### What Drives the Speedup

**P44.4 (comparison fast path)** is responsible for the main gains. In loops like:
```js
for (let i = 0; i < n; i++) ...    // i=INT, n=JSVAL
for (let i = 1; i <= n; i++) ...   // i=INT, n=JSVAL
```
P43 used a JSVAL×JSVAL comparison that checked both operand tags every iteration.
P44 detects `INT typed < JSVAL` at codegen time and emits an integer-only branch:
```c
// P44 generated code for i < n (INT i, JSVAL n):
if(js_likely(_tb==JS_TAG_INT)){
    _cond=((int32_t)_ti0 < JS_VALUE_GET_INT(_b));  // direct integer compare
} else if(_tb==JS_TAG_FLOAT64){
    _cond=((double)_ti0 < JS_VALUE_GET_FLOAT64(_b));
} else { /* slow vtable path */ }
```
This eliminates one tag check per iteration and avoids INT→double conversion entirely.

### Why arr_sum Doesn't Improve

The accumulator `s` in `arr_sum` is typed as JSVAL by the type inferencer (because
`OP_add(JSVAL_array_element, JSVAL_accumulator)` → JSVAL is conserved for string-safe
add). P44 does not change type inference for OP_add when one operand is JSVAL.
The array elements load as JSVAL, so the HALF_L add path doesn't apply here.

### Why dot_product Doesn't Improve

Both `a[i]` and `b[i]` are loaded as JSVAL array elements. After the first floating-point
multiplication, the type inferencer would normally give a NUMBER result — but since both
inputs are JSVAL in `local_type`, the fully-typed NUMBER×NUMBER path (P9.4) never fires.
The HALF_L mul path produces a JSVAL result in P44 (fixed to avoid rebox overhead), so
performance is the same as P43's JSVAL×JSVAL path.

### P44.3 sub/mul HALF_L Design Note

P44.3 added fast paths for `JSVAL op INT` (sub, mul) patterns. The initial implementation
stored the result as `double` (`_tsd`) which required re-boxing when the result was used
as a function argument (e.g., recursive calls `fib(n-1)`). This caused a ~25% regression
on `fib`. The fix emits a JSVAL result (`_tsv`) with an INT fast path that keeps integer
arithmetic as integers, avoiding any double conversion for integer-only code.

## Correctness

All correctness tests pass with P44:
- `make test` — 0 failures
- `tests/test_jit_p41_variadic.js` — all tests passed
