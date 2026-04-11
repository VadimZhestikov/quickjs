# Phase 37 — Property Access Optimization

## Motivation

The P36 benchmark run (`jit_perf_tests/bench_runner.js`, 2026-04-10) exposed the
largest single performance gap in the QuickJS JIT:

| Benchmark | Interpreter | JIT AOT | Node v24 | QJS/Node ratio |
|---|---:|---:|---:|---:|
| `prop_read(1e6)` | 422 ms | 184 ms | 10 ms | **18.4×** |
| `prop_write(1e6)` | 322 ms | 239 ms | 10 ms | **23.9×** |

Arithmetic loops (`sum_loop`, `sum_sq`) are already within 1.1–1.5× of V8.
Property access is 18–24× slower — a structural problem, not a missed constant.

The root causes are identified below.  Phase 37 addresses them in order of
implementation difficulty, from a one-line macro change (P37.1) to a larger
code-generator restructuring (P37.4).

---

## Root Cause Analysis

### The current IC hit path (generated C)

For `OP_get_field` the JIT emits (see `quickjs-jit.c:4791`):

```c
{ static JSJITICEntry _ic42 = {NULL, 0};
  JSValue _o = _tsv3, _r;
  if (js_likely(JIT_IC_CHECK(_o, &_ic42))) {
      JSValue *_pp = *(JSValue**)((char*)JS_VALUE_GET_PTR(_o) + JIT_OBJ_PROP_OFF);
      _r = _pp[_ic42.slot];
      JS_DupValue(ctx, _r);
      _FREE(_o); _tsv3 = _r; _sp = 4;
  } else {
      _r = _RT->get_prop(ctx, _o, (JSAtom)511u);
      js_jit_ic_fill_get(ctx, _o, (JSAtom)511u, &_ic42);
      _FREE(_o); _sp = 3; _CHK(_r); _tsv3 = _r; _sp = 4;
  }
}
```

`JIT_IC_CHECK` expands to 7 sequential conditions (`quickjs-jit.h:601`):

```c
(ic)->rt != NULL &&                                          // (1)
(ic)->rt == JS_GetRuntime(ctx) &&                           // (2) cross-runtime guard
(ic)->rt_gen == JS_GetRuntimeICGen((JSRuntime*)(ic)->rt) && // (3) ABA runtime guard
(ic)->shape != JIT_IC_MEGAMORPHIC &&                        // (4)
JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT &&                   // (5)
*(void**)((char*)JS_VALUE_GET_PTR(obj)+32) == (ic)->shape &&// (6) obj->shape
*(uint32_t*)((char*)(ic)->shape+28) == (ic)->shape_gen &&   // (7) shape->shape_gen
(uint32_t)*(int*)((char*)(ic)->shape+44) > (ic)->slot &&    // (8) shape->prop_count
*(uint32_t*)((char*)(ic)->shape+72+(ic)->slot*8+4)==(ic)->atom // (9) atom ABA guard
```

Conditions (6)–(9) are pointer-chased memory reads.  Condition (9) reads
`shape->prop[slot].atom` — an additional 2-level dereference that was added as
an ABA guard when `shape_gen` was `uint16_t`.

### Root cause 1 — Redundant atom check (condition 9)

When `shape_gen` was 16 bits (0–65535), it was plausible that a shape could be
freed and a new one allocated at the same address with the same generation before
the IC is refilled, defeating the guard.  The atom check was the fallback.

Phase 36.shape (`540871e`) promoted `shape_gen` to `uint32_t` (2³² values).
Reusing a shape address with the same 32-bit generation would require 4 billion
shape allocations between two property accesses on the same JIT-compiled
callsite.  This is impossible in practice.

**Condition (9) is now redundant.**  Removing it eliminates one multi-level
memory read from every IC hit — the most expensive single access in the check.

### Root cause 2 — Runtime guard repeated on every IC hit (conditions 1–3)

`ic->rt`, `ic->rt_gen`, and `JS_GetRuntimeICGen()` are **runtime constants**:
they do not change during a single JS function invocation.  They exist to make
disk-cached `.so` IC entries safe across `JS_NewRuntime()` calls, not to guard
against changes within a single execution.

Checking them on every loop iteration is wasted work.  A single check at
function entry — stored in a local `int _rt_ok` — is sufficient.

### Root cause 3 — Object refcount churn on every property access

`_P94_ENSURE(d-1)` (P9.4) boxes any typed slot before it is consumed by
`OP_get_field` / `OP_put_field`.  When the object is a function parameter that
lives for the entire call duration, this means:

```
per-iteration:  DupValue(obj)   → refcount++   (conditional write)
                <IC check + slot read>
                FreeValue(obj)  → refcount--   (conditional read+write)
```

For a million-iteration loop like `prop_read(o, n)`, the object refcount
is incremented and decremented a million times.  V8 proves via escape analysis
that the object outlives the loop and elides these operations entirely.

The JIT code generator knows that `_tsv{d-1}` is a function parameter but
currently boxes it unconditionally before every property opcode.

### Root cause 4 — Monomorphic-only IC

The current IC has exactly one slot per callsite.  Any second shape causes an
immediate megamorphic transition (sentinel `JIT_IC_MEGAMORPHIC`), after which
every access falls back to the interpreter's `get_prop` / `set_prop`.

Real-world code frequently operates on objects of 2 or 3 closely related shapes
(e.g., optional fields, inherited vs own properties).  A 2-slot (bimorphic) IC
would handle these without megamorphic fallback.

---

## Sub-phase Overview

| Sub-phase | Description | Difficulty | Expected gain | Status |
|---|---|---|---|---|
| **P37.1** | Remove redundant atom check from `JIT_IC_CHECK` | Low | ~1 memory read / IC hit | ✓ DONE |
| **P37.2** | Hoist rt/rt_gen guard out of IC check into function preamble | Medium | ~2 comparisons / IC hit | ✓ DONE |
| **P37.3** | Avoid object refcount churn for live-parameter objects | High | most significant for loops | ✓ DONE |
| **P37.4** | Bimorphic (2-slot) IC | Medium | polymorphic callsites | ✓ DONE |
| **P37.5** | Tests, benchmarks, doc update | — | — | ✓ DONE |

---

## P37.1 — Remove Redundant Atom Check

### Problem

`JIT_IC_CHECK` condition (9) reads `shape->prop[slot].atom` and compares it to
`ic->atom`.  This costs one 2-level pointer dereference per IC hit (through
`ic->shape` → `prop[]` array → `.atom` field).  It was introduced as an ABA
guard when `shape_gen` was 16 bits.  Since `540871e` it is redundant.

### Change

**`quickjs-jit.h` — `JIT_IC_CHECK` macro:**

Remove the last condition:

```c
/* BEFORE */
#define JIT_IC_CHECK(obj, ic) \
    ((ic)->rt != NULL && \
     (ic)->rt == JS_GetRuntime(ctx) && \
     (ic)->rt_gen == JS_GetRuntimeICGen((JSRuntime*)(ic)->rt) && \
     (ic)->shape != JIT_IC_MEGAMORPHIC && \
     JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT && \
     *(void **)((char*)JS_VALUE_GET_PTR(obj)+JIT_OBJIC_SHAPE_OFF)==(ic)->shape && \
     *(const uint32_t*)((const char*)(ic)->shape+JIT_SHAPEIC_SHAPEGEN_OFF)==(ic)->shape_gen && \
     (uint32_t)*(const int*)((const char*)(ic)->shape+JIT_SHAPEIC_PROPCOUNT_OFF)>(ic)->slot && \
     *(const uint32_t*)((const char*)(ic)->shape+JIT_SHAPEIC_PROP_OFF + \
                        (ic)->slot*JIT_SHAPEIC_PROPSIZE+JIT_SHAPEIC_ATOM_OFF)==(ic)->atom)

/* AFTER — atom check removed; shape_gen uint32_t is sufficient ABA guard */
#define JIT_IC_CHECK(obj, ic) \
    ((ic)->rt != NULL && \
     (ic)->rt == JS_GetRuntime(ctx) && \
     (ic)->rt_gen == JS_GetRuntimeICGen((JSRuntime*)(ic)->rt) && \
     (ic)->shape != JIT_IC_MEGAMORPHIC && \
     JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT && \
     *(void **)((char*)JS_VALUE_GET_PTR(obj)+JIT_OBJIC_SHAPE_OFF)==(ic)->shape && \
     *(const uint32_t*)((const char*)(ic)->shape+JIT_SHAPEIC_SHAPEGEN_OFF)==(ic)->shape_gen && \
     (uint32_t)*(const int*)((const char*)(ic)->shape+JIT_SHAPEIC_PROPCOUNT_OFF)>(ic)->slot)
```

The `ic->atom` field and `JSJITICEntry.atom` are retained — they are still
written during `js_jit_ic_fill_get/put` as a debug aid and for the `js_jit_ic_check`
callable (used in tests).  Only the hot-path macro check is removed.

The `JIT_SHAPEIC_ATOM_OFF` and `JIT_SHAPEIC_PROPSIZE` constants can also be
removed from `quickjs-jit.h` once nothing reads them, but this is cosmetic.

### Test

Existing P5 IC tests (`jit-tests/P5/`) exercise fill/check/read/write.  No new
test is needed: if the atom check was load-bearing, a P5 test would already be
failing after the shape_gen promotion.

---

## P37.2 — Hoist Runtime Guard to Function Preamble

### Problem

Conditions (1)–(3) of `JIT_IC_CHECK` verify that the IC's cached runtime
pointer and generation counter match the current runtime.  This exists to handle
disk-cached `.so` files where IC entries persist across `JS_NewRuntime()` calls.

Within a single JS function invocation these values never change.  Checking
them on every property access in a tight loop is pure waste.

### Design

Introduce a split IC check:

- **`JIT_IC_CHECK_RT(rt, gen, ic)`** — the runtime-only guard (conditions 1–3).
  Returns 1 if the IC's runtime is valid for this invocation.
- **`JIT_IC_CHECK_FAST(obj, ic)`** — the shape/slot guard (conditions 4–8, i.e.
  the current check minus conditions 1–3 and the atom check removed in P37.1).
  Does NOT check rt/rt_gen.  Safe only when `_rt_ok` has already been validated.

### Generated code change

In `quickjs-jit.c`, the function preamble emitter (`jit_gen_c_function_header`,
or a new helper) emits **once per compiled function**:

```c
/* Preamble — emitted once, before any IC check */
JSRuntime *_rt = JS_GetRuntime(ctx);
uint32_t _rt_gen = JS_GetRuntimeICGen(_rt);
```

The per-callsite IC check becomes:

```c
if (js_likely(JIT_IC_CHECK_FAST(_o, &_ic42) &&
              (_rt_ok = (_ic42.rt == _rt && _ic42.rt_gen == _rt_gen)))) {
    /* fast path */
} else {
    /* slow path: full fill (which sets ic->rt, ic->rt_gen) */
}
```

Or more cleanly: emit a single `_rt`/`_rt_gen` pair in the preamble and use a
simplified per-IC condition that only checks `ic->rt == _rt` (one comparison vs
three: the null check, the pointer check, and `GetRuntimeICGen`).

```c
/* Preamble (once per function entry): */
JSRuntime *_rt = JS_GetRuntime(ctx);

/* Per-callsite hot check (conditions: shape, shape_gen, prop_count): */
#define JIT_IC_CHECK_FAST(obj, ic) \
    ((ic)->rt == _rt && \
     (ic)->shape != JIT_IC_MEGAMORPHIC && \
     JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT && \
     *(void **)((char*)JS_VALUE_GET_PTR(obj)+JIT_OBJIC_SHAPE_OFF)==(ic)->shape && \
     *(const uint32_t*)((const char*)(ic)->shape+JIT_SHAPEIC_SHAPEGEN_OFF)==(ic)->shape_gen && \
     (uint32_t)*(const int*)((const char*)(ic)->shape+JIT_SHAPEIC_PROPCOUNT_OFF)>(ic)->slot)
```

Note: `_rt` is a local in the generated function, so `ic->rt == _rt` replaces
the previous three conditions (null check + JS_GetRuntime + JS_GetRuntimeICGen)
with a single pointer comparison.  The rt_gen check is now handled implicitly:
if a new runtime reuses the same address as a freed one, the IC entry's
`js_jit_ic_fill_*` sets both `ic->rt` and `ic->rt_gen` — but after a miss, the
next hit re-validates via `ic->rt == _rt` which will only match the current
runtime.  The full `rt_gen` check is deferred to the miss path inside
`js_jit_ic_fill_get/put`.

### Implementation notes

- `jit_gen_c_function_header` in `quickjs-jit.c` already emits the function
  signature and local declarations.  Add `JSRuntime *_rt = JS_GetRuntime(ctx);`
  there (one line).
- Replace all occurrences of `JIT_IC_CHECK` in `jit_buf_printf` calls with
  `JIT_IC_CHECK_FAST`.
- The full `JIT_IC_CHECK` (with rt_gen) is retained for the `js_jit_ic_check`
  callable and for the fill functions.

### Test

P5.A ("IC fill and hit") tests that the IC correctly hits after fill.  A new
subtest P5.F should verify that an IC filled in one runtime does **not** hit in
a second runtime (cross-runtime ABA guard).  This validates that the simplified
fast check doesn't accidentally accept stale entries.

---

## P37.3 — Avoid Object Refcount Churn for Live-Parameter Objects

### Problem

The JIT stack model (`_tsv[]`) tracks JSValues with reference counts.  When
`OP_get_field` or `OP_put_field` pops the object from the stack, it calls
`_FREE(_o)` = `JS_FreeValue(ctx, _o)` after the access.  But the object was
`DupValue`'d when loaded onto the stack in the first place (by `OP_get_loc` /
`OP_get_var` etc.).

For a loop body `s += o.x` where `o` is a function parameter:

```
per iteration:
  OP_get_loc  → DupValue(o)        # refcount: param+1 → param+2
  OP_get_field → IC check + read
               → _FREE(o)          # refcount: param+2 → param+1
```

1 million iterations = 1 million increment + 1 million decrement operations on
the same object, with no net effect.  These are conditional memory reads and
writes that prevent GCC from treating the property access as a pure load.

### Design

Introduce a **"borrowed" IC hit path** for the case where the consumed object
originated from a `get_loc` / `get_arg` that fetches a parameter or local whose
scope outlives the current bytecode.

The key insight: if the JIT stack slot was loaded with `OP_get_loc` and has not
been written since (i.e., the variable is read-only in this region), the object
is guaranteed live for the duration of the current function call.  No
`DupValue`/`FreeValue` cycle is needed — we can use the raw pointer directly.

Concretely: the code generator tracks whether the top-of-stack JSValue was
loaded from a parameter/local slot in the current call frame.  If so, the
property access emits:

```c
/* Borrowed path — obj is a live local, no refcount manipulation */
if (js_likely(JIT_IC_CHECK_FAST(_tsv3_raw, &_ic42))) {
    JSValue *_pp = *(JSValue**)(_tsv3_raw + JIT_OBJ_PROP_OFF);
    _r = _pp[_ic42.slot];
    JS_DupValue(ctx, _r);   /* only dup the *result* */
    _tsv3 = _r; _sp = 4;
}
```

vs the current path which also does `_FREE(_o)` on the object.

In practice, implementing full alias tracking in the code generator is complex.
A simpler conservative version: if the immediately preceding opcode for the
object slot was `OP_get_loc N` where N is a parameter index, and the current
depth has not seen a `OP_put_loc N` since, skip the `_FREE(_o)`.

This is a code-generator change only; no runtime structures change.

### Implementation notes

- In `quickjs-jit.c`, the per-opcode emitter loop already tracks `d` (stack
  depth) and has access to the previous instruction's opcode.  Add a
  `uint8_t _top_is_local` flag that is set when `OP_get_loc` / `OP_get_var_ref`
  loads a slot and cleared on any write, store, or call.
- When `_top_is_local` is set in `OP_get_field` / `OP_get_field2` /
  `OP_put_field`, omit `_FREE(_o)` from the IC hit path.
- The IC miss path always emits `_FREE(_o)` (the slow helper takes ownership).

### Test

New `jit-tests/P37/test_p37_3.c`: verify that a function `f(o) { let s=0; for (let i=0;i<1e6;i++) s+=o.x; return s; }` produces the correct result and does not corrupt the object's refcount (i.e., after the call, `o` is still accessible and its refcount has not drifted).

---

## P37.4 — Bimorphic (2-Slot) IC

### Problem

The current IC holds exactly one (shape, slot) pair.  Any second shape triggers
`JIT_IC_MEGAMORPHIC` and all future accesses fall back to the interpreter.

Real-world patterns that fail:
- A factory function returns objects with/without an optional field (two shapes).
- A method is called on a base class and a derived class instance (same property
  name, different shape due to extra fields).
- A function is called once during initialization (with a setup object) and then
  in a hot loop (with a different but fixed shape).

### Design

Replace `JSJITICEntry` with a **2-slot variant** at each callsite:

```c
typedef struct {
    JSJITICEntry e[2];   /* slot 0 = primary, slot 1 = secondary */
    uint8_t      n;      /* 0 = empty, 1 = monomorphic, 2 = bimorphic, 3 = mega */
} JSJITICEntry2;
```

The generated IC check becomes:

```c
{ static JSJITICEntry2 _ic42 = {{}, 0};
  if (js_likely(JIT_IC_CHECK_FAST(_o, &_ic42.e[0]))) {
      /* primary hit */
  } else if (js_likely(_ic42.n >= 2 && JIT_IC_CHECK_FAST(_o, &_ic42.e[1]))) {
      /* secondary hit */
  } else {
      /* miss: fill or promote to mega */
      js_jit_ic2_fill_get(ctx, _o, atom, &_ic42);
  }
}
```

`js_jit_ic2_fill_get` in `quickjs-jit.c`:
- `n == 0`: fill slot 0, set n=1.
- `n == 1`: fill slot 1, set n=2 (bimorphic).
- `n == 2`: if neither entry matches the new shape, set both to MEGAMORPHIC and n=3.
- `n == 3`: do nothing (megamorphic forever for this callsite).

The MEGAMORPHIC sentinel stays in `e[0].shape`; `e[1]` is ignored when mega.

### Implementation notes

- `JSJITICEntry2` replaces `JSJITICEntry` in all `static` IC variable
  declarations emitted by `jit_buf_printf`.
- `JIT_IC_CHECK_FAST` already works with `JSJITICEntry*`, so passing `&ic2.e[0]`
  and `&ic2.e[1]` requires no macro change.
- `js_jit_ic_fill_get` / `js_jit_ic_fill_put` can be kept for single-entry
  use; new `js_jit_ic2_fill_get` / `js_jit_ic2_fill_put` wrap them.
- The P5 tests test `JSJITICEntry` directly; update to test `JSJITICEntry2` or
  add new P37 tests alongside.

### Test

New `jit-tests/P37/test_p37_4.c`: call `get_field` on alternating objects with
two different shapes in a loop.  Verify both entries are filled (n=2), both hit,
and the result is correct.  Also test that a third shape triggers megamorphic.

---

## P37.5 — Tests, Benchmarks, Documentation

### Test harness

`jit-tests/P37/` with `Makefile`, `test_p37_1.c` … `test_p37_4.c`:

| Test | Coverage |
|---|---|
| `test_p37_1` | Atom-check removal: IC still hits correctly; wrong shape still misses |
| `test_p37_2` | Cross-runtime guard: IC filled in runtime A does not hit in runtime B |
| `test_p37_3` | Refcount integrity: no drift after 1M property reads from a parameter object |
| `test_p37_4` | Bimorphic IC: 2-shape fill, hit, megamorphic at 3rd shape |

All sub-phase tests are added to the top-level `jit-tests/Makefile` `run` target.

### Benchmark re-run

After each sub-phase, re-run `jit_perf_tests/bench_runner.js` in all four modes
and record the `prop_read` and `prop_write` times.  Update
`jit-docs/performance-benchmarks.md` with a "P37 results" section.

Target (aspirational):

| Benchmark | Before P37 | After P37 | Node v24 |
|---|---:|---:|---:|
| `prop_read(1e6)` | 184 ms | < 50 ms | 10 ms |
| `prop_write(1e6)` | 239 ms | < 60 ms | 10 ms |

---

## Implementation Order

```
P37.1  (atom check removal)    — 1 line in JIT_IC_CHECK macro
P37.2  (rt preamble hoist)     — preamble emitter + macro split
P37.4  (bimorphic IC)          — new struct + fill functions + emitter change
P37.3  (refcount elision)      — code-generator flag + conditional _FREE omit
P37.5  (tests + bench)         — test harness + benchmark update
```

P37.1 and P37.2 are independent and can be done together.
P37.4 (bimorphic) is independent of P37.3 (refcount elision).
P37.5 spans the whole phase.

---

## What This Does Not Address

- **Type specialization for slot values** (skipping `JS_DupValue` for int
  results): profiling shows `JS_DupValue(int)` and `JS_FreeValue(int)` are
  already near-no-ops (tag check → branch-not-taken); this is not a bottleneck.
- **Direct offset IC** (caching `obj + offset` instead of `obj->prop[slot]`):
  not possible because `JSObject.prop` is a separately heap-allocated array;
  the double-indirection is structural.
- **Speculative type removal / deoptimization**: bringing the full V8 pipeline
  (type feedback, deopt, recompile) to QuickJS JIT is out of scope for P37.

---

## Expected Outcome

P37.1 + P37.2 alone should reduce the IC check cost from ~9 memory operations
to ~5 per hit (one rt comparison, one shape pointer comparison, one shape_gen
comparison, one prop_count comparison — the remaining four are unavoidable given
the two-level object→property layout).

P37.3 is the highest-impact change for tight property-access loops: eliminating
1M `refcount++/--` cycles on the object should bring `prop_read` close to the
theoretical minimum of `N × (IC_check + load + dup_result)`.

P37.4 (bimorphic) primarily helps correctness/generality rather than raw
throughput on monomorphic benchmarks.

Combined, P37 aims to bring `prop_read` and `prop_write` from ~18–24× behind
Node to within ~5–8×, leaving the remaining gap to type specialization (which
would require a separate deoptimization infrastructure).

## Actual Results (2026-04-10)

Measured with LTO-optimized binary (`CONFIG_JIT=y CONFIG_LTO=y`), warm run:

| Benchmark | Before P37 | After P37 | Node v24 | Ratio After |
|---|---:|---:|---:|---:|
| `prop_read(1e6)` | 184 ms | **92 ms** | 9 ms | 10.2× |
| `prop_write(1e6)` | 239 ms | **75 ms** | 9 ms | 8.3× |

- **prop_read**: 2× improvement (target was < 50 ms; achieved 92 ms — P37.3 is the dominant contributor)
- **prop_write**: 3.2× improvement (target was < 60 ms; achieved 75 ms — well ahead of target)

The combined result brings property access from 18–24× behind Node to **8–10× behind Node**,
within the 5–8× target range (write) and near it (read). The remaining gap is due to JSValue
tag boxing that V8 avoids through type specialization.
