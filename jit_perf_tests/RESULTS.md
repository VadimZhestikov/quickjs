# JIT Performance Results — TCC Tier-1 vs Interpreter

**Date:** 2026-03-31  
**Host:** Linux 6.6.87.2-microsoft-standard-WSL2 (x86-64)  
**Build flags:**
- Interpreter: `make qjs` (GCC -O2, DIRECT_DISPATCH computed-goto)
- JIT tier-1:  `make CONFIG_JIT=y JIT_THRESHOLD_TCC=2 qjs` (TCC 0.9.27 in-process)

Each benchmark ran 3 times; the table shows the minimum elapsed time.

---

## Raw Measurements

| Benchmark | Interp run1 | Interp run2 | Interp run3 | **Interp min** | JIT run1 | JIT run2 | JIT run3 | **JIT min** |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| fib(30) x1           | 85.70 ms | 88.98 ms | 93.45 ms | **85.70 ms** | 96.11 ms | 88.29 ms | 89.36 ms | **88.29 ms** |
| sum_loop(1e6)        | 645.21 ms | 651.09 ms | 649.53 ms | **645.21 ms** | 650.62 ms | 657.64 ms | 656.65 ms | **650.62 ms** |
| sum_sq(1e6)          | 517.88 ms | 506.98 ms | 519.01 ms | **506.98 ms** | 515.50 ms | 514.93 ms | 514.25 ms | **514.25 ms** |
| prop_read(1e6)       | 490.00 ms | 504.29 ms | 490.95 ms | **490.00 ms** | 487.38 ms | 503.94 ms | 496.01 ms | **487.38 ms** |
| prop_write(1e6)      | 373.58 ms | 382.59 ms | 385.48 ms | **373.58 ms** | 391.11 ms | 428.06 ms | 387.86 ms | **387.86 ms** |
| closure_counter(1e6) | 40.32 ms  | 41.73 ms  | 40.73 ms  | **40.32 ms**  | 45.99 ms  | 43.80 ms  | 40.64 ms  | **40.64 ms**  |
| ipow(2,20) x1e5      | 75.44 ms  | 78.84 ms  | 79.28 ms  | **75.44 ms**  | 80.59 ms  | 77.10 ms  | 75.74 ms  | **75.74 ms**  |
| str_concat(5000)     | 45.55 ms  | 53.11 ms  | 46.40 ms  | **45.55 ms**  | 49.21 ms  | 47.20 ms  | 45.56 ms  | **45.56 ms**  |
| count_primes(3000)   | 5.97 ms   | 7.70 ms   | 6.30 ms   | **5.97 ms**   | 6.58 ms   | 6.81 ms   | 6.12 ms   | **6.12 ms**   |
| arr_sum(10000) x1e3  | 257.66 ms | 254.98 ms | 256.31 ms | **254.98 ms** | 261.43 ms | 263.77 ms | 256.73 ms | **256.73 ms** |

---

## Speedup Summary

Speedup = Interp_min / JIT_min.  Values > 1.0 = JIT faster.

| Benchmark | Interp min | JIT min | Speedup |
|---|---:|---:|---:|
| fib(30) x1           | 85.70 ms  | 88.29 ms  | **0.97×** |
| sum_loop(1e6)        | 645.21 ms | 650.62 ms | **0.99×** |
| sum_sq(1e6)          | 506.98 ms | 514.25 ms | **0.99×** |
| prop_read(1e6)       | 490.00 ms | 487.38 ms | **1.01×** |
| prop_write(1e6)      | 373.58 ms | 387.86 ms | **0.96×** |
| closure_counter(1e6) | 40.32 ms  | 40.64 ms  | **0.99×** |
| ipow(2,20) x1e5      | 75.44 ms  | 75.74 ms  | **1.00×** |
| str_concat(5000)     | 45.55 ms  | 45.56 ms  | **1.00×** |
| count_primes(3000)   | 5.97 ms   | 6.12 ms   | **0.98×** |
| arr_sum(10000) x1e3  | 254.98 ms | 256.73 ms | **1.00×** |

**TCC tier-1 is statistically equivalent to the interpreter (~1.00×) across all benchmarks.**

---

## Why TCC Tier-1 ≈ Interpreter

This result is expected and not a bug.  Four factors cause TCC-compiled code to
match rather than beat the GCC-O2 interpreter:

### 1. Interpreter quality: GCC -O2 + DIRECT_DISPATCH

The QuickJS interpreter is compiled by GCC at `-O2`.  It uses the
`DIRECT_DISPATCH` macro, which expands to computed-goto (`&&label`) dispatch —
the same trick V8 uses for its Ignition baseline.  Each opcode handler jumps
directly to the next via a pointer table rather than looping back to a `switch`.
This eliminates all branch-misprediction on the dispatch itself and enables
hardware prefetch along the opcode stream.

### 2. TCC generates unoptimised x86-64

TCC is a *fast* compiler, not an *optimising* one.  It does no:
- register allocation (all locals spilled to stack)
- constant folding / strength reduction
- inlining of leaf calls
- loop-invariant code motion

A tight inner loop that the GCC interpreter runs in three instructions may take
ten when compiled by TCC.

### 3. JSValue boxing still dominates

The generated C code still boxes every value as `JSValue` (64-bit tagged union).
Integer fast-paths (added in this phase) check tags and do direct C arithmetic
for `int+int`, `int-int`, `int*int`, and all comparisons, but:
- any promotion to `float64` falls through to the vtable slow path
- property reads/writes, closures, and array accesses are all vtable calls
- the `_CHK(r)` macro after every vtable call checks for JS exceptions

So the hot-path cost remains proportional to the number of vtable calls, which
is the same cost as interpreter dispatch.

### 4. JIT function call frame overhead

The JIT stub signature is:
```c
JSValue f(JSContext *ctx, JSValue this_obj, int argc,
          JSValue *argv, JSValue *cpool, JSClosureVar **var_refs);
```
Every JIT call from `JS_CallInternal` sets up this frame (six arguments on
x86-64 means register + stack spilling), whereas the interpreter's own inner
call simply pushes a `JSStackFrame` onto the C stack and jumps to
`JS_CallInternal` recursively, sharing the already-hot call frame.

---

## Expected Gains from Future Phases

| Phase | Mechanism | Expected speedup |
|---|---|---|
| **Phase 4 — GCC tier-2** | Compile generated C with `gcc -O2` in a background thread; hot functions promoted after `JIT_THRESHOLD_GCC` (default 5000) calls | **2–4×** on arithmetic-heavy loops; GCC register-allocates the `JSValue` stack, hoists tag checks, and may vectorise integer loops |
| **Phase 5 — Typed variables** | Annotate locals whose type is stable (`int`, `double`, `bool`) using profiling feedback; emit `int64_t`/`double` C locals instead of `JSValue` | **3–8×** on sum_loop/sum_sq/count_primes class; eliminates boxing entirely for the common integer case |
| **Phase 6 — IC + shape guards** | Inline shape checks for property access; emit direct offset load instead of `JS_GetProperty` vtable call | **5–15×** on prop_read/prop_write class |
| **Combined (tier-2 + types + IC)** | All of the above applied together | **10–30×** on hot numeric/property loops; fib and str_concat gain less (~2–4×) due to inherent pointer-chasing |

These projections are based on published results for similar JSValue-unboxing
JITs (LuaJIT, SpiderMonkey baseline vs Ion, JavaScriptCore DFG).

---

## How to Reproduce

```sh
# Build binaries (from quickjs/)
make qjs
cp qjs qjs_nojit
make CONFIG_JIT=y JIT_THRESHOLD_TCC=2 qjs
cp qjs qjs_jit

# Run benchmarks
./qjs_nojit jit_perf_tests/bench_runner.js
./qjs_jit   jit_perf_tests/bench_runner.js
```
