# Phase 8 — Performance Gap Closures

Derived from the Phase 7 post-mortem: expectations vs actual results analysis.
Current state: JIT AOT scores 0.90× vs interpreter on v8bench; sum_loop and fib
are regressions.  Phase 8 steps are ordered by impact-to-effort ratio.

---

## Todo

- [ ] **P8.1 — `JIT_T_INT` integer type for locals**
  Integer loop counters are inferred as `double _ld[]` by Phase 5.  Add a third
  inferred type `JIT_T_INT` (→ `int32_t _li[]`).  Infer it when a local is only
  assigned from integer literals, `inc_loc`, or addition of two INT-typed values.
  `int32_t` arithmetic matches the interpreter's int fast path; eliminates the
  `ADDSD` / double boxing overhead in tight integer loops.
  *Fixes: sum_loop regression (0.90× → expected ~1.4×), Crypto v8bench*

- [ ] **P8.2 — Direct self-recursive JIT calls**
  When `OP_call` / `OP_tail_call` targets the currently-compiling function
  (detectable at codegen time), emit a direct C call to `__jit_f_<hash>` instead
  of `_RT->call`.  Eliminates argv dup + 3 refcount heap ops per recursive frame.
  *Fixes: fib regression (0.79× → expected ~2×)*

- [ ] **P8.3 — Inline intra-module JIT-to-JIT calls**
  Generalize P8.2 to all callees whose `JSFunctionBytecode` is reachable in the
  current module's cpool at compile time.  For each such callee, emit an `extern`
  declaration for its `__jit_f_<hash>` symbol and call it directly, bypassing
  `_RT->call → JS_Call → JS_CallInternal`.
  *Largest single gain for v8bench: Richards, DeltaBlue, EarleyBoyer spend most
  time in short intra-module methods calling each other*

- [ ] **P8.4 — Integer argument type guards at function entry**
  Add a generated preamble check: for each argument, if `JS_VALUE_GET_TAG(argv[i])
  == JS_TAG_INT`, extract to a local `int32_t _ai[]` and use it throughout.  On tag
  mismatch (rare) fall through to the `JSValue` slow path.  Enables functions like
  `fib(n)` to treat the argument as `int32_t` without caller-side type information.
  *Prerequisite for P8.1+P8.2 to fully eliminate boxing in argument-taking functions*

- [ ] **P8.5 — Array element inline cache for `OP_get_array_el` / `OP_set_array_el`**
  Phase 6.2 caches `get_field` but not array index access.  For dense arrays
  (`class_id == JS_CLASS_ARRAY`, integer index in range), emit an inline guard and
  direct `prop[index].u.value` read/write, matching the interpreter's `fast_array`
  path.  No hash walk, no vtable call.
  *Fixes: arr_sum (0.97× → expected 3–10×); affects any array-heavy workload*

- [ ] **P8.6 — Typed float IC slots for object fields**
  Phase 6.2 IC reads property values as `JSValue`.  Add a 3-state IC
  (cold / INT / FLOAT64) that, when a slot always holds `JS_TAG_FLOAT64`, caches
  the slot type and emits a direct `double` read with no tag check.
  *Target: RayTrace (float coordinates in objects), DeltaBlue (float weights)*

- [ ] **P8.7 — In-process LLVM JIT backend (replaces fork+exec GCC)**
  Use `llvm::orc::LLJIT` to compile LLVM IR in-process in milliseconds, replacing
  the fork+exec GCC path.  The generated C structure (typed locals, IC helpers,
  vtable slow paths) maps directly to LLVM IR.  Eliminates the 2–5 s cold-compile
  latency and makes the Phase 7 cache optional rather than mandatory.
  *Large restructuring; prerequisite: all other phases stable*

---

## Expected outcomes after P8.1–P8.6

| Benchmark | Current | After P8.1–P8.6 (estimate) |
|---|---:|---|
| fib(30) | 0.79× | ~2–3× (P8.2 + P8.4) |
| sum_loop | 0.90× | ~1.3–1.5× (P8.1) |
| sum_sq | 1.95× | ~2–3× (P8.1 int path) |
| count_primes | 2.38× | ~3–4× (P8.1 int path) |
| arr_sum | 0.97× | ~3–8× (P8.5) |
| v8bench Score | 0.90× | ~1.2–1.5× (P8.3 + P8.5) |
