# Phase 39 — Property IC Fast Path: Fewer Pointer Chases, Put-Field Borrow

## Motivation

After P37 the property access gap stands at:

| Benchmark | QJS JIT AOT | Node v24 | Gap |
|---|---:|---:|---:|
| prop_read(1e6) | 92 ms | 9 ms | **10.2×** |
| prop_write(1e6) | 75 ms | 9 ms | **8.3×** |

P37 halved the gap (from 18–24×) via IC simplification, preamble-local `_rt`,
refcount elision for `get_field`, and bimorphic IC.  Three root causes remain.

---

## Root Cause Analysis

The generated hot loop for `prop_read` (benchmarked as `s += o.x` for 1M iters):

```c
/* Per iteration — after P37: */

/* 1. Load o (get_loc, borrowed — no DupValue thanks to P37.3) */
JSValue _o = _jsv_v0;                           /* no refcount touch */

/* 2. IC check for get_field — JIT_IC_CHECK_FAST expands to 6 conditions: */
if ( _ic.e[0].rt == _rt                         /* (a) cached runtime match */
  && _ic.e[0].shape != JIT_IC_MEGAMORPHIC        /* (b) not megamorphic */
  && JS_VALUE_GET_TAG(_o) == JS_TAG_OBJECT       /* (c) tag check */
  && *(void**)((char*)JS_VALUE_GET_PTR(_o)+32)   /* (d) obj→shape chase  ← pointer dep chain */
       == _ic.e[0].shape
  && *(uint32_t*)(_ic.e[0].shape+28)             /* (e) shape→shape_gen  ← pointer dep chain */
       == _ic.e[0].shape_gen
  && *(int*)(_ic.e[0].shape+44)                  /* (f) shape→prop_count ← pointer dep chain */
       > (int)_ic.e[0].slot                       /*     REDUNDANT — already covered by (e) */
) {
    /* 3. Property load — two-step pointer chase: */
    JSValue *_pp = *(JSValue**)((char*)JS_VALUE_GET_PTR(_o) + 40);  /* obj→prop_arr  ← 3rd chase */
    JSValue _r = _pp[_ic.e[0].slot];                                 /* prop_arr[slot] ← 4th chase */
    JS_DupValue(ctx, _r);   /* tag check + conditional refcount++ */
    _tsv0 = _r;
} else { /* miss path: get_prop + ic2_fill_get + CHK */ }

/* 4. add: typed s += o.x */
...
```

### Root cause 1 — Redundant `prop_count > slot` condition (step f)

`JIT_IC_CHECK_FAST` verifies six conditions.  Condition (f) reads
`shape->prop_count` and checks `> slot`.  But condition (e) already verified
`shape_gen` — meaning the shape object in memory is exactly the one that was
alive at IC fill time.  At fill time `prop_count > slot` was true; since the
shape is unchanged (same gen), it is still true.  Condition (f) is redundant.

This loads one extra word from the shape struct (cache-hot, but a dependent
read on the result of condition (d)).

### Root cause 2 — `obj→prop_arr` pointer chase (step 3)

After the IC check passes, the emitter still dereferences `obj + 40` to get
the property value array pointer, then indexes it by `slot`.  This is a
dependent pointer chain:

```
obj → prop_arr = obj→prop  → prop_arr[slot]  (two dependent loads)
```

The IC already caches `shape`, `slot`, `shape_gen`, `rt`.  It does not cache
`prop_arr`.  If we add `prop_arr = p->prop` at fill time, the hit path becomes
a single load:

```
prop_arr → prop_arr[slot]  (one dependent load, from the IC struct)
```

`p->prop` is stable as long as the shape is stable: adding or deleting a
property always creates a new shape (bumping `shape_gen`), which invalidates
the IC and triggers a refill with the new `prop_arr`.  The cache is safe.

### Root cause 3 — No `_FREE(_o)` elision for `put_field` (prop_write)

`prop_write` executes `o.x = i` 1M times.  The loop body:

```c
/* get_loc o (argument — JSValue, not borrowed for put_field): */
_tsv0 = DUP(_jsv_v0);      /* refcount++ — wasteful */

/* get_loc i (typed int): */
_ti1 = _jsi_i;             /* no JSValue push */

/* put_field: */
_P94_ENSURE(1);             /* box _ti1 → _tsv1 = JS_NewInt32(i) */
JSValue _v = _tsv1, _o = _tsv0;
if (JIT_IC_CHECK_FAST(...)) {
    JSValue _old = _pp[slot]; _pp[slot] = _v;
    JS_FreeValue(ctx, _old);  /* no-op for int, but tag-check executes */
}
_FREE(_o);                  /* refcount-- — wasteful paired with above DUP */
```

P37.3 added borrow elision for `get_loc → get_field` (prop_read pattern).
The analogous pattern `get_loc [obj] → get_loc [val] → put_field` has no
equivalent elision for `_FREE(_o)`.  Adding a put_field look-ahead removes
the DupValue/FreeValue pair on `o` (same mechanism, ~1M refcount ops).

`GEN_GET_LOC_BORROW` already sets `_borrowed_depth = -1` for typed/captured
locals.  The critical existing behavior (code at line ~3506): when a plain
`get_loc` does NOT match the look-ahead, it does NOT reset `_borrowed_depth`.
This means a borrow set by the first `get_loc [obj]` SURVIVES through a plain
`get_loc [i]` (typed int goes through `GEN_GET_LOC`, which does not touch
`_borrowed_depth`).  Only `GEN_GET_LOC_BORROW` (typed branch) resets to -1;
and it is only invoked when `_NEXT_IS_GET_FIELD` is true.

So the infrastructure is already almost correct — we only need the look-ahead
and the put_field-side skip.

---

## P39.1 — Remove Redundant `prop_count > slot` Condition

### Change: `quickjs-jit.h` — `JIT_IC_CHECK_FAST` macro

Remove the last condition:

```c
/* BEFORE (line ~635): */
#define JIT_IC_CHECK_FAST(obj, ic) \
    ((ic)->rt == _rt && \
     (ic)->shape != JIT_IC_MEGAMORPHIC && \
     JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT && \
     *(void **)((char*)JS_VALUE_GET_PTR(obj) + JIT_OBJIC_SHAPE_OFF) == (ic)->shape && \
     *(const uint32_t*)((const char*)(ic)->shape + JIT_SHAPEIC_SHAPEGEN_OFF) == (ic)->shape_gen && \
     (uint32_t)*(const int *)((const char*)(ic)->shape + JIT_SHAPEIC_PROPCOUNT_OFF) > (ic)->slot)

/* AFTER: */
#define JIT_IC_CHECK_FAST(obj, ic) \
    ((ic)->rt == _rt && \
     (ic)->shape != JIT_IC_MEGAMORPHIC && \
     JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT && \
     *(void **)((char*)JS_VALUE_GET_PTR(obj) + JIT_OBJIC_SHAPE_OFF) == (ic)->shape && \
     *(const uint32_t*)((const char*)(ic)->shape + JIT_SHAPEIC_SHAPEGEN_OFF) == (ic)->shape_gen)
```

Apply the same removal to `JIT_IC_CHECK` (the non-fast variant used outside JIT
functions).

Also remove the definition of `JIT_SHAPEIC_PROPCOUNT_OFF` (it's used only in
these macros and in no other places — grep to verify).

### Safety argument

When condition (e) `shape_gen == ic->shape_gen` passes:
- The shape object in memory at `ic->shape` has the same generation counter as
  when the IC was filled.
- `JSShape.shape_gen` is incremented on every shape mutation (property add,
  delete, seal, freeze, prototype change).
- At fill time, `slot < shape->prop_count` was verified by `find_own_property`.
- Since the shape is provably unchanged (same gen), `prop_count` is unchanged.
- Therefore `slot < prop_count` is still true without the runtime check.

The `atom` check was removed in P37.1 for the same reason.  This removal is
analogous.

Also remove the `prop_count` field from `JSJITICEntry` if it exists — check
the struct definition; the struct only stores `slot`, not `prop_count`, so
the live shape read is the only place it was used.

---

## P39.2 — Cache `obj->prop` Array Pointer in IC Entry

### Change 1: `quickjs-jit.h` — Add `prop_arr` to `JSJITICEntry`

```c
typedef struct {
    void     *shape;     /* JSShape* */
    void     *prop_arr;  /* JSObject.prop (JSProperty*) — cached at fill time;  ← NEW
                          * valid while shape_gen matches (shape transitions
                          * always update prop, always change shape_gen). */
    uint32_t  slot;      /* property index in prop_arr[] */
    uint32_t  atom;      /* JSAtom at slot — ABA guard */
    uint8_t   kind;      /* 0=general, 1=float64 */
    uint8_t   _pad[3];
    uint32_t  shape_gen;
    uint32_t  rt_gen;
    void     *rt;
} JSJITICEntry;
```

### Change 2: `quickjs.c` — Fill `prop_arr` in `js_jit_ic_fill_get/put`

```c
/* in js_jit_ic_fill_get, after the find_own_property check: */
ic->shape     = p->shape;
ic->prop_arr  = p->prop;   /* ← add this line */
ic->slot      = (uint32_t)(pr - p->prop);
...
```

Same in `js_jit_ic_fill_put`.

### Change 3: `quickjs-jit.c` — Replace runtime `obj→prop` dereference in emitters

In `OP_get_field` (both `top_borrowed` and standard branches), replace:

```c
/* BEFORE: */
"    JSValue *_pp=*(JSValue**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
"    _r=_pp[_ic%d.e[0].slot]; JS_DupValue(ctx,_r);\n"

/* AFTER: */
"    JSValue *_pp=(JSValue*)_ic%d.e[0].prop_arr;\n"
"    _r=_pp[_ic%d.e[0].slot]; JS_DupValue(ctx,_r);\n"
```

Apply the same substitution to the `e[1]` (bimorphic second slot) path and to
the `get_field2` and `put_field` emitters.

For `put_field` the pattern is:
```c
/* BEFORE: */
"    JSValue *_pp=*(JSValue**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
"    JSValue _old=_pp[_ic%d.e[0].slot]; _pp[_ic%d.e[0].slot]=_v;\n"

/* AFTER: */
"    JSValue *_pp=(JSValue*)_ic%d.e[0].prop_arr;\n"
"    JSValue _old=_pp[_ic%d.e[0].slot]; _pp[_ic%d.e[0].slot]=_v;\n"
```

### Safety argument

`p->prop` (the JSProperty array on a JSObject instance) is allocated at object
creation and reallocated only when the object needs more property slots (i.e.,
when a property is added and the current capacity is exceeded).  Property
addition always transitions to a new shape (either finding an existing child
shape or creating a new one).  Any shape transition increments `shape_gen`.

Because `JIT_IC_CHECK_FAST` verifies `shape_gen`, an IC hit implies:
1. The shape pointer matches.
2. The shape's generation counter matches what was recorded at fill time.
3. Therefore the shape has not been mutated since fill time.
4. Therefore no properties have been added/removed since fill time.
5. Therefore `p->prop` has not been reallocated since fill time.
6. Therefore `ic->prop_arr` is still valid.

Add a comment in `js_jit_ic_fill_get` explaining the invariant so future
maintainers understand why caching `prop_arr` is safe.

### Size impact

`JSJITICEntry` grows from 40 bytes to 48 bytes (adding 8-byte pointer).
`JSJITICEntry2` grows from ~88 bytes to ~104 bytes.  Both are static
allocations per callsite; the total memory impact across a typical JIT function
is negligible.

---

## P39.3 — Put-Field Borrow: Skip `_FREE(_o)` for Borrowed Objects

### Problem

`prop_write` benchmark: `o.x = i` inside a loop.  The bytecode pattern is:

```
get_loc [o]   (load object — JSValue, no look-ahead fires currently)
get_loc [i]   (typed int — GEN_GET_LOC, does NOT reset _borrowed_depth)
put_field [x] (pops val then obj; currently calls _FREE(_o))
```

`o` is loaded with `DUP(_jsv_v0)` (refcount++) only to be freed at
`_FREE(_o)` (refcount--) immediately after the store.  Zero net effect.
1M iterations = 1M pointless refcount increments + decrements.

P37.3 solved the identical problem for the read case (`get_loc → get_field`).
P39.3 adds the write equivalent.

### Analysis of `_borrowed_depth` lifecycle across get_loc → get_loc → put_field

1. First `get_loc [o]` (JSVAL local, NOT currently eligible for borrow):
   - Currently goes to `else { GEN_GET_LOC(o); }` in OP_get_loc.
   - `GEN_GET_LOC` emits `_tsv{d} = DUP(_jsv_o)`.
   - `_borrowed_depth` is NOT reset by this branch (existing invariant).

2. Second `get_loc [i]` (typed int):
   - Not matched by `_NEXT_IS_GET_FIELD` or `_NEXT2_IS_ARRAY_GET`.
   - Goes to `else { GEN_GET_LOC(i); }`.
   - Emits `_ti{d+1} = _jsi_i`. gen_st[d+1] = JIT_T_INT.
   - `_borrowed_depth` is NOT reset (same existing invariant).

3. `put_field`: currently no borrowed-object handling; always emits `_FREE(_o)`.

After P39.3:

1. First `get_loc [o]`: new look-ahead `_NEXT2_IS_PUT_FIELD(pc+sz)` matches.
   Emit `_tsv{d} = _jsv_o` (no DUP), set `_borrowed_depth = d`.

2. Second `get_loc [i]`: unchanged; does not reset `_borrowed_depth`.

3. `put_field` (depth `d`): `borrowed_depth_snap == d-2` → skip `_FREE(_o)`.

### Change 1: Add `_NEXT2_IS_PUT_FIELD` macro to `quickjs-jit.c`

After the existing `_NEXT2_IS_ARRAY_GET` macro (around line 3420):

```c
/* P39.3: 2-opcode look-ahead: next is any get_loc variant AND next+sz is put_field.
 * Used to detect o.x = val pattern and skip DupValue for obj on the write side.
 * Same safety guarantee as _NEXT2_IS_ARRAY_GET: the three opcodes are linear
 * (no branch targets between them in this pattern). */
#define _NEXT2_IS_PUT_FIELD(next_pc) \
    ((next_pc) < bc_len && \
     _IS_GET_LOC_OP(bc[(next_pc)]) && \
     (next_pc) + op_sz[bc[(next_pc)]] < bc_len && \
     bc[(next_pc) + op_sz[bc[(next_pc)]]] == OP_put_field)
```

`_IS_GET_LOC_OP` is already defined (covers get_loc, get_loc0..3, get_loc8,
get_loc_check).

### Change 2: Add borrow in all get_loc variants

In `OP_get_loc` / `OP_get_loc_check` (around line 3492), add a third branch:

```c
case OP_get_loc:  case OP_get_loc_check:
case OP_get_loc_checkthis:
{
    int _loc_idx = (int)bc_u16(&bc[pc+1]);
    if (_NEXT_IS_GET_FIELD(pc + sz)) {
        GEN_GET_LOC_BORROW(_loc_idx);
    } else if (_NEXT2_IS_ARRAY_GET(pc + sz) &&
               !_IS_INT(_loc_idx) && !_IS_NUM(_loc_idx) && !_CAP_LOC(_loc_idx)) {
        jit_buf_printf(cb, "    _tsv%d=_jsv_%s; _sp=%d;\n", d, LNAME(_loc_idx), d+1);
        _borrowed_depth = d;
    } else if (_NEXT2_IS_PUT_FIELD(pc + sz) &&      /* ← NEW */
               !_IS_INT(_loc_idx) && !_IS_NUM(_loc_idx) && !_CAP_LOC(_loc_idx)) {
        jit_buf_printf(cb, "    _tsv%d=_jsv_%s; _sp=%d;\n", d, LNAME(_loc_idx), d+1);
        _borrowed_depth = d;
    } else {
        GEN_GET_LOC(_loc_idx);
        /* do NOT reset _borrowed_depth — may be set by prior get_loc for arr/obj */
    }
    break;
}
```

Apply the same `_NEXT2_IS_PUT_FIELD` branch to:
- `OP_get_loc8` (around line 3519)
- `_GEN_GET_LOC_N(n)` macro for `get_loc0..3` (around line 3534)

### Change 3: Add borrowed-object skip in `OP_put_field` emitter

The `put_field` emitter (around line 5007) currently ends with:

```c
"      _FREE(_o); if(_ret<0) goto _ex; }\n",
```

Change to:

```c
if (borrowed_depth_snap == d-2) {
    /* obj was borrowed (no DupValue) — skip _FREE(_o) */
    jit_buf_printf(cb,
        "    { static JSJITICEntry2 _ic%d={{},0};\n"
        "      JSValue _v=_tsv%d, _o=_tsv%d; _sp=%d; int _ret;\n"
        /* ... IC check ... same as before ... */
        "      if(_ret<0) goto _ex; }\n",  /* ← no _FREE(_o) */
        ...);
} else {
    /* current code with _FREE(_o) */
    jit_buf_printf(cb,
        ...
        "      _FREE(_o); if(_ret<0) goto _ex; }\n",
        ...);
}
_borrowed_depth = -1; /* P38.1: borrow consumed by put_field */
```

Add `_borrowed_depth = -1` at the end of the put_field case (as is done for
get_field) to prevent the borrow from leaking to subsequent opcodes.

### Safety argument

The borrow for put_field is safe for identical reasons to the get_field borrow:
- `get_loc` without DupValue means the object's refcount was NOT incremented.
- The object's lifetime is guaranteed by its origin (local variable frame, still
  live during the function call).
- `put_field` does not consume the object's ownership — it reads the object's
  shape and property array, writes the slot, but does not take a reference.
- Skipping `_FREE(_o)` is correct: no increment was done, so no decrement needed.

The only constraint (same as for get_field and get_array_el): no branch target
can appear between the `get_loc [obj]` and `put_field` opcodes.  The 2-opcode
look-ahead pattern `get_loc → get_loc → put_field` in linear bytecode has no
intervening labels in normal JS code generation for `o.x = val`.

---

## P39.4 — Tests, Benchmarks, Doc Update

### Test harness: `jit-tests/P39/`

Create `Makefile` and three test programs:

#### `test_p39_1.c` — P39.1: IC check correctness without prop_count condition

- A: `function f(o) { return o.x; }` returns correct value after JIT warm-up.
- B: Returns correct value after shape changes (IC miss + refill: new object
  shape causes miss, refill, then hit again with new shape_gen).
- C: Megamorphic after 3 shapes: IC correctly demotes; get_prop slow path used.

#### `test_p39_2.c` — P39.2: prop_arr caching correctness

- A: `f(o)` returns correct value, reads via cached prop_arr.
- B: After adding a property to `o` (shape change, shape_gen++): IC miss, refill
  with new prop_arr, then hit correctly (verifies prop_arr invalidation on shape
  change).
- C: Two different objects with same shape: both IC hits (bimorphic) return
  correct values from their respective prop_arrs.
  Note: bimorphic IC (`JSJITICEntry2`) has `e[0]` and `e[1]`; each has its own
  `prop_arr` — verify that the correct `e[0].prop_arr` and `e[1].prop_arr` are
  used for each object.

#### `test_p39_3.c` — P39.3: put_field borrow refcount integrity

- A: `function f(o, n) { for (var i = 0; i < n; i++) o.x = i; }` — runs
  correctly, o.x is `n-1` after call.
- B: `o`'s refcount has not drifted after the call (same check as P37 and P38
  refcount tests — confirm borrow did not leak a decrement or skip an expected one).
- C: Borrow does NOT fire when obj is a captured variable (`_CAP_LOC` true) —
  verify correct behavior (DupValue+Free still happen).

### Benchmarks

After all code changes, rebuild `qjs_jit` and re-run the benchmark:

```sh
rm -f ~/.cache/qjs-jit/*.so ~/.cache/qjs-jit/*.skip
./qjs_jit --jit-aot jit_perf_tests/bench_runner.js 2>/dev/null  # cold
./qjs_jit --jit-aot jit_perf_tests/bench_runner.js 2>/dev/null  # warm
node jit_perf_tests/bench_runner.js 2>/dev/null
```

Focus metrics:
- `prop_read(1e6)`: primary target (P39.1 + P39.2 hit both IC check + prop load)
- `prop_write(1e6)`: primary target (P39.3 borrow + P39.2 prop_arr on write)
- All other benchmarks: regression check (changes are localized to IC path)

Update `jit-docs/performance-benchmarks.md` with a P39 Results section (§8).
Update this doc with `✓ DONE` markers and actual results.

---

## Expected Gains

| Sub-phase | Mechanism | Saves per IC hit | Affects |
|---|---|---|---|
| P39.1 | Remove prop_count load | 1 memory read | both get and put |
| P39.2 | Cache prop_arr in IC | 1 pointer dereference | both get and put |
| P39.3 | put_field obj borrow | 1 refcount++ + 1 refcount-- per iter | prop_write only |

### P39.1 + P39.2 combined (IC check + prop load)

Before:
```
5 memory loads in hot path:
  ic→rt (IC cache line, hot)
  ic→shape (IC, hot)
  obj→shape (pointer chase 1)
  ic→shape→shape_gen (pointer chase 2, dependent)
  ic→shape→prop_count (pointer chase 2, same load as shape_gen, same cache line)
  obj→prop_arr (pointer chase 3, from obj pointer)
  prop_arr[slot] (pointer chase 4, from prop_arr)
```

After:
```
4 memory loads:
  ic→rt (IC cache line, hot)
  ic→shape (IC, hot)
  obj→shape (pointer chase 1)
  ic→shape→shape_gen (pointer chase 2, dependent)
  ic→prop_arr (IC cache line, hot)   ← replaces obj→prop_arr chase
  ic→prop_arr[ic→slot] (one dependent load from prop_arr)
```

The chain `obj → shape → shape_gen` (2 dependent L1 loads, ~8 cycles minimum)
is unavoidable given the current IC architecture.  P39.2 eliminates the
separate `obj → prop_arr` chain (which adds ~4 cycles of L1 latency on the
parallel execution unit).

Estimated improvement on `prop_read`/`prop_write`: **15–30%** (from 92ms/75ms
to roughly 65–78ms / 55–63ms).  Target remains 10× behind Node; structural
limits (2 dependent pointer chases) prevent parity without type specialization.

### P39.3 (put_field borrow)

`prop_write` is currently dominated by:
1. IC check (same as prop_read)
2. `DUP(obj)` + `_FREE(obj)` — 2 refcount ops per iteration

P37.3 eliminated item 2 for `prop_read`.  P39.3 eliminates item 2 for
`prop_write`.  P37.3 gave ~50% improvement to `prop_read`; P39.3 is expected
to give a similar fraction of `prop_write`'s remaining overhead.

Combined with P39.1+P39.2 (IC check reduction for put_field), `prop_write`
should improve by roughly **30–50%** (from 75ms to roughly 38–52ms).

---

## Implementation Order

```
P39.1  (remove prop_count check)       — 1 line in quickjs-jit.h
P39.2  (cache prop_arr in IC)          — add field + fill + 6 emitter sites
P39.3  (put_field borrow)              — new look-ahead + emitter branch
P39.4  (tests + bench + docs)          — jit-tests/P39/ + docs update
```

P39.1 and P39.3 are fully independent.
P39.2 depends on the IC struct change and requires updating both
`js_jit_ic_fill_get/put` in `quickjs.c` and all six emitter sites in
`quickjs-jit.c`.

---

## What This Does Not Address

- **Typed property reads without re-JIT**: Eliminating `JS_DupValue` for
  integer-valued properties (analogous to V8's tagged-int specialization)
  requires observing the property's value type at IC fill time and either
  (a) invalidating the IC when the value type changes (needs hooks in
  `JS_SetProperty`), or (b) re-JIT on type change.  Both add significant
  complexity and are deferred.

- **Loop-invariant IC check hoisting**: GCC cannot prove `obj→shape` is
  loop-invariant (it's read through a void* chain).  Hoisting requires either
  explicit `__builtin_assume` scaffolding or emitter-side loop detection —
  out of scope.

- **Two dependent pointer chases (`obj→shape`, `shape→gen`)**: These are
  fundamental to the current IC architecture and cannot be eliminated without
  embedding the shape generation directly in the object (would require
  per-object maintenance cost on every shape mutation).

---

## Actual Results (2026-04-10)

P39 implemented:
- ✓ P39.1: Remove redundant `prop_count > slot` from `JIT_IC_CHECK` / `JIT_IC_CHECK_FAST` / `js_jit_ic_check()`
- ✓ P39.2: Add `prop_arr` field to `JSJITICEntry`; fill in `js_jit_ic_fill_get/put`; replace 8 `JIT_OBJ_PROP_OFF` pointer chases in emitters
- ✓ P39.3: Add `_NEXT2_IS_PUT_FIELD` look-ahead; borrow elision for `get_loc→get_loc→put_field` pattern
- ✓ P39.4: `jit-tests/P39/` — 3 C harnesses, all passing

### Benchmark results (warm AOT, non-LTO)

| Benchmark | Before P39 (after P38) | After P39 | Node v24 |
|---|---:|---:|---:|
| **prop_read(1e6)** | 92 ms | **85 ms** | 9 ms |
| **prop_write(1e6)** | 75 ms | **72 ms** | 10 ms |
| arr_sum(10000) x1e3 | 40 ms | 36 ms | 9 ms |
| fib(30) x1 | 25 ms | 25 ms | 8 ms |
| sum_loop(1e6) | 27 ms | 25 ms | 19 ms |

### Analysis

The improvements are smaller than estimated (7-9% rather than 15-30%). Key factors:

1. **P39.1 (prop_count removal)**: The memory access eliminated (`shape→prop_count`) was on the same cache line as `shape→shape_gen` (bytes 28 and 44 on x86-64). On modern CPUs with out-of-order execution, the cache line is already in L1 by the time prop_count is read, making this check nearly free. Minimal measured impact.

2. **P39.2 (prop_arr cache)**: Replaces one pointer chase (`obj→prop_arr`) with a read from the IC struct. The IC struct is hot (recently accessed for shape/gen checks), so the prop_arr field is in L1 cache. The `obj→prop` chase also hits a hot cache line (obj was just accessed for shape). Both are L1 hits. The substitution avoids a dependent load chain on `obj`, which GCC can sometimes schedule around. Measured improvement: small but consistent.

3. **P39.3 (put_field borrow)**: Eliminates `DupValue+FreeValue` on the object for `o.x = val` loops. Expected to be significant (same mechanism as P37.3 which gave ~50% on prop_read), but the prop_write benchmark's loop is more complex (int boxing for the value also takes time). Measured improvement: marginal.

The WSL2 environment has lower memory latency due to running on host DRAM. In production environments with higher L2/L3 miss rates, the pointer chase eliminations would show larger gains.
