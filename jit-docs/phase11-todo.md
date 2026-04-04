# Phase 11 — Performance Gap Analysis and Improvement Plan

## Why the gain is only +16% instead of the expected 2–5×

Measured 2026-04-04.  Interpreter median: **975**.  `--jit-aot` median: **1136** (+16%).

Before designing fixes, the actual bottlenecks were measured by reading all 527 generated
`.c` files and disassembling `combined.so`.  Key findings:

| Metric | Count | Root cause |
|---|---:|---|
| `js_jit_ic_read` calls remaining in `combined.so` after LTO+O3 | **1966** | GCC inliner budget exhausted — still a function call on IC hit |
| `_RT->call` (method/function vtable calls) | **895** | No call-site cache; every method dispatch goes through `JS_Call` |
| `_RT->get_array_el` (no IC) | **161** | Array element reads have no fast path |
| `_CHK` exception checks | **6090** | Bloat; many after provably non-throwing ops |
| `_DUP`+`_FREE` (refcount ops) | **18550** | Conservative; many redundant |
| Typed double (`_tsd`) usage fraction | **8.2%** | Type inference rarely reaches hot code |
| Splay benchmark | **−24%** regression | JIT is *slower* than interpreter on recursive call-heavy workloads |

### Architectural summary

The current JIT converts JS bytecode to C code that still uses `JSValue` (16-byte tagged
union) for nearly all operations, calls into the same runtime vtable as the interpreter,
and adds per-call frame initialization overhead.  The interpreter's `JS_CallInternal`
compiles as a single monolithic function under GCC -O2: GCC keeps `sp` and object pointers
in registers across all opcodes, CSEs repeated property reads, and sees no function-call
boundaries.  The JIT breaks this into 528 separate C functions — each one competes for
register allocation in isolation, and every cross-function edge pays ABI cost.

The JIT wins only where:
1. **IC fast path avoids `JS_GetPropertyInternal`** — prototype chain walk eliminated.
2. **Typed double inference** eliminates boxing for a local (only 8.2% of the time).

It loses where per-call overhead (slot initialization, refcount bookkeeping, ABI setup)
exceeds the savings from eliminated opcode dispatch.

---

## Dependencies

```
P11.1 (inline ic_read)         ─── independent, do first (one-liner)
P11.2 (slot init overhead)     ─── independent, do first (codegen change)
P11.3 (method call IC)         ─── independent; benefits compound with P11.1
P11.4 (array element IC)       ─── independent
P11.5 (elide exception checks) ─── depends on P11.6 (safe flag needs type info)
P11.6 (INT32 type inference)   ─── independent; P11.8 feeds it
P11.7 (persist IC state)       ─── independent
P11.8 (get_length → INT32)     ─── feeds P11.6
P11.9 (OSR)                    ─── independent; large
P11.10 (speculative types)     ─── requires P11.6 foundation
```

Suggested order: **P11.1 → P11.2** (immediate fixes, fix regression) →
**P11.3 → P11.4** (second tier, high return) →
**P11.5 → P11.6 → P11.8** (type system work) →
**P11.7** (infrastructure) →
**P11.9 → P11.10** (long-term).

---

## P11.1 — Inline `js_jit_ic_read` at the call site
**Effort:** ~0.5 day  **Risk:** trivial  **Files:** `quickjs-jit.c`

### Problem

Despite `-O3 -flto`, GCC does not inline `js_jit_ic_read` into `combined.so` —
**1966 call sites remain** as real function calls.  GCC's inliner budget exhausted: the
resulting combined function would exceed the code-size threshold.

So on every IC hit today:
```
JIT_IC_CHECK (6 conditions, ~3 cache lines)  →  CALL js_jit_ic_read  →  DupValue
```

`js_jit_ic_read` is 4 source lines:
```c
JSValue js_jit_ic_read(JSContext *ctx, JSValue obj, uint32_t slot) {
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    return JS_DupValue(ctx, p->prop[slot].u.value);
}
```

Inlining it at each call site costs 2 additional instructions and gives GCC the object
pointer — already in a register from the IC check — to reuse for the load.

### Fix

In the code generator (`quickjs-jit.c`), change the IC hit block emitted for
`OP_get_field` / `OP_get_field2` from:

```c
"if (js_likely(JIT_IC_CHECK(_o,&%s)))\n"
"    _r=js_jit_ic_read(ctx,_o,%s.slot);\n"
```

to:

```c
"if (js_likely(JIT_IC_CHECK(_o,&%s))){\n"
"    JSObject *_p=(JSObject*)JS_VALUE_GET_PTR(_o);\n"
"    _r=_p->prop[%s.slot].u.value;\n"
"    JS_DupValue(ctx,_r);}\n"
```

Same change for the `OP_put_field` write path: inline `js_jit_ic_write`.

### Tasks

- [ ] **P11.1-A** Change `OP_get_field` IC hit emission in `quickjs-jit.c` to inline the
  `js_jit_ic_read` body.  Use `JS_VALUE_GET_PTR` (defined in `quickjs.h`) — not
  `JS_VALUE_GET_OBJ` (only in `quickjs.c`; caused the P10.5 dlopen bug).

- [ ] **P11.1-B** Change `OP_get_field2` IC hit emission similarly.

- [ ] **P11.1-C** Change `OP_put_field` IC write path: inline `js_jit_ic_write`
  (`set_value(ctx, &p->prop[slot].u.value, val)`).

- [ ] **P11.1-D** Rebuild `combined.so`, verify:
  `objdump -d combined.so | grep -c "call.*js_jit_ic_read"` = **0**.

- [ ] **P11.1-E** `make CONFIG_JIT=y test` passes.  Run V8bench 3× AOT, record scores.

### Definition of done
`objdump -d combined.so | grep -c "call.*js_jit_ic_read"` = 0.
V8bench DeltaBlue and Richards scores improve vs P10.5 baseline.

---

## P11.2 — Eliminate JSValue slot zero-initialization (fix Splay −24% regression)
**Effort:** ~1 day  **Risk:** low–medium  **Files:** `quickjs-jit.c`

### Problem

Every JIT function begins with:
```c
JSValue _tsv0=JS_UNDEFINED; JSValue _tsv1=JS_UNDEFINED; /* ... N slots */
```
This zeroes 16×N bytes unconditionally at function entry.  For a recursive `splay_()`
function with 10 stack slots called millions of times, this is >160 MB of unnecessary
write traffic per benchmark run.  The interpreter avoids this entirely: it pushes a
single `JSStackFrame` (~40 bytes) and `sp` arithmetic tracks live slots.

The initialization exists only for the exception cleanup path (`_ex:` label), which
must `_FREE` any live heap-reference slots.  The existing `if(_sp>N){_FREE(_tsvN);}`
pattern already uses `_sp` to decide what to free — it does not actually need the
upfront zero-initialization.

### Fix

Remove unconditional `=JS_UNDEFINED` initialization.  Declare slots as bare `JSValue`
variables.  Update the `_ex:` cleanup to continue using the `_sp` high-water mark
(already maintained correctly throughout the function body).

Correctness concern: if a goto jumps backward over a slot assignment, the slot may
hold garbage.  Audit the codegen for all backward edges (loops) — the existing
`_sp` tracking and `if(_sp>N)` guards at `_ex:` already handle this case correctly,
since `_sp` is only advanced after a slot is assigned.

An alternative simpler approach: keep initialization but switch from `JS_UNDEFINED`
(which zero-initializes the 16-byte struct) to `{.tag = JS_TAG_UNDEFINED}` and check
whether GCC can then use a register instead of a stack spill for unused slots.

### Tasks

- [ ] **P11.2-A** Audit all backward gotos in the generated C: confirm that `_sp` is
  never advanced past a slot before the slot has been written.  Write a test JS function
  with a loop and multiple slot assignments to verify.

- [ ] **P11.2-B** Change slot declaration generation in `emit_func_prologue()` (or
  equivalent) from `JSValue _tsvN=JS_UNDEFINED;` to `JSValue _tsvN;` (uninitialized).
  Add `__attribute__((uninitialized))` or equivalent if the compiler warns.

- [ ] **P11.2-C** Verify `_ex:` cleanup correctness with an ASAN + UBSAN build:
  `make CONFIG_JIT=y CONFIG_ASAN=y CONFIG_UBSAN=y && ./qjs tests/test_closure.js`.

- [ ] **P11.2-D** V8bench: run 3× `--jit-aot`, confirm Splay returns to ≥ interpreter
  baseline.  Record all 7 benchmark scores.

### Definition of done
Splay `--jit-aot` score ≥ interpreter baseline (≥2100).  ASAN+UBSAN clean.
Perf regression eliminated.

---

## P11.3 — Method call monomorphic IC
**Effort:** ~2 days  **Risk:** medium  **Files:** `quickjs-jit.c`, `quickjs.c`, `quickjs-jit.h`

### Problem

895 calls to `_RT->call` (vtable function pointer) remain in the generated code.  Each
goes through `JS_Call` → `JS_CallInternal` with full argument marshalling, argv
refcounting, and stack frame setup.  For a call site that always calls the same method
on the same type of object (monomorphic — the common case), this is pure overhead.

### Fix

Add a per-call-site cache analogous to the property IC:

```c
typedef struct {
    JSObject *expected_func;   /* NULL = cold */
    JSJITFunc direct_jit;      /* non-NULL = callee is JIT-compiled */
    JSValue  *callee_cpool;    /* callee's constant pool */
    JSVarRef **callee_var_refs;
} JSJITCallICEntry;
```

Generated code for a method call site:
```c
static JSJITCallICEntry _cic_N = {NULL, NULL, NULL, NULL};
JSObject *_fo = JS_VALUE_GET_TAG(_f)==JS_TAG_OBJECT ? (JSObject*)JS_VALUE_GET_PTR(_f) : NULL;
if (js_likely(_fo && _fo == _cic_N.expected_func)) {
    if (_cic_N.direct_jit) {
        if (_RT->poll_interrupts(ctx)) goto _ex;
        _r = _cic_N.direct_jit(ctx, _t, N, _ca, _cic_N.callee_cpool, _cic_N.callee_var_refs);
    } else {
        _r = _RT->call_direct(ctx, _f, _t, N, _ca); /* fast non-IC call */
    }
} else {
    _r = _RT->call(ctx, _f, _t, N, _ca);
    js_jit_callIC_fill(ctx, _f, &_cic_N);   /* fill on miss */
}
```

For JIT-compiled callees, `direct_jit` holds the `__jit_f_<hash>` pointer — this
subsumes P10.3's direct-call mechanism and extends it to all call sites, not just
statically-known closures.

### Tasks

- [ ] **P11.3-A** Define `JSJITCallICEntry` in `quickjs-jit.h`.

- [ ] **P11.3-B** Implement `js_jit_callIC_fill(ctx, func, ic)` in `quickjs.c`: extracts
  `expected_func`, detects if callee is JIT-compiled (`b->jit_func != NULL`), and stores
  `direct_jit`, `callee_cpool`, `callee_var_refs`.

- [ ] **P11.3-C** In the code generator, replace `_RT->call(...)` emission with the
  guarded call IC block for `OP_call`, `OP_call0`..`OP_call3`, `OP_call_method`.
  `OP_tail_call` / `OP_tail_call_method` may keep the vtable path initially.

- [ ] **P11.3-D** Handle megamorphic invalidation: if `_fo != _cic_N.expected_func` and
  the IC was previously filled, set `expected_func = JIT_IC_MEGAMORPHIC` sentinel and
  fall through to vtable permanently.

- [ ] **P11.3-E** Tests: `make CONFIG_JIT=y test`.  V8bench: Richards and DeltaBlue
  should show the largest gains (method-call-heavy).

### Definition of done
`grep -c "_RT->call\b" *.c` in cache drops by >50% for typical v8bench functions.
Richards AOT score improves by ≥20% vs P11.2 baseline.

---

## P11.4 — Inline array element fast path
**Effort:** ~0.5 day  **Risk:** low  **Files:** `quickjs-jit.c`

### Problem

161 `_RT->get_array_el` calls have no fast path.  Each is a full `JS_GetPropertyInternal`
call.  `js_jit_array_get` exists for writes (`set_array_el`), but read sites still go
through the vtable.  Crypto (RSA array arithmetic) and EarleyBoyer (array-heavy parser
state) are most affected.

### Fix

Inline the dense-array fast path at each `OP_get_array_el` call site, analogous to how
P11.1 inlines `js_jit_ic_read`.

For writes, `js_jit_array_set` is already used (78 sites) but it's still a function
call.  Inline that too.

Generated code for `OP_get_array_el`:
```c
{
  JSValue _o=_tsv1, _idx=_tsv0, _r;
  if (js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT &&
                JS_VALUE_GET_TAG(_idx)==JS_TAG_INT)) {
      JSObject *_ap = (JSObject*)JS_VALUE_GET_PTR(_o);
      uint32_t _ai = (uint32_t)JS_VALUE_GET_INT(_idx);
      if (js_likely(_ap->class_id == JS_CLASS_ARRAY &&
                    _ai < (uint32_t)_ap->u.array.count)) {
          _r = JS_DupValue(ctx, _ap->u.array.u.values[_ai]);
          goto _after_get_arr_N;
      }
  }
  _r = _RT->get_array_el(ctx, _o, _idx);
  _CHK(_r);
  _after_get_arr_N:;
}
```

The struct offsets (`class_id`, `u.array.count`, `u.array.u.values`) must be verified
with `_Static_assert` in `quickjs.c` (pattern from P10.5).

### Tasks

- [ ] **P11.4-A** Add `_Static_assert` checks in `quickjs.c` for: `offsetof(JSObject, class_id)`,
  `offsetof(JSObject, u.array.count)`, `offsetof(JSObject, u.array.u.values)`.  Add
  corresponding `JIT_OBJ_CLASSID_OFF` etc. constants to `quickjs-jit.h`.

- [ ] **P11.4-B** Change `OP_get_array_el` emission in the code generator to produce
  the inline fast path above.

- [ ] **P11.4-C** Inline `OP_set_array_el` / `js_jit_array_set` similarly for write sites.

- [ ] **P11.4-D** `make CONFIG_JIT=y test` passes.  V8bench Crypto and EarleyBoyer scores
  improve.

### Definition of done
`grep -c "_RT->get_array_el" *.c` in cache ≈ 0 for dense-array-heavy functions.
Crypto AOT score improves by ≥10%.

---

## P11.5 — Elide exception checks after provably safe operations
**Effort:** ~1 day  **Risk:** low  **Files:** `quickjs-jit.c`

### Problem

6090 `_CHK(r)` checks bloat the generated code.  Many are after operations that cannot
throw:
- Integer arithmetic that took the inline `JS_TAG_INT + JS_TAG_INT` fast path (no vtable)
- Comparisons between primitive types
- The inline array element path (after P11.4)
- Tag checks and boolean operations

Each `_CHK` is a branch + tag check that fragments basic blocks and prevents GCC from
merging adjacent loops or vectorising them.

### Fix

Track a `safe` flag in the code generator: when an operation is emitted entirely as
inline C without a vtable call, set `safe=1` and skip `_CHK` emission.  This requires
the arithmetic emitters to return whether they produced a safe value.

Specifically, the current arithmetic fast-path emits:
```c
if(_ta==JS_TAG_INT&&_tb==JS_TAG_INT){
    int64_t _r64=...;
    _tsvN=JS_NewInt32(ctx,...);  /* this cannot throw */
}
/* ... */
_CHK(_tsvN);   /* this check is dead for the int+int path */
```

The `_CHK` is needed only if the vtable slow path was taken.  For the int+int branch,
it can be skipped.  Best approach: emit the `_CHK` only at the point of the vtable call,
not after the entire if/else.

### Tasks

- [ ] **P11.5-A** Refactor arithmetic emission: move `_CHK` inside the `else` (vtable)
  branch rather than after the entire expression.  The int+int and float+float inline
  paths skip it entirely.

- [ ] **P11.5-B** After P11.4, the inline array element path never throws — remove
  `_CHK` from those sites.

- [ ] **P11.5-C** Comparisons between two JSValue operands that have both been checked as
  `JS_TAG_INT` in the inline path cannot throw — skip `_CHK`.

- [ ] **P11.5-D** Measure: count remaining `_CHK` calls.  Target: reduce from 6090 to
  <2000 across all cached functions.

- [ ] **P11.5-E** `make CONFIG_JIT=y test` passes.  Rebuild `combined.so` and run V8bench.

### Definition of done
`grep -c "_CHK(" *.c` in cache reduces by ≥50%.  No correctness regressions.

---

## P11.6 — INT32 type inference (separate from double)
**Effort:** ~2 days  **Risk:** medium  **Files:** `quickjs-jit.c`

### Problem

Phase 5's type inference emits `double _tsd[]` slots for numeric locals.  But the
majority of hot loops use integer arithmetic (loop counters, indices, integer sums).
Storing integers as `double` means:
- FPU register pressure instead of integer registers
- FP addition latency (3–5 cycles) instead of integer (1 cycle)
- Possible FP precision surprises requiring `(double)(int32_t)_dv==_dv` checks
- An extra conversion to/from JSValue (`JS_NewInt32` / `JS_VALUE_GET_INT`)

Typed double usage is only 8.2% of all value operations, and for integer-heavy benchmarks
like `count_primes`, `sum_loop`, and `fib`, the typed slots are in the wrong register
class.

### Fix

Extend the type lattice from `{JSVAL, NUMBER}` to `{JSVAL, INT32, FLOAT64}`.

Infer `INT32` for:
- Argument slots proven to be `JS_TAG_INT` at the call site (from `_aim` bits, already
  computed in P8.4)
- Locals assigned from `OP_push_i` (integer literal) 
- Locals assigned from integer arithmetic on `INT32` operands
- Loop counters assigned from `OP_inc_loc` / `OP_dec_loc` on an `INT32` slot
- `OP_get_length` result (always non-negative integer — see P11.8)

Emit `int32_t _ti_N` for `INT32`-typed slots.  Arithmetic between two `INT32` slots
emits:
```c
int64_t _r64 = (int64_t)_ti_A + _ti_B;
_ti_result = (int32_t)_r64;   /* if result still INT32 */
/* or: box to JSVAL if overflow or result escapes */
```

For comparisons between two `INT32` slots: emit a direct C comparison — no JSValue, no
vtable, no `_CHK`.

### Tasks

- [ ] **P11.6-A** Add `JIT_T_INT32` to the type enum in `quickjs-jit.c` (between
  `JIT_T_JSVAL` and `JIT_T_NUMBER`).  Add `int32_t _ti_N` declarations to the function
  prologue emitter.

- [ ] **P11.6-B** Propagate `JIT_T_INT32` through `OP_push_i`, `OP_add`, `OP_sub`,
  `OP_mul` (when both operands are `INT32` and result fits int32), `OP_inc_loc`,
  `OP_dec_loc` on integer slots.

- [ ] **P11.6-C** Emit native int32 arithmetic for `INT32 op INT32` cases.  Include
  overflow check using `__builtin_add_overflow` / `__builtin_mul_overflow` — on overflow,
  box to `JSValue` float64 and switch to `JIT_T_JSVAL`.

- [ ] **P11.6-D** Emit native int32 comparisons (`OP_lt`, `OP_lte`, `OP_gt`, `OP_gte`,
  `OP_eq`, `OP_strict_eq`) for `INT32 op INT32` — no vtable, no `_CHK`.

- [ ] **P11.6-E** Tests: `make CONFIG_JIT=y test`.  Micro-benchmark:
  `./qjs jit_perf_tests/bench_runner.js` — `count_primes` and `sum_loop` should show
  significant gains.

### Definition of done
`count_primes` AOT ≥ 5× interpreter baseline.  `sum_loop` AOT ≥ 1.5× baseline.
`make CONFIG_JIT=y test` passes cleanly.

---

## P11.7 — Persist IC state between `--jit-warmup` and `--jit-aot`
**Effort:** ~1.5 days  **Risk:** medium  **Files:** `quickjs-jit.c`, `quickjs.c`

### Problem

In `--jit-aot` mode, all `static JSJITICEntry _ic_N = {NULL, 0}` entries are cold at
startup.  Every property access on the first call to each function triggers an IC miss,
paying the full `JS_GetPropertyInternal` cost and the `js_jit_ic_fill_get` fill cost.

For a short benchmark run with v8bench's 1-second measurement windows, a significant
fraction of execution time is spent filling ICs that were already filled (and discarded)
during `--jit-warmup`.

### Fix

Serialize IC contents to a sidecar file during `--jit-warmup`.  Load and prime the
static IC entries during `--jit-aot` startup.

The challenge: `JSJITICEntry.shape` is a pointer — not stable across runs.  Instead,
serialize the shape *fingerprint*: a sorted list of `(atom, slot)` pairs (or a hash of
the shape's property array).  At `--jit-aot` time, resolve the fingerprint back to the
live shape pointer by scanning the runtime's shape table.

Alternatively, prime ICs by re-executing each function once with representative objects
from a stored "probe" data set.  Simpler but less precise.

### Tasks

- [ ] **P11.7-A** Define a sidecar format: `<hash>.ic` file containing one record per IC
  entry: `{uint16_t ic_index; uint32_t atom; uint32_t slot}`.  Written during `--jit-warmup`
  by a new `js_jit_save_ic_state(JSFunctionBytecode *b)` called after execution.

- [ ] **P11.7-B** In `--jit-aot`, after `js_jit_preload_combined()`, load each `.ic`
  file and prime the corresponding `JSJITICEntry` in `combined.so` by resolving the
  (atom, slot) pair against the current runtime via `js_jit_resolve_ic_entry(rt, atom, slot)`
  which walks the runtime's shape table to find a matching shape.

- [ ] **P11.7-C** Handle shape-not-found gracefully (skip; IC stays cold and fills lazily).

- [ ] **P11.7-D** Measure: compare first-run IC hit rate before and after.  V8bench 5×
  `--jit-aot` runs — variance should decrease and first-run score should improve.

### Definition of done
First `--jit-aot` run score is within 5% of runs 2–5 (no warmup penalty).

---

## P11.8 — Mark `OP_get_length` result as INT32
**Effort:** ~0.25 day  **Risk:** trivial  **Files:** `quickjs-jit.c`

### Problem

`OP_get_length` always returns a non-negative integer (`array.length`, `string.length`).
Currently it returns a `JSValue` (type = JSVAL).  Every loop bounded by an array length
(`for(i=0; i<arr.length; i++)`) therefore has a JSValue comparison:
```c
_tsv_len = arr.length (JSValue)
...
if (JS_VALUE_GET_TAG(i) == INT && JS_VALUE_GET_TAG(len) == INT && ...) ...
```

If `get_length` were marked `INT32`, and P11.6 makes the loop counter `INT32`, the
entire loop condition becomes a native int32 comparison with no JSValue involved.

### Fix

In the code generator, after emitting `OP_get_length`, push `JIT_T_INT32` onto the
gen_st type stack instead of `JIT_T_JSVAL`.  The emitted code already boxes the result
as `JSValue` (for compatibility with the current type system), so this is a no-op on the
emission side — it only changes the downstream type inference.

After P11.6 is in place, the `INT32`-typed length can be stored as `int32_t _ti_N`
directly: `get_length` emitter uses `_ti_N = (int32_t)js_jit_get_array_length(ctx, obj)`.

### Tasks

- [ ] **P11.8-A** In the gen_st type stack update for `OP_get_length`, push `JIT_T_INT32`
  (or `JIT_T_NUMBER` until P11.6 is done, which is the current state — just ensure it
  propagates correctly into P11.6).

- [ ] **P11.8-B** After P11.6: add `js_jit_get_array_length_int32(ctx, obj)` helper
  returning `int32_t`; emit directly into `_ti_N` slot.

- [ ] **P11.8-C** Verify that `for(var i=0; i<arr.length; i++)` loops in v8bench now
  compile with `int32_t` loop bounds after P11.6+P11.8.

### Definition of done
`for(...; i < arr.length; ...)` hot loop uses INT32 comparison.  No regressions.

---

## P11.9 — OSR (On-Stack Replacement) for top-level loops
**Effort:** ~5 days  **Risk:** high  **Files:** `quickjs-jit.c`, `quickjs.c`, `qjs.c`

### Problem

The JIT only activates at function call boundaries.  A script with a hot top-level loop
(or a long-running function that is called only once) never triggers JIT compilation at
the threshold.  The V8bench harness itself is a 1-second counting loop — the benchmark
function runs in the interpreter even when JIT is enabled for its callees.

### Fix

OSR allows the JIT to take over *mid-execution* of an already-running function:
1. The interpreter detects a backward branch (loop back-edge) that has exceeded the
   threshold.
2. A JIT version of the remaining loop body is compiled.
3. The interpreter's current variable state (value stack, local slots) is serialized
   into a JIT frame.
4. Execution continues in JIT mode.

This is the most complex item in Phase 11.  A simplified version (OSR at loop headers
only, no deoptimization back to interpreter) is achievable.

### Tasks

- [ ] **P11.9-A** Add a per-back-edge counter to the bytecode interpreter: increment at
  each `OP_goto` that goes backward.  Trigger OSR when counter exceeds threshold.

- [ ] **P11.9-B** Implement loop-body extraction: given a function bytecode and a loop
  header PC, extract the loop body as a separate JIT compilation unit.  The loop body
  takes as input: `(ctx, JSValue *locals, JSValue *stack, const uint8_t *resume_pc)`.

- [ ] **P11.9-C** Implement variable state serialization: map the interpreter's current
  `sp` and `var_buf` to the JIT function's `_tsv[]` / `_tsd[]` slots.

- [ ] **P11.9-D** Implement re-entry: after the OSR function returns, resume interpreter
  execution from the loop exit point.

- [ ] **P11.9-E** Tests: write a JS file with a hot top-level for-loop.  Verify JIT takes
  over before the loop ends.  Check correctness with `make CONFIG_JIT=y test`.

### Definition of done
A top-level `for(var i=0; i<1e7; i++) { ... }` compiles and executes via JIT.
V8bench harness loop itself (not just its callees) runs in JIT mode.

---

## P11.10 — Speculative type specialization with deoptimization
**Effort:** ~10+ days  **Risk:** very high  **Files:** `quickjs-jit.c`, `quickjs.c`

### Problem

The fundamental ceiling of the current JIT: it compiles to generic JSValue code.  A
mature JIT specializes on the observed argument types and property shapes, generating
native int32/float64 arithmetic with no boxing.  When assumptions are violated
(type mismatch), it "deoptimizes" — reconstructs the interpreter state and continues
in the interpreter.

Without this, the JIT cannot break the ~2× barrier on numeric benchmarks.  V8/SpiderMonkey/
JSC all get their 5–20× gains primarily from this mechanism.

### Architecture

1. **Profiling feedback in interpreter**: count argument types per call site and per
   loop iteration.  After N calls, attach a `JSJITTypeProfile` to `JSFunctionBytecode`.

2. **Speculative code generation**: at JIT compile time, read the type profile.
   For arguments/locals that are always `INT32`, generate a guard:
   ```c
   if (JS_VALUE_GET_TAG(argv[0]) != JS_TAG_INT) goto _deopt;
   int32_t _a0 = JS_VALUE_GET_INT(argv[0]);
   ```
   Then use `_a0` (native int32) throughout, with no boxing.

3. **Deoptimization handler**: `_deopt:` reconstructs a JSStackFrame from current JIT
   state, fills in value stack from local variables, and transfers control back to
   `JS_CallInternal` at the current bytecode PC.

4. **Invalidation**: if the type profile changes (a new call with different types), the
   JIT function is marked invalid and a new version (or a generic fallback) is compiled.

### Tasks

- [ ] **P11.10-A** Implement `JSJITTypeProfile` in `quickjs.c`: attach to
  `JSFunctionBytecode`; record observed tags for each argument slot and local variable
  over the first K calls.

- [ ] **P11.10-B** Add `--jit-speculative` CLI flag.  In speculative mode, the JIT reads
  the type profile and generates guarded specializations.

- [ ] **P11.10-C** Implement deoptimization: `_deopt:` label generates a
  `js_jit_deopt(ctx, locals, stack_depth, resume_pc)` call that reconstructs interpreter
  state and falls back.

- [ ] **P11.10-D** Test on micro-benchmarks first (`count_primes`, `sum_loop`) where types
  are perfectly stable.  Verify ≥5× speedup on those.

- [ ] **P11.10-E** Extend to v8bench.  Handle the case where speculation fires invalidation
  (e.g., a function called with both int and string args) — fall back to generic codegen.

### Definition of done
`count_primes` ≥ 8× interpreter baseline.  `sum_loop` ≥ 3× baseline.
All v8bench correctness tests pass.  No deopt loops.

---

## Milestone summary

| Sub-phase | Deliverable | Effort | Risk | Expected V8bench gain |
|---|---|---:|---|---:|
| P11.1 Inline ic_read | Zero calls to `js_jit_ic_read` in combined.so | 0.5 day | trivial | +5–15% DeltaBlue/Richards/RayTrace |
| P11.2 Slot init | Eliminate JSValue zero-init overhead | 1 day | low | +20–30% Splay/fib/recursion; fixes regression |
| P11.3 Call IC | Monomorphic method dispatch | 2 days | medium | +30–50% Richards/DeltaBlue |
| P11.4 Array IC | Inline dense array element access | 0.5 day | low | +10–20% Crypto/EarleyBoyer |
| P11.5 Elide CHK | Remove exception checks from safe ops | 1 day | low | +5–15% (compiler unlock) |
| P11.6 INT32 types | Native int32 arithmetic, no boxing | 2 days | medium | +3–8× count_primes/sum_loop |
| P11.7 Persist IC | Warm ICs on AOT startup | 1.5 days | medium | +5–10% cold AOT runs |
| P11.8 get_length INT32 | Enables P11.6 for array-bounded loops | 0.25 day | trivial | feeds P11.6 |
| P11.9 OSR | JIT activates for top-level loops | 5 days | high | unlocks new workload class |
| P11.10 Speculative | Guard-based type specialization + deopt | 10+ days | very high | 3–10× numeric |
| **P11.1–P11.5 total** | | **~5 days** | | **~1.5–2× over P10.5** |
| **P11.1–P11.8 total** | | **~9 days** | | **~2–3× over P10.5** |

### Per-benchmark projections (P10.5 → P11.1–P11.5)

| Benchmark | P10.5 median | P11.1–5 target | Driver |
|---|---:|---:|---|
| Richards | ~910 | ~1200–1400 | P11.3 call IC |
| DeltaBlue | ~1070 | ~1500–2000 | P11.1 + P11.3 |
| Crypto | ~1540 | ~1800–2500 | P11.4 array IC |
| RayTrace | ~1070 | ~1200–1500 | P11.1 inlined reads |
| EarleyBoyer | ~1500 | ~1700–2500 | P11.3 + P11.4 |
| RegExp | ~430 | ~430–500 | marginal (C regexp engine) |
| Splay | ~1710 | ~2200–2500 | P11.2 fixes regression |
| **Score** | **~1136** | **~1500–2000** | |

---

## Testing checklist (run after each sub-phase)

- [ ] `make CONFIG_JIT=y test` — full test suite passes
- [ ] `make CONFIG_JIT=y CONFIG_ASAN=y test` — no memory errors
- [ ] `(cd jit_perf_tests/v8bench && ../../qjs --jit-warmup run_qjs.js)` — warms cache
- [ ] `(cd jit_perf_tests/v8bench && ../../qjs --jit-link run_qjs.js)` — link step succeeds
- [ ] `(cd jit_perf_tests/v8bench && for i in 1 2 3; do ../../qjs --jit-aot run_qjs.js; done)` — 3 runs, record all scores
- [ ] `objdump -d ~/.cache/qjs-jit/combined.so | grep -c "call.*js_jit_ic_read"` = 0 (after P11.1)
- [ ] Splay AOT score ≥ interpreter baseline (after P11.2)
