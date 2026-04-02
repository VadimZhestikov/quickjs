# QuickJS JIT Compiler — Overview

A two-tier native-code JIT for the QuickJS JavaScript engine, implemented incrementally
across six phases.  The JIT compiles hot JS functions to C, then lets GCC produce
optimised machine code — no custom register allocator, no IR — just generated C as the
intermediate representation.

---

## Contents

| Document | What it covers |
|---|---|
| This file | Architecture, design decisions, phase roadmap |
| [phase1-skeleton.md](phase1-skeleton.md) | Hot probe, calling convention, JIT fields in bytecode objects |
| [phase2-codegen.md](phase2-codegen.md) | Bytecode-to-C code generator, vtable, scan pass |
| [phase3-tcc.md](phase3-tcc.md) | TCC tier-1: synchronous in-process compilation (retrospective) |
| [phase4-gcc-tier2.md](phase4-gcc-tier2.md) | GCC tier-2: background thread, fork/exec, dlopen, atomic install |
| [phase5-typed-vars.md](phase5-typed-vars.md) | Forward type inference, `double _ld[]` locals, `inc_loc` fast path |
| [phase6-optimizations.md](phase6-optimizations.md) | Comparison+branch fusion, gen-time type stack, inline property cache |

---

## Motivation

QuickJS is a complete ES2020 engine in under 250 kB of C — it fits in an embedded system
or a serverless sandbox.  Its interpreter is already highly optimised: GCC -O2,
`DIRECT_DISPATCH` computed-goto dispatch, integer fast paths on every arithmetic opcode.
Beating it requires eliminating the *structural* overhead that no amount of interpreter
tuning can remove:

1. **Tag checks on every value** — every `JSValue` carries an 8-bit tag that must be
   tested before any operation.
2. **Boxing/unboxing on every arithmetic result** — `int + int` stores to `JSValue`,
   next opcode unpacks it again.
3. **Hash-chain property lookup on every field read** — `obj.x` walks a per-shape linked
   list every time, even if `obj`'s shape never changes.
4. **Comparison round-trip through bool boxing** — `a < b` produces a `JSBool` on the
   value stack only to be consumed two instructions later by `if_false`.

The JIT eliminates all four by emitting C code that GCC -O2 can reason about statically.

---

## Architecture

### Execution tiers

```
                 jit_call_count
                        │
     JS source          │  0 .. JIT_THRESHOLD_GCC (default 100)
         │              │
         ▼              ▼
    ┌──────────────────────────┐
    │   QuickJS Interpreter    │  GCC -O2, DIRECT_DISPATCH computed-goto
    │   JS_CallInternal()      │  ~200–500 MIPS on typical workloads
    └──────────┬───────────────┘
               │  count == JIT_THRESHOLD_GCC
               │  js_jit_queue_gcc() ──────────────────────────────────┐
               │                                                        │
               │  (continues interpreting while GCC works)             ▼
               │                                             ┌─────────────────────┐
               │                                             │  Background worker  │
               │                                             │  pthread            │
               │                                             │                     │
               │                                             │  gen_body() → .c    │
               │                                             │  fork+exec gcc -O2  │
               │                                             │  dlopen .so         │
               │                                             │  atomic install     │
               │                                             └─────────────────────┘
               │
               │  jit_func != NULL  (atomic read)
               ▼
    ┌──────────────────────────┐
    │   GCC tier-2 stub        │  GCC -O2, XMM regs, inlined IC, vectorised
    │   __jit_f_<addr>()       │  typically 2–3× faster than interpreter
    └──────────────────────────┘
```

### How a JIT-compiled call looks

```
JS_CallInternal(ctx, func, this, argc, argv)
        │
        ├─ read jit_func  (ACQUIRE load)
        │
        ├─ NULL?  → interpret as usual, increment jit_call_count
        │           at threshold: generate C, enqueue GCC job
        │
        └─ non-NULL?  → call jit_func(ctx, this, argc, argv, cpool, var_refs)
                        │
                        └─ returns JSValue  (new reference, caller owns it)
```

### Code generation pipeline

```
JSFunctionBytecode
        │  (bytecode[], var_count, stack_size, arg_count)
        │
        ▼
   jit_infer_types()                    ← Phase 5
        │  → local_type[var_count]
        │    JIT_T_NUMBER or JIT_T_JSVAL per local
        │
        ▼
   scan_body()                          ← Phase 2
        │  → JSJITScanResult
        │    sorted branch-target offsets
        │    early-exit on unsupported opcodes
        │
        ▼
   gen_preamble()                       ← Phase 2
        │  → emits #include, macros, function signature
        │
        ▼
   gen_body()                           ← Phase 2, extended in P5/6
        │  opcode loop with:
        │    - gen-time type stack gen_st[]  (Phase 6.1)
        │    - double _ld[] for NUMBER locals (Phase 5)
        │    - comparison fusion             (Phase 6.1)
        │    - IC for get/put_field          (Phase 6.2)
        │
        ▼
   JSJITCodeBuf (char* C source)
        │
        ▼
   GCC background worker
        │  fork + exec gcc -O2 -shared -fPIC
        │
        ▼
   .so  →  dlopen  →  dlsym("__jit_f_<addr>")
        │
        ▼
   js_jit_fb_set_func()   ← atomic RELEASE store
```

---

## Key design decisions

### C as intermediate representation

Rather than designing a custom IR and register allocator, the JIT emits C source that GCC
optimises.  This means:

- **No register allocator to write** — GCC handles spill/fill, calling convention, SIMD
- **Full GCC optimisation for free** — CSE, LICM, loop unrolling, auto-vectorisation
- **Easy incremental development** — add a new opcode by appending a `printf` to `gen_body()`
- **Cost**: GCC compilation takes 2–5 seconds per function; managed via background thread

### JSValue boxing as the main target

Every optimisation phase targets one aspect of JSValue boxing overhead:

| Phase | What boxing is eliminated |
|---|---|
| Phase 5 | Loop counters and accumulator variables: `double _ld[]` instead of `JSValue` |
| Phase 6.1 | `JSBool` boxing between comparison and branch: fuse into one C `if` |
| Phase 6.2 | `JSProperty` hash lookup on every field read: inline shape guard + slot index |

### Vtable for slow paths

All operations that cannot be inlined (complex arithmetic, calls, property modification)
go through a compile-time-constant vtable `JSJITRuntime js_jit_rt`.  The generated C
references it as `_RT->op(...)`.  This means:

- The generated `.so` has no direct dependency on QuickJS internals
- Slow-path implementations live in `quickjs.c` / `quickjs-jit.c` (not regenerated)
- GCC can devirtualise through `_RT` since it is `const` and LTO-visible

### Atomics for tier upgrade

The `jit_func` field in `JSFunctionBytecode` is read with `__ATOMIC_ACQUIRE` in
`JS_CallInternal` and written with `__ATOMIC_RELEASE` by the GCC worker thread.  No mutex
needed on the hot path — the interpreter continues running while GCC compiles.

---

## Value representation

QuickJS uses a tagged-union `JSValue` (64-bit on 64-bit platforms):

```
┌─────────────────────────────────────────┬──────────────┐
│  payload (pointer or int or double)     │  tag (int32) │
└─────────────────────────────────────────┴──────────────┘
  JS_TAG_INT       =  0   → payload is int32
  JS_TAG_BOOL      =  1   → payload is 0 or 1
  JS_TAG_NULL      =  2
  JS_TAG_UNDEFINED =  3
  JS_TAG_OBJECT    = -1   → payload is JSObject* (heap, refcounted)
  JS_TAG_STRING    = -2   → payload is JSString*
  JS_TAG_FLOAT64   = -5   → payload is double (separate boxed heap alloc on 32-bit)
```

The JIT's job is to prove — statically or with a runtime guard — that a value has a
specific tag, then operate on the payload directly.

---

## Object and shape system (relevant to IC)

```
JSObject
  ├── shape: JSShape*          ← pointer-unique per property layout
  └── prop:  JSProperty[]      ← parallel array of property values

JSShape
  ├── prop_hash_mask            ← hash table size - 1
  ├── prop[]:  JSShapeProperty[]← atom + flags + hash_next per property
  └── [hash table before struct]← uint32 array, indexed by atom & mask

Property lookup (atom → value):
  h = atom & shape->prop_hash_mask
  h = prop_hash_end(shape)[-h-1]   ← hash chain head (1-indexed)
  while (h): if shape->prop[h-1].atom == atom → found at slot h-1
             else h = shape->prop[h-1].hash_next
```

IC fast path: check `obj->shape == cached_shape` (one pointer compare), then
read `obj->prop[cached_slot].u.value` directly — no hash chain walk.

---

## Performance summary

All measurements: Linux 6.6.87.2 WSL2 x86-64, GCC -O2, 3 runs minimum.
Speedup = interpreter_min / JIT_min.  Values > 1 mean JIT is faster.

```
Benchmark           Interp     JIT P6.2   Speedup   Bottleneck removed
─────────────────────────────────────────────────────────────────────
fib(30) ×1          106 ms      95 ms      1.12×    (vtable recursion limits gain)
sum_loop(1e6) ×20   833 ms     795 ms      1.05×    (no typed vars, no IC)
sum_sq(1e6) ×20     706 ms     270 ms      2.62×    Phase 5 double locals + IC
count_primes ×10    7.4 ms     2.9 ms      2.55×    Phase 5 inc_loc fast path
arr_sum ×1000       361 ms     379 ms      0.95×    get_array_el still vtable
```

V8 benchmark suite (higher = better, WSL2 noise ±15%):

```
Richards    656 → 866   (+32%)   property-access heavy, IC fires
DeltaBlue   617 → 618   (≈ same) similarly property-heavy, noise masks gain
Crypto      938 → 949   (≈ same) mostly float arithmetic
RayTrace   1049 → 933   (−11%)  GCC compile-time competing with 1-s window
```

---

## Build flags

```sh
make CONFIG_JIT=y qjs                      # JIT with default threshold (100)
make CONFIG_JIT=y JIT_THRESHOLD_GCC=2 qjs  # threshold=2 (benchmark mode)
```

`JIT_THRESHOLD_GCC` controls how many calls before GCC compilation is queued.
Lower = JIT fires sooner (useful for benchmarks); higher = only truly hot functions
are compiled (good for startup-sensitive workloads).

---

## Source files

| File | Role |
|---|---|
| `quickjs-jit.h` | Public JIT API: vtable struct, IC entry, accessor declarations |
| `quickjs-jit.c` | Code generator, GCC worker thread, vtable implementations |
| `quickjs.c` | Hot probe in `JS_CallInternal`; bytecode accessor functions; IC helpers |
| `jit_perf_tests/bench_gcc.js` | Micro-benchmark suite (8-second warm-up) |
| `jit_perf_tests/v8bench/` | V8 benchmark suite port |
| `jit_perf_tests/RESULTS.md` | Raw measurements for all phases |
