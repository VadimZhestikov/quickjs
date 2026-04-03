# QuickJS JIT Compiler — Overview

A two-tier native-code JIT for the QuickJS JavaScript engine, implemented incrementally
across eight phases (P8 in progress).  The JIT compiles hot JS functions to C, then lets
GCC produce optimised machine code — no custom register allocator, no IR — just generated
C as the intermediate representation.

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
| [phase7-cache.md](phase7-cache.md) | Persistent .so cache, `--jit-aot`, `--jit-warmup`, e.stack fix |
| [phase8-todo.md](phase8-todo.md) | Phase 8 improvement backlog: P8.1–P8.7 |
| [phase8-p81-int-locals.md](phase8-p81-int-locals.md) | P8.1: `JIT_T_INT` integer locals, `int64_t _li[]`, inc/add/dec_loc fast paths |
| [phase8-p82-self-recursive.md](phase8-p82-self-recursive.md) | P8.2: direct self-recursive C calls, `JIT_T_SELF_FUNC` gen_st marker, `unlikely` bug fix |
| [phase8-p83-jit-to-jit.md](phase8-p83-jit-to-jit.md) | P8.3: `js_jit_call` vtable entry bypasses `JS_CallInternal` for JIT-compiled callees |
| [phase8-ic-fixes.md](phase8-ic-fixes.md) | IC correctness: `likely`→`js_likely` fix, atom ABA guard, megamorphic demotion |
| [phase8-p84-int-args.md](phase8-p84-int-args.md) | P8.4: `_ai[]`/`_aim` integer argument fast-path, register-resident args |
| [phase8-p85-array-fast.md](phase8-p85-array-fast.md) | P8.5: dense array element fast path, bypass `JS_ValueToAtom` + hash walk |
| [phase8-p86-float64-arith.md](phase8-p86-float64-arith.md) | P8.6: `JSJITICEntry.kind`, float64 arithmetic/comparison fast paths |
| [phase9-todo.md](phase9-todo.md) | Phase 9 plan: stackless IR, CF structuring, variable names, typed temporaries |
| [phase10-todo.md](phase10-todo.md) | Phase 10 plan: combined .so, LTO inter-procedural inlining, direct C calls |

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
               │  js_jit_queue_gcc()
               │     │
               │     ├─ jit_cache_get(hash) ──hit──► dlopen(.so) → atomic install
               │     │                                (no GCC, ~1 ms)
               │     │
               │     └─ miss ──────────────────────────────────────────┐
               │  (continues interpreting while GCC works)             ▼
               │                                             ┌─────────────────────┐
               │                                             │  Background worker  │
               │                                             │  pthread            │
               │                                             │                     │
               │                                             │  gen_body() → .c    │
               │                                             │  fork+exec gcc -O2  │
               │                                             │  jit_cache_put(.so) │
               │                                             │  dlopen .so         │
               │                                             │  atomic install     │
               │                                             └─────────────────────┘
               │
               │  jit_func != NULL  (atomic read)
               ▼
    ┌──────────────────────────┐
    │   GCC tier-2 stub        │  GCC -O2, XMM regs, inlined IC, vectorised
    │   __jit_f_<hash>()       │  typically 2–3× faster than interpreter
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
   gen_body()                           ← Phase 2, extended in P5/6/8/9
        │  opcode loop with:
        │    - gen-time type stack gen_st[]      (Phase 6.1)
        │    - double _ld[] for NUMBER locals     (Phase 5)
        │    - int64_t _li[] for INT locals       (P8.1)
        │    - comparison fusion                  (Phase 6.1)
        │    - IC for get/put_field               (Phase 6.2, fixed in IC-fixes)
        │    - int32_t _ai[]/_aim arg fast path   (P8.4)
        │    - array element fast path            (P8.5)
        │    - float64 arith/cmp fast paths       (P8.6)
        │    - [P9.1] JS variable names in locals
        │    - [P9.2] stackless: _ts{N} instead of _s[]
        │    - [P9.3] structured CF: if/while/for instead of goto
        │    - [P9.4] typed stack temporaries: double _tsd{N}
        │
        ▼
   JSJITCodeBuf (char* C source)
        │
        ▼
   GCC background worker
        │  fork + exec gcc -O2 -shared -fPIC
        │
        ▼
   jit_cache_put()                      ← Phase 7.3: copy to ~/.cache/qjs-jit/<hash>.so
        │                                  [P10.1] also writes <hash>.c
        ▼
   .so  →  dlopen  →  dlsym("__jit_f_<hash>")   ← hash-stable symbol (Phase 7.3)
        │
        │  [P10.2] --jit-link: gcc -O2 -flto <all .c> -o combined.so
        │  [P10.3] emits direct extern __jit_f_<hash>() calls in gen_body()
        │  [P10.4] manifest: bc_hash → func_ptr; atomic install from combined.so
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
| Phase 7 | GCC compilation overhead on startup: pre-built `.so` loaded from cache |
| P8.1 | Integer loop counters: `int64_t _li[]`, branch-free `inc/dec/add_loc` |
| P8.2 | Self-recursive call overhead: direct C call instead of `_RT->call` |
| P8.3 | Inter-function call overhead: `js_jit_call` checks `jit_func` before `JS_Call` |
| IC fixes | False IC hits (ABA, `likely` bug): atom guard + megamorphic demotion |
| P8.4 | Argument tag checks on every read: `int32_t _ai[]` extracted at entry, register-resident |
| P8.5 | Array index: `JS_ValueToAtom` + hash walk → `class_id` + bounds check + direct slot read |
| P8.6 | Float64 arithmetic/comparison: `(INT\|F64)×(INT\|F64)` fast path avoids vtable for object-property float math |
| **P9.1** | **Variable names: JS identifier names in generated C locals (debuggability)** |
| **P9.2** | **Stackless IR: `_s[]` array → named `_ts{N}` locals; GCC keeps temporaries in registers** |
| **P9.3** | **CF structuring: goto spaghetti → `if/while/for`; enables GCC loop optimisations** |
| **P9.4** | **Typed stack temporaries: `double _tsd{N}` for provably float64 stack slots; zero tag checks** |
| **P10.3** | **Direct C calls: `extern __jit_f_<hash>()` bypasses `js_jit_call()` indirect + atomic read** |
| **P10.5** | **IC check inlining: LTO inlines `js_jit_ic_check()` (3 pointer compares) into hot loop** |

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

All measurements: Linux 6.6.87.2 WSL2 x86-64, GCC -O2.
P8.1 results use `--jit-aot` with warm cache (bench_aot.js, 3 runs, min shown).
Speedup = interpreter_min / JIT_min.  Values > 1 mean JIT is faster.

All measurements: Linux 6.6.87.2 WSL2 x86-64, GCC -O2, `--jit-aot` warm cache,
`qjs_interp` = JIT-disabled binary.  Min of 3 runs (WSL2 timing is noisy; outliers
discarded).

```
Benchmark               Interp    JIT P8.5   Speedup   Primary driver
──────────────────────────────────────────────────────────────────────────────
fib(38) ×1             4401 ms    1343 ms     3.3×     P8.2 self-call + P8.4 int arg
sum_loop(1e6) ×20       465 ms     166 ms     2.8×     P8.1 int locals
arr_sum(10k) ×1000      213 ms     188 ms     1.1×     P8.5 array fast path
count_primes(1e4) ×10    24 ms      13 ms     1.8×     P8.1 + P8.3 JIT-to-JIT
```

Previous checkpoint (post-P8.3, pre-IC-fixes):

```
Benchmark               Interp    JIT P8.3   Speedup   Notes
──────────────────────────────────────────────────────────────────────────────
fib(30) ×1              112 ms      28 ms     4.0×     P8.2 direct self-call
sum_loop(1e6) ×20       796 ms     940 ms     0.85×    regression (let vars, no add_loc)
sum_sq(1e6) ×20         616 ms     237 ms     2.60×    P8.1 + P8.3 JIT-to-JIT callee
count_primes ×10       7.59 ms    2.48 ms     3.06×    P8.1 + P8.3 JIT-to-JIT callee
arr_sum ×1000           345 ms     350 ms     0.99×    get_array_el vtable — no IC yet
```

---

## Phase 9 — Planned architecture evolution

Phase 9 addresses the two remaining structural inefficiencies in the generated C that no
amount of opcode-level tuning can fix: the explicit value stack and the goto-based control
flow.

### 9.1 Variable names (debuggability)

`JSFunctionBytecode::vardefs[i].var_name` holds the original JS identifier as a `JSAtom`.
Before the codegen loop, build a name table and use it in all local/argument declarations:

```c
// Before P9.1          After P9.1
double _ld[3];     →    double _jsd_x_0, _jsd_y_1, _jsd_z_2;
int64_t _li[1];    →    int64_t _jsi_count_0;
```

Names are always present in non-stripped builds; non-ASCII falls back to `_jsv_{idx}`.
Zero runtime cost; C keyword collisions avoided by the type prefix.

### 9.2 Stackless value stack

The explicit `JSValue _s[N]` array forces GCC to treat every push/pop as a memory
operation through an aliased pointer.  The key insight: **the value stack is provably
empty at every basic-block boundary** for all JIT-eligible functions (for-in/for-of,
which keep iterator state on the stack across loop headers, are already excluded).

Therefore no spilling is needed at block boundaries.  The transform is purely local:

```
Before                       After
──────────────────────────   ──────────────────────────────────────
JSValue _s[8]; int _sp=0;    JSValue _tsv0,_tsv1,_tsv2; // max depth=3
_s[_sp++] = ic_read(...);    _tsv0 = ic_read(...);
_s[_sp++] = ic_read(...);    _tsv1 = ic_read(...);
JSValue _b=_s[--_sp],        double _da = F64(_tsv0);
         _a=_s[--_sp];       double _db = F64(_tsv1);
double _da=F64(_a),_db=...;  _tsv0 = JS_NewFloat64(ctx,_da+_db);
_s[_sp++]=JS_NewFloat64(..);
```

GCC now sees pure locals — no pointer aliasing, all temporaries live in registers.
A pre-pass computes `stack_depth_tab[pc]` so each opcode knows the exact slot names
to use without changing the linear gen_body structure.

### 9.3 Control flow structuring

`gen_body` currently emits a flat sequence of C blocks connected by `goto _L{pc}`.
GCC can sometimes recover loop structure from gotos, but it cannot reliably apply
vectorisation, LICM, or unrolling unless it sees canonical `while`/`for` forms.

**Algorithm:**

1. Extend `JSJITScanResult` to a full CFG (basic blocks + successor/predecessor edges)
2. Compute dominator tree (iterative Cooper-Harvey-Kennedy, ~35 lines)
3. Identify back-edges (loop headers) and immediate post-dominators (if/else joins)
4. Replace the linear opcode loop with a recursive structured emitter:

```
emit_region(b, end):
  if b is loop header  →  emit "while(1){" + emit_region(body) + "}"
  if b is if/else      →  emit condition + "if(){" + then + "}else{" + else + "}"
  if b is switch       →  emit "switch(val){ case N: ... }"
  else                 →  emit block contents + advance to successor
```

Fallback to `goto` for exception paths, labelled break/continue, and any pattern the
structurer does not recognise.  All reducible CFGs (the only kind QuickJS generates
from structured JavaScript) are handled without fallback.

### 9.4 Typed stack temporaries

Once the stack is stackless (P9.2), individual slots can carry type information from
gen_st.  A slot known to be `JIT_T_NUMBER` at push time uses `double _tsd{N}` instead
of `JSValue _tsv{N}`.  Combined with IC `kind=1` (P8.6), object float properties are
read as raw doubles and never re-boxed until a function boundary:

```c
// After P9.4 for this.x * this.y + this.z (all float64 IC slots)
double _tsd0 = p_this->prop[ic0.slot].u.float64;  // no JSValue, no tag check
double _tsd1 = p_this->prop[ic1.slot].u.float64;
double _tsd2 = _tsd0 * _tsd1;
double _tsd3 = p_this->prop[ic2.slot].u.float64;
return JS_NewFloat64(ctx, _tsd2 + _tsd3);
```

### Expected performance impact

| Benchmark | Current JIT | After Phase 9 | Estimated gain |
|---|---:|---|---|
| RayTrace | 963 | 3000–5000 | **3–5×** (float-heavy, typed temps dominant) |
| DeltaBlue | 905 | 1600–2300 | **1.8–2.5×** (float strengths + LICM) |
| Crypto | 1253 | 2200–3800 | **1.8–3×** (int locals, loop vectorisation) |
| Richards | 805 | 1100–1450 | **1.4–1.8×** (object loops, no vtable per access) |
| Splay | 1208 | 1700–2050 | **1.4–1.7×** |
| EarleyBoyer | ~1400 | 1800–2400 | **1.3–1.7×** |
| RegExp | 393 | 430–510 | **1.1–1.3×** (regex engine not JIT-compiled) |
| **V8bench score** | **~940** | **~1800–3000** | **~2–3×** |

Theoretical ceiling for the GCC-JIT approach (function-call overhead, IC checks, and
JS→C ABI remain): approximately 5000–7000 overall, or ~25–35% of V8.  Closing the
remaining gap requires emitting native machine code directly.

---

## Phase 10 — Planned: Combined .so + LTO Inter-procedural Optimisation

Phase 9 makes each function's generated C as clean as possible for GCC within a single
function.  Phase 10 removes the remaining barrier: today each function is compiled to its
own `.so`, so GCC cannot inline `dot()` into its callers or propagate constants across
function boundaries.

### 10.1 Cache `.c` source

`jit_cache_put()` writes `<hash>.so` today; P10.1 also writes `<hash>.c`.  Zero change to
the hot path.

### 10.2 `--jit-link` combiner

A new execution mode that collects all `.c` files for a script and invokes GCC once:

```sh
gcc -O2 -flto -shared -fPIC \
    ~/.cache/qjs-jit/aabbcc.c \
    ~/.cache/qjs-jit/ddeeff.c \
    ...
    -o ~/.cache/qjs-jit/combined_<run_hash>.so
```

GCC sees all JIT-compiled functions simultaneously: inlining, IPO, constant propagation,
and vectorisation across call boundaries become possible.

### 10.3 Direct C calls

When the callee's `bc_hash` is known at code-generation time, emit a direct `extern`
declaration and call instead of `js_jit_call()`:

```c
// Before P10.3 — indirect through runtime pointer
_r = js_jit_call(ctx, _callee_obj, argc, argv);

// After P10.3 — direct call, visible to GCC inliner
extern JSValue __jit_f_aabbcc0011223344(
    JSContext*, JSValue, int, JSValue*, JSValue*, JSValue**);
_r = __jit_f_aabbcc0011223344(ctx, _this, argc, argv, _cpool, _var_refs);
```

When caller and callee land in the same combined `.so`, GCC inlines small callees
(dot, add, normalize, etc.) automatically.

### 10.4 Manifest and combined loader

The combined `.so` exports a `__jit_manifest[]` table mapping `bc_hash → func_ptr`.
`js_jit_install_combined()` walks the table and atomically installs each function pointer
into the corresponding `JSFunctionBytecode`, replacing the per-function `.so` handles.

### 10.5 IC check inlining via LTO

`js_jit_ic_check()` (shape + atom guard, ~3 pointer compares) is currently in
`quickjs-jit.c`, compiled separately — GCC cannot inline it into the generated hot loop.
The fix: extract the IC helpers into `quickjs-jit-ic.c` and add it to the `--jit-link`
GCC command line alongside the generated `.c` files:

```sh
gcc -O2 -flto -shared -fPIC \
    ~/.cache/qjs-jit/*.c quickjs-jit-ic.c \
    -o combined.so
```

GCC inlines the 3-compare guard directly into every `get_field` hot path.  This removes
the last function-call overhead from the most frequent operation in object-heavy
benchmarks (DeltaBlue, RayTrace).  Verification: `objdump -d combined.so | grep -c
"call.*js_jit_ic_check"` must return 0.

### Synergy with Phase 9

Phase 9 typed temps (`_tsd{N}`) mean the inlined callee code contains only `double`
arithmetic — no JSValue boxing at the inlined boundary.  The combination of P9.4 +
P10.3 is what enables GCC to produce tight XMM loops for float-heavy benchmarks
(RayTrace, DeltaBlue).

### Expected performance (P9 → P10 additional gain)

| Benchmark | After P9 | After P10 | P10 gain |
|---|---:|---:|---|
| RayTrace | 3000–5000 | 8000–15000 | **2–3×** |
| DeltaBlue | 1600–2300 | 2200–4600 | **1.4–2×** |
| Crypto | 2200–3800 | 3000–5000 | **1.3–1.5×** |
| Richards | 1100–1450 | 1400–1800 | **1.2–1.4×** |
| **V8bench score** | **~1800–3000** | **~3000–5500** | **~1.7–2×** |

---

## Build flags

```sh
make CONFIG_JIT=y qjs                      # JIT with default threshold (100)
make CONFIG_JIT=y JIT_THRESHOLD_GCC=2 qjs  # threshold=2 (benchmark mode)
```

`JIT_THRESHOLD_GCC` controls how many calls before GCC compilation is queued.
Lower = JIT fires sooner (useful for benchmarks); higher = only truly hot functions
are compiled (good for startup-sensitive workloads).

## CLI flags

```sh
./qjs --jit-warmup script.js   # compile all functions → cache, then exit   (Phase 7)
./qjs --jit-aot    script.js   # compile all functions → cache (or hit), then execute (Phase 7)
./qjs              script.js   # normal: JIT triggers at threshold during execution

# Phase 10 workflow (planned):
./qjs --jit-warmup script.js   # Step 1: warm all functions, write .so + .c to cache
./qjs --jit-link   script.js   # Step 2: combine .c files with GCC -O2 -flto
./qjs --jit-aot    script.js   # Step 3: execute with combined .so (inlined callees)
```

Cache location: `$QJS_JIT_CACHE` or `~/.cache/qjs-jit/<hash16hex>.so`.

---

## Source files

| File | Role |
|---|---|
| `quickjs-jit.h` | Public JIT API: vtable struct, IC entry, accessor declarations |
| `quickjs-jit.c` | Code generator, GCC worker thread, vtable implementations |
| `quickjs.c` | Hot probe in `JS_CallInternal`; bytecode accessor functions; IC helpers |
| `jit_perf_tests/bench_gcc.js` | Micro-benchmark suite (8-second warm-up, background GCC) |
| `jit_perf_tests/bench_aot.js` | Micro-benchmark suite for `--jit-aot` mode (no warm-up needed) |
| `jit_perf_tests/v8bench/` | V8 benchmark suite port |
| `jit_perf_tests/RESULTS.md` | Raw measurements for all phases |
