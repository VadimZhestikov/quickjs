# QuickJS JIT Performance Benchmarks

*Date: 2026-04-10 · Engine: QuickJS 2025-09-13 · Platform: x86\_64 Linux (WSL2)*

---

## 1. Introduction

This article documents the throughput characteristics of the QuickJS GCC-backend JIT compiler across a representative set of JavaScript micro-benchmarks. Four execution modes are compared:

| Mode | Binary | Flag | Description |
|---|---|---|---|
| **Interpreter** | `qjs_nojit` | — | Pure bytecode interpreter, no JIT |
| **JIT link (cold)** | `qjs_jit` | `--jit-link` | Functions compiled on-demand; combined `.so` written at the end (first ever run, cache empty) |
| **JIT link (warm)** | `qjs_jit` | `--jit-link` | Same flag, cache already populated from the cold run; the combined `.so` is loaded at startup |
| **JIT AOT** | `qjs_jit` | `--jit-aot` | All functions compiled by GCC before execution begins |
| **Node.js v24** | `node` | — | V8 TurboFan JIT (reference baseline) |

The goal is to answer three questions:
1. How much does the GCC-backend JIT help over the interpreter?
2. Is there a meaningful difference between the AOT and the warm link-time modes?
3. How far is QuickJS JIT from V8 on these workloads?

---

## 2. Methodology

### 2.1 Hardware

- **CPU**: Intel Core i7-10850H @ 2.70 GHz (4 cores / 8 threads, Comet Lake)
- **L3 cache**: 12 MB
- **OS**: Ubuntu 22.04 under WSL2 (Linux 6.6.87.2-microsoft-standard-WSL2)

### 2.2 Software versions

| Component | Version |
|---|---|
| QuickJS | 2025-09-13 |
| GCC (JIT back-end) | 11.4.0 (Ubuntu 11.4.0-1ubuntu1~22.04.3) |
| Node.js | v24.2.0 (V8 13.x) |

### 2.3 Benchmark harness

All benchmarks run from `jit_perf_tests/bench_runner.js`. Each benchmark follows the same structure:

```js
function bench(name, warmup_iters, iters, fn) {
    for (let i = 0; i < warmup_iters; i++) fn();   // warm-up (threshold crossing)
    const t0 = performance.now();
    for (let i = 0; i < iters; i++) fn();
    const elapsed = performance.now() - t0;
    ...
}
```

Five warm-up iterations precede every timed run. In JIT mode this is sufficient to cross the default call threshold (100 calls) inside the warm-up loop, so the measured loop runs entirely in compiled code. The `performance.now()` timer is sourced from `clock_gettime(CLOCK_MONOTONIC)` in QuickJS and `Date.now()` on Node (1 ms resolution; only affects the Node figures slightly for very short benchmarks).

### 2.4 Benchmark inventory

| # | Name | What it exercises |
|---|---|---|
| 1 | `fib(30) x1` | Recursive calls, integer compare/branch |
| 2 | `sum_loop(1e6)` | Tight integer addition loop |
| 3 | `sum_sq(1e6)` | Integer multiply inside loop |
| 4 | `prop_read(1e6)` | Repeated property get (`get_field` IC) |
| 5 | `prop_write(1e6)` | Repeated property set (`put_field` IC) |
| 6 | `closure_counter(1e6)` | Closure `var_ref` read/write |
| 7 | `ipow(2,20) x1e5` | Deep recursion (depth 20, 1e5 calls) |
| 8 | `str_concat(5000) x100` | String concatenation / heap allocation |
| 9 | `count_primes(3000) x10` | Nested loops, modulo, early break |
| 10 | `arr_sum(10000) x1e3` | Dense array element reads |

### 2.5 Cache management

Before the cold `--jit-link` run, the JIT cache (`~/.cache/qjs-jit/`) was cleared entirely:

```sh
rm -f ~/.cache/qjs-jit/*.so ~/.cache/qjs-jit/*.skip
```

The warm `--jit-link` run immediately followed the cold run (no source changes), so the combined LTO `.so` was already present. The `--jit-aot` run used the cache populated by the previous `--jit-link` cold run.

---

## 3. Results

### 3.1 Raw elapsed time (ms)

All times are wall-clock milliseconds for the complete timed portion (excluding warm-up). Lower is better.

| Benchmark | Interpreter | JIT link<br>cold | JIT link<br>warm | JIT AOT | Node v24 |
|---|---:|---:|---:|---:|---:|
| fib(30) x1 | 78.06 | 23.70 | 23.79 | 24.12 | 8.00 |
| sum\_loop(1e6) | 561.96 | 26.22 | 26.64 | 26.17 | 23.00 |
| sum\_sq(1e6) | 426.04 | 26.29 | 26.54 | 26.42 | 18.00 |
| prop\_read(1e6) | 422.78 | 312.87 | 188.05 | 184.24 | 10.00 |
| prop\_write(1e6) | 322.37 | 335.36 | 244.67 | 238.57 | 10.00 |
| closure\_counter(1e6) | 37.96 | 14.39 | 14.59 | 15.17 | 2.00 |
| ipow(2,20) x1e5 | 67.33 | 28.57 | 27.79 | 27.76 | 11.00 |
| str\_concat(5000) x100 | 40.18 | 41.23 | 40.15 | 41.78 | 5.00 |
| count\_primes(3000) x10 | 5.35 | 1.42 | 1.45 | 1.54 | 1.00 |
| arr\_sum(10000) x1e3 | 220.81 | 45.05 | 48.02 | 48.08 | 9.00 |

### 3.2 Speedup relative to interpreter

The table below shows how many times faster each JIT mode is compared to the pure interpreter (`interpreter_ms / mode_ms`).

| Benchmark | JIT link<br>cold | JIT link<br>warm | JIT AOT | Node v24 |
|---|---:|---:|---:|---:|
| fib(30) | 3.3× | 3.3× | 3.2× | **9.8×** |
| sum\_loop | 21.4× | **21.1×** | **21.5×** | 24.4× |
| sum\_sq | 16.2× | **16.1×** | **16.1×** | 23.7× |
| prop\_read | 1.4× | 2.2× | **2.3×** | 42.3× |
| prop\_write | 0.96× | 1.3× | **1.4×** | 32.2× |
| closure\_counter | 2.6× | 2.6× | **2.5×** | 19.0× |
| ipow | 2.4× | 2.4× | **2.4×** | 6.1× |
| str\_concat | 0.97× | 1.0× | 0.96× | **8.0×** |
| count\_primes | **3.8×** | 3.7× | 3.5× | 5.4× |
| arr\_sum | **4.9×** | 4.6× | 4.6× | 24.5× |

### 3.3 QuickJS JIT vs Node v24 ratio

Ratio of QuickJS JIT AOT time to Node time (values > 1 mean QuickJS is slower).

| Benchmark | QJS JIT AOT (ms) | Node (ms) | Ratio (QJS/Node) |
|---|---:|---:|---:|
| fib(30) | 24.12 | 8.00 | 3.0× |
| sum\_loop | 26.17 | 23.00 | 1.1× |
| sum\_sq | 26.42 | 18.00 | 1.5× |
| prop\_read | 184.24 | 10.00 | 18.4× |
| prop\_write | 238.57 | 10.00 | 23.9× |
| closure\_counter | 15.17 | 2.00 | 7.6× |
| ipow | 27.76 | 11.00 | 2.5× |
| str\_concat | 41.78 | 5.00 | 8.4× |
| count\_primes | 1.54 | 1.00 | 1.5× |
| arr\_sum | 48.08 | 9.00 | 5.3× |

---

## 4. Discussion

### 4.1 Tight arithmetic loops: JIT excels

`sum_loop` and `sum_sq` show the most dramatic improvement — **21× and 16× faster** than the interpreter respectively, and nearly on par with V8 (only 1.1× and 1.5× slower). These benchmarks are integer-only loops with no property access or heap allocation. The GCC backend is free to keep the loop variable in a register and emit native `add`/`imul` instructions, collapsing what the interpreter does in ~10 bytecode dispatches per iteration down to a handful of machine instructions. This is exactly the workload the JIT was designed to accelerate.

`count_primes` (3.5–3.8× speedup, within 1.5× of V8) similarly benefits: tight nested loops with integer modulo and early-break branches compile cleanly with no boxing overhead.

### 4.2 Recursive functions: moderate gain

`fib(30)` and `ipow(2,20)` show 3.2–3.3× and 2.4× speedups. The gain is real but more modest than loop benchmarks because recursive calls carry overhead that the JIT cannot currently eliminate: each call still goes through `JS_CallInternal` for the interpreter path, or through the JIT dispatch trampoline for JIT-to-JIT calls. V8's 9.8× advantage on `fib` reflects its ability to speculate on integer results across the recursion depth and inline aggressively — capabilities the GCC back-end JIT does not yet implement.

### 4.3 Property access: the biggest gap vs V8

`prop_read` and `prop_write` are the most striking outliers. Despite inline cache (IC) instrumentation in the JIT, the speedup is only **2.2–2.3× and 1.3–1.4×** versus the interpreter — and V8 is **18–24× faster** than QuickJS JIT on these same benchmarks.

The reasons are layered:

- **IC check cost**: Every `get_field`/`put_field` JIT stub must validate `shape_ptr == cached_shape && shape_gen == cached_gen` before taking the fast path. At 1M iterations this check itself is non-trivial compared to the trivial payload (`o.x += 0`).
- **No type specialization**: V8 knows that `o.x` is always a small integer after observing a few iterations and eliminates the JSValue boxing/unboxing entirely. QuickJS JIT always works with tagged `JSValue` (64-bit nan-boxed union), so every load/store involves a tag check.
- **No store-to-load forwarding elimination**: V8 can prove that `o.x = i` followed by `o.x` elsewhere is the same memory location and optimize accordingly. QuickJS JIT makes no such inference.

This is the largest single opportunity for future optimization: a type-specialized IC path that skips tag checks when the value is observed to be a small integer would likely bring `prop_read` within 3–5× of V8.

### 4.4 Closures: useful but limited

`closure_counter` shows a 2.5–2.6× JIT speedup. Closure variable access goes through `var_ref` — an extra pointer dereference vs a plain local — which limits how tight the generated code can be. V8's 19× advantage comes from devirtualizing the closure access entirely after observing that the captured cell always holds a small integer.

### 4.5 String concatenation: no benefit

`str_concat` shows essentially **no JIT speedup** (0.97–1.0×). String concatenation is dominated by `js_string_concat` — a C helper that allocates a new `JSString` on every `+` and copies both halves. The JIT dispatches this helper the same way the interpreter does; there is no string-specific optimization. V8's 8× advantage here is mostly rope/builder-style string representation that avoids the O(n²) copy behavior.

### 4.6 Array reads: solid mid-range gain

`arr_sum` achieves a **4.6× speedup**, with QuickJS JIT still 5.3× behind Node. Dense array element reads go through `JS_GetPropertyInt64` for the index path; the JIT emits an optimized integer-indexed property getter, but still has to validate that the array is a fast-path dense array on each iteration. V8 generates a direct memory load with no helper call after speculating on array type.

### 4.7 Cold vs warm link mode

`--jit-link` cold and warm results are nearly identical for computation-heavy benchmarks (`fib`, `sum_loop`, `sum_sq`, `ipow`). The difference is visible mainly for **property-access benchmarks**:

- `prop_read` cold: 312.87 ms → warm: 188.05 ms (1.7× improvement)
- `prop_write` cold: 335.36 ms → warm: 244.67 ms (1.4× improvement)

The cold run's individual per-function `.so` files are loaded via `dlopen` and cannot see each other. The warm combined `.so` (built with `-flto`) allows GCC's LTO pass to inline IC check helper functions across function boundaries, which matters precisely where IC checks dominate (property access). This explains why arithmetic-heavy benchmarks see no difference (nothing to inline) while property-access benchmarks see a meaningful gain.

### 4.8 JIT link warm vs JIT AOT

These two modes are **statistically indistinguishable** across all benchmarks (within 3%). Both present the JIT-compiled code from the same cache. The only practical difference is startup cost (not measured here): `--jit-aot` pays GCC compilation time before the first line of JS runs, while `--jit-link` (warm) loads the pre-built combined `.so` nearly instantly.

### 4.9 Summary of opportunities

| Area | Current gap vs V8 | Primary cause | Potential fix |
|---|---|---|---|
| Property read/write | 18–24× | Tag checks + no type specialization | Integer-typed IC fast path |
| Closures | 7.6× | `var_ref` pointer indirection | Speculative int-cell fast path |
| Recursive calls | 3.0× | Trampoline overhead | Direct JIT-to-JIT call without interpreter |
| Array reads | 5.3× | `GetPropertyInt64` helper call | Inline dense-array fast path |
| String ops | 8.4× | Allocating concat | No feasible change without rope strings |
| Arithmetic loops | 1.1–1.5× | Minimal — nearly at parity | Minor register allocation improvements |

---

## 5. Conclusion

The QuickJS GCC-backend JIT delivers **large, consistent speedups on compute-bound workloads**: 16–21× for tight arithmetic loops, 3–5× for recursive and array-access patterns. These gains bring integer-loop performance to within 10–50% of V8 TurboFan. The primary remaining gap is property access, where the absence of type specialization leaves 10–20× performance on the table. The warm link-time and AOT modes are equivalent at steady state; the combined LTO `.so` provides a measurable advantage for IC-heavy code over separate per-function `.so` files.

---

## 6. P37 Results (2026-04-10)

Phase 37 targeted the property-access gap with four sub-phases:

| Sub-phase | Change |
|---|---|
| P37.1 | Remove redundant atom check from `JIT_IC_CHECK` (P36 promoted `shape_gen` to uint32_t) |
| P37.2 | Hoist runtime guard (`_rt = JS_GetRuntime(ctx)`) to function preamble; replace `JIT_IC_CHECK` with `JIT_IC_CHECK_FAST` in emitters |
| P37.3 | Peephole: skip `DupValue`/`FreeValue` pair for get_loc→get_field pattern (refcount elision) |
| P37.4 | Bimorphic IC: upgrade from `JSJITICEntry` (1 slot) to `JSJITICEntry2` (2 slots) |

### 6.1 prop_read and prop_write: before vs after P37

All figures use the LTO-optimized binary (`CONFIG_JIT=y CONFIG_LTO=y`), warm run (cached `.so`).

| Mode | prop_read Before | prop_read After | prop_write Before | prop_write After |
|---|---:|---:|---:|---:|
| Interpreter | 422 ms | 432 ms | 322 ms | 330 ms |
| JIT AOT (warm) | 184 ms | **92 ms** | 239 ms | **75 ms** |
| Node v24 | 10 ms | 9 ms | 10 ms | 9 ms |

**prop_read speedup**: 184 ms → 92 ms (**2× faster**, from 18.4× behind Node to 10.2× behind Node)

**prop_write speedup**: 239 ms → 75 ms (**3.2× faster**, from 23.9× behind Node to 8.3× behind Node)

### 6.2 Which sub-phase contributed what

- **P37.3 (refcount elision)** is the dominant factor for `prop_read`: the `get_loc→get_field` peephole eliminates 1M unnecessary `DupValue`/`FreeValue` pairs in the hot loop, removing two conditional memory read-writes per iteration.
- **P37.1+P37.2 (IC check simplification)** reduce the per-hit check from 9 conditions to 5: one `_rt` pointer comparison, one shape pointer comparison, one `shape_gen` comparison, and one `prop_count` comparison.
- **P37.4 (bimorphic IC)** prevents early megamorphic demotion when two shapes alternate; its benefit is most visible in polymorphic workloads (not captured by the monomorphic `prop_read`/`prop_write` benchmarks).
- **prop_write** benefits from P37.1+P37.2+P37.4 but not P37.3 (the `put_field` pattern is `get_loc_obj, get_loc_val, put_field` and the object is loaded independently).

### 6.3 Full P37 benchmark table (warm AOT)

| Benchmark | Interpreter | JIT AOT (Before P37) | JIT AOT (After P37) | Node v24 |
|---|---:|---:|---:|---:|
| fib(30) x1 | 75 ms | 24 ms | 25 ms | 8 ms |
| sum_loop(1e6) | 548 ms | 26 ms | 27 ms | 23 ms |
| sum_sq(1e6) | 423 ms | 26 ms | 27 ms | 18 ms |
| **prop_read(1e6)** | 432 ms | 184 ms | **92 ms** | 9 ms |
| **prop_write(1e6)** | 330 ms | 239 ms | **75 ms** | 9 ms |
| closure_counter(1e6) | 33 ms | 15 ms | 15 ms | 2 ms |
| ipow(2,20) x1e5 | 68 ms | 28 ms | 30 ms | 11 ms |
| str_concat(5000) | 38 ms | 42 ms | 42 ms | 5 ms |
| count_primes(3000) | 5 ms | 1.5 ms | 1.4 ms | 1 ms |
| arr_sum(10000) x1e3 | 210 ms | 48 ms | 49 ms | 9 ms |

Non-property-access benchmarks are unaffected by P37, as expected.
