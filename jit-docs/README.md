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
| [phase11-todo.md](phase11-todo.md) | Phase 11: performance gap analysis, P11.1–P11.4 implemented, P11.5–P11.10 planned |
| [phase12-todo.md](phase12-todo.md) | Phase 12: generator/async JIT (yield/await CPS transform) — P12.1–P12.4 implemented |
| [phase13-todo.md](phase13-todo.md) | Phase 13–17: closure creation, try/catch, iterators, delete, spread/apply |
| [phase18-todo.md](phase18-todo.md) | Phase 18: type-test opcodes (is_null/undefined/typeof_is_*), stack-shuffle extras (dup3, nip1, insert3/4, perm3/4/5, rot4l/5l, swap2) |
| [phase18-todo.md](phase18-todo.md) | Phase 19: utility ops (close_loc, get_var_undef, throw_error, to_object/propkey, regexp, set_name_computed, set_proto/home_object, get_array_el2/3, define_array_el, push_bigint_i32) |
| [phase18-todo.md](phase18-todo.md) | Phase 20: ref-slot ops (make_loc_ref, make_arg_ref, make_var_ref, make_var_ref_ref, get_ref_value, put_ref_value) |
| [phase18-todo.md](phase18-todo.md) | Phase 21: spread/rest/copy (append, copy_data_properties, rest) — includes gen_st type invalidation fix for append |
| [phase18-todo.md](phase18-todo.md) | Phase 22: private fields (private_symbol, get/put/define_private_field, private_in) |
| [phase18-todo.md](phase18-todo.md) | Phase 23: class/OOP (check_ctor_return, check/add_brand, get_super, get/put_super_value, define_method/computed); check_ctor/init_ctor/define_class deferred (need new_target/sf) |
| [phase18-todo.md](phase18-todo.md) | Phase 25: for-in / iterator protocol (for_in_start, for_in_next, iterator_next, iterator_call) — was already implemented in P15; regression tests added |
| [phase18-todo.md](phase18-todo.md) | Phase 26: constructors/classes (check_ctor, init_ctor, define_class, define_class_computed) via ctx->rt->current_stack_frame; no signature change needed |
| [phase18-todo.md](phase18-todo.md) | Phase 27: dynamic import (import_op wrapping js_dynamic_import) |
| [phase18-todo.md](phase18-todo.md) | Phase 28: OP_eval added to scan_is_unsupported — functions with direct eval run interpreted |

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
| **P11.1** | **Inline `js_jit_ic_read`: 1966 call sites eliminated in `combined.so`** |
| **P11.2** | **Remove JSValue slot zero-initialization: Splay −24% regression resolved** |
| **P11.3** | **Monomorphic call IC: per-call-site `JSJITCallICEntry` bypasses vtable for JIT callees** |
| **P11.4** | **Inline array element fast path: `class_id` guard + direct slot read, no `JS_ValueToAtom`** |

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

All measurements: Linux 6.6.87.2 WSL2 x86-64, GCC -O2, `--jit-aot` warm cache + `combined.so`.
5 runs, medians reported.  WSL2 scheduling noise is high; individual runs vary ±20%.

### V8bench (version 6) — current and projected (2026-04-04)

Node v24.2.0 reference: **37 551**.  All JIT figures are `--jit-aot` AOT medians.

| Benchmark | Interpreter | **P11.3+P11.4** | P11.5–P11.8 est. | P11.10 est. | Node v24 |
|---|---:|---:|---:|---:|---:|
| Richards    |  ~900 | **1105** | 1350–1600  | 2500–4000  | 31 461 |
| DeltaBlue   | ~1000 | **1063** | 1350–1550  | 2500–4000  | 74 912 |
| Crypto      | ~1700 | **1759** | 2600–3800  | 5000–9000  | 41 627 |
| RayTrace    | ~1000 | **1045** | 1150–1300  | 2500–5000  | 67 783 |
| EarleyBoyer | ~1400 | **1508** | 1900–2400  | 3000–5500  | 56 761 |
| RegExp      |  ~400 |  **360** |  380–440   |  400–500   |  9 001 |
| Splay¹      | ~1804 | **2507** | 2900–3400  | 4500–7000  | 30 991 |
| **Score**   | ~1000 | **1063** | **1400–1650** | **3000–5000** | **37 551** |
| % of Node   |  2.7% |  **2.8%** | 3.7–4.4%  | 8–13%      | 100%   |

¹ Splay: isolated single-benchmark measurement (full-suite median 1688 due to WSL2
  scheduling noise; isolated runs are more reliable for this benchmark).

Note: interpreter scores are noisy in full-suite runs (first benchmark suffers WSL2
startup jitter).  Gains vs P11.1+P11.2 baseline (950): **+12%** overall,
DeltaBlue +49%, Crypto +42%, Splay +38% (full-suite medians).

### Micro-benchmarks (P8.5 era, for reference)

```
Benchmark               Interp    JIT P8.5   Speedup   Primary driver
──────────────────────────────────────────────────────────────────────────────
fib(38) ×1             4401 ms    1343 ms     3.3×     P8.2 self-call + P8.4 int arg
sum_loop(1e6) ×20       465 ms     166 ms     2.8×     P8.1 int locals
arr_sum(10k) ×1000      213 ms     188 ms     1.1×     P8.5 array fast path
count_primes(1e4) ×10    24 ms      13 ms     1.8×     P8.1 + P8.3 JIT-to-JIT
```

---

## Phase 9 — Planned architecture evolution

Phase 9 addresses the two remaining structural inefficiencies in the generated C that no
amount of opcode-level tuning can fix: the explicit value stack and the goto-based control
flow.

### 9.0 Compiler-side annotations (prerequisite)

Rather than re-deriving structure from bytecode in the JIT (stack simulation, dominator
analysis), Phase 9 taps information the `quickjs.c` compiler already has at compile time.

**Stack depths** — `compute_stack_size()` already simulates every opcode; it just doesn't
save the per-PC table.  One new field (`int8_t *stack_depth_tab`) in `JSFunctionBytecode`
and ~10 lines make it available.  The JIT's ~60-line pre-pass simulation disappears.

**Control flow structure** — at each label patch site the compiler knows
`(branch_pc, target_pc, kind)` for every `if`, `for`, `while`, and `switch` at the
moment the jump is resolved.  A compact `JSJITCFAnnotation[]` side-table records this:

```c
typedef enum { JIT_CF_LOOP, JIT_CF_IF, JIT_CF_ELSE, JIT_CF_SWITCH } JSJITCFKind;
typedef struct { uint32_t branch_pc, target_pc; uint8_t kind; } JSJITCFAnnotation;
```

Cost in `quickjs.c`: ~60 lines total.  Benefit: ~350 lines of CFG/dominator analysis
removed from the JIT; P9.3 drops from ~5 days to ~2 days.

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
`stack_depth_tab[pc]` (filled by `compute_stack_size()` in P9.0) tells each opcode
the exact slot names to use — no separate JIT pre-pass needed.

### 9.3 Control flow structuring

`gen_body` currently emits a flat sequence of C blocks connected by `goto _L{pc}`.
GCC can sometimes recover loop structure from gotos, but it cannot reliably apply
vectorisation, LICM, or unrolling unless it sees canonical `while`/`for` forms.

With the P9.0 `cf_annotations` table, no CFG or dominator analysis is needed.
The JIT loads the annotation table and uses it directly in a recursive emitter:

```
emit_region(pc, end_pc):
  ann = cf_by_branch[pc]
  if ann.kind == LOOP    →  emit "while(1){" + emit_region(body) + "}"
  if ann.kind == IF/ELSE →  emit condition + "if(){" + then + "}else{" + else + "}"
  if ann.kind == SWITCH  →  emit "switch(val){ case N: ... }"
  else                   →  emit block contents + advance to next PC
```

Fallback to `goto _L{pc}` for any branch PC not in the annotation table (exception
paths, complex patterns).  If `cf_annotations` is NULL (stripped build), the old
linear goto emitter is used unchanged.

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

Phase 9 figures use the pre-P11 baseline (~940).  Current baseline is P11.3+P11.4 (~1063).

| Benchmark | Pre-P11 JIT | After Phase 9 | Estimated gain |
|---|---:|---|---|
| RayTrace | 963 | 3000–5000 | **3–5×** (float-heavy, typed temps dominant) |
| DeltaBlue | 905 | 1600–2300 | **1.8–2.5×** (float strengths + LICM) |
| Crypto | 1253 | 2200–3800 | **1.8–3×** (int locals, loop vectorisation) |
| Richards | 805 | 1100–1450 | **1.4–1.8×** (object loops, no vtable per access) |
| Splay | 1208 | 1700–2050 | **1.4–1.7×** |
| EarleyBoyer | ~1400 | 1800–2400 | **1.3–1.7×** |
| RegExp | 393 | 430–510 | **1.1–1.3×** (regex engine not JIT-compiled) |
| **V8bench score** | **~940** | **~1800–3000** | **~2–3×** |

Theoretical ceiling for the GCC-JIT approach is approximately **4500–8000** overall
(~12–21% of Node v24 = 37 551).  The four irreducible overheads that bound this ceiling:

1. **C function call ABI** — every JS function boundary pays ~15–30 cycles for register
   save/restore regardless of callee size.  Recursive and dynamically-dispatched calls
   cannot be inlined even with LTO.  DeltaBlue (pure method calls) hits this hardest:
   ceiling ~5–11% of Node.
2. **JSValue boxing at non-inlined boundaries** — even with INT32/FLOAT64 typed locals,
   return values must be boxed at every call site GCC doesn't inline.
3. **Reference counting** — `DupValue`/`FreeValue` on every heap JSValue access.  The JIT
   and interpreter pay the same cost here; only integer values (no refcount) give the JIT
   an asymmetric advantage (INT32 locals from P11.6).
4. **GCC inliner budget** — `combined.so` LTO enables cross-function inlining but GCC's
   size heuristics cap the depth.  V8's Turbofan uses profile-guided inlining with
   arbitrary depth.

Per-benchmark ceiling (all P11 + P9 + P10 implemented, C-as-IR only):

| Benchmark | Current | Ceiling | Node v24 | % of Node |
|---|---:|---:|---:|---:|
| Richards    | 1105 | 4000–6000  | 31 461 | 13–19% |
| DeltaBlue   | 1063 | 4000–8000  | 74 912 |  5–11% |
| Crypto      | 1759 | 8000–15000 | 41 627 | 19–36% |
| RayTrace    | 1045 | 6000–12000 | 67 783 |  9–18% |
| EarleyBoyer | 1508 | 5000–10000 | 56 761 |  9–18% |
| RegExp      |  360 |  500–1000  |  9 001 |  6–11% |
| Splay       | 2507 | 4000–8000  | 30 991 | 13–26% |
| **Score**   | **1063** | **4500–8000** | **37 551** | **12–21%** |

Closing the gap beyond 21% requires native machine code emission (custom x86-64
assembler or LLVM backend), tracing-style inlining across arbitrary call depth, or
eliminating reference counting via generational GC with escape analysis.

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
./qjs              script.js   # normal: JIT triggers at threshold during execution
./qjs --jit-warmup script.js   # compile all functions → cache, then exit   (Phase 7)
./qjs --jit-aot    script.js   # compile all functions → cache (or hit), then execute (Phase 7)

# Recommended AOT workflow (Phase 10, fully implemented):
./qjs --jit-warmup        script.js   # Step 1: warm all functions, write .so + .c to cache
./qjs --jit-warmup --jit-link script.js  # Step 2: combine .c files → combined.so (LTO)
./qjs --jit-aot           script.js   # Step 3: execute with combined.so (fast load, inlined)
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
