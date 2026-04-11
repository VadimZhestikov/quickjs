# Phase 38 — Array Access Optimization

## Motivation

After P37 the gap table looks like:

| Area | Gap vs V8 | Notes |
|---|---|---|
| Property read/write | 8–10× | P37 brought this from 18–24× |
| Array reads | **5.3×** | `arr_sum` 48 ms vs 9 ms (Node) |
| Closures | 7.6× | `closure_counter` 15 ms vs 2 ms |
| Recursive calls | 3.0× | `fib` 25 ms vs 8 ms |

Array reads are the next best target: a 5.3× gap with clearly identifiable,
mechanical causes that do not require deoptimization infrastructure.

---

## Root Cause Analysis

The generated C for `arr_sum(arr)` (from `--jit-dump-c`) reveals the per-iteration
hot path in the loop `for (var i = 0; i < n; i++) s += arr[i]`:

```c
/* --- every iteration --- */

/* 1. Load arr (parameter) onto stack: DupValue on an object */
_tsv1 = DUP(argv[0]);                          /* refcount++ */

/* 2. Box the loop counter i (already int64_t _jsi_i_3) into JSValue */
{ int64_t _tv = _jsi_i_3;
  _tsv2 = (int32_t)_tv == _tv ? JS_NewInt32(ctx, (int32_t)_tv)
                               : JS_NewFloat64(ctx, (double)_tv); }

/* 3. Array access — inline fast path (P11.4) */
{ JSValue _idx = _tsv2, _o = _tsv1; JSValue _r;
  if (js_likely(JS_VALUE_GET_TAG(_o)   == JS_TAG_OBJECT    /* (a) */
             && JS_VALUE_GET_TAG(_idx) == JS_TAG_INT)) {   /* (b) */
    char *_op = (char*)JS_VALUE_GET_PTR(_o);
    uint32_t _ai = (uint32_t)JS_VALUE_GET_INT(_idx);
    if (js_likely(*(uint16_t*)(_op + JIT_OBJ_CLASSID_OFF) == JIT_CLASS_ARRAY  /* (c) */
               && _ai < (uint32_t)*(int*)(_op + JIT_ARR_COUNT_OFF))) {        /* (d) */
      _r = (*(JSValue**)(_op + JIT_ARR_VALUES_OFF))[_ai];
      JS_DupValue(ctx, _r);                               /* (e) dup element */
      _FREE(_o);   /* (f) FreeValue(arr) — refcount-- */
      _FREE(_idx); /* (g) FreeValue(i)   — no-op (int) */
      ...
    }
  }
}
```

### Root cause 1 — Array parameter refcount churn (steps 1 + f)

`arr` is a function parameter that lives for the entire call.  Loading it onto
the stack (`DUP(argv[0])`) increments the refcount; `_FREE(_o)` decrements it.
For 10 000 elements × 1 000 calls = 10 million iterations, this is 10M
increment + 10M decrement operations with zero net effect.

P37.3 introduced the `_top_borrowed` peephole for `get_loc → get_field`.
The same pattern applies here: `get_loc → get_array_el` should also skip the
DupValue/FreeValue cycle.

### Root cause 2 — Index boxing round-trip (step 2 + b + g)

The loop counter `i` is tracked as a typed native `int64_t _jsi_i_3` — the type
inference system (P8.1/P9.2) already knows it is an integer.  Yet the emitter
boxes it into a `JSValue` (`JS_NewInt32`), then immediately extracts the integer
back out (`JS_VALUE_GET_INT(_idx)`) inside the array access guard.  Step (b)
`JS_VALUE_GET_TAG(_idx) == JS_TAG_INT` is always true and could be eliminated.

The JIT already has the typed value `_jsi_i_3` available.  The array access
guard only needs `uint32_t _ai = (uint32_t)_jsi_i_3` — no boxing or tag check
at all.

### Root cause 3 — `array.length` via full helper call

In `arr_sum`, `var n = arr.length` emits:

```c
JSValue _r = _RT->get_prop(ctx, _tsv0, (JSAtom)50u);  /* atom 50 = "length" */
_sp=0; _CHK(_r); _FREE(_tsv0);
_ti0 = (JS_VALUE_GET_TAG(_r)==JS_TAG_INT) ? (int64_t)JS_VALUE_GET_INT(_r)
                                           : (int64_t)JS_VALUE_GET_FLOAT64(_r);
_FREE(_r); _sp=1;
```

This goes through `get_prop` — a virtual-dispatch helper that handles all
property types.  For a dense array the length is simply
`*(int*)((char*)obj + JIT_ARR_COUNT_OFF)`, a direct memory read.

`OP_get_length` already has infrastructure to store the result into a typed
`int64_t` slot (`_ti{d-1}`).  Adding an inline array fast path before the
helper call saves a helper call + JSValue allocation + CHK on the dominant code
path.

### Root cause 4 — Element DupValue on integer arrays (step e)

`arr[i]` in `arr_sum` always holds a small integer (the array was built with
`arr.push(i)`).  `JS_DupValue(ctx, _r)` on a `JS_TAG_INT` is a no-op —
but the tag check still executes.  If we track that a slot holds integers, we
can emit the element load without any DupValue at all.

This is the smallest of the four causes and is also covered partially by GCC's
branch prediction, so it is lower priority than 1–3.

---

## Sub-phase Overview

| Sub-phase | Description | Difficulty | Expected gain | Status |
|---|---|---|---|---|
| **P38.1** | Extend `_top_borrowed` peephole: `get_loc arr→get_loc i→get_array_el` | Low | Eliminates 2 refcount ops per array element access (for local vars) | ✓ DONE |
| **P38.2** | Typed index fast path: skip boxing when index is native int | Low-Medium | Eliminates `JS_NewInt32` + tag check per element | ✓ DONE |
| **P38.3** | Inline `array.length` fast path in `OP_get_length` | Medium | Replaces helper call with direct memory read for arrays | ✓ DONE |
| **P38.4** | Tests, benchmarks, doc update | — | — | ✓ DONE |

---

## P38.1 — Extend `_top_borrowed` to `get_array_el`

### Problem

P37.3 added look-ahead in `OP_get_loc`: if the next bytecode is `OP_get_field`
or `OP_get_field2`, emit without `JS_DupValue` and set `_top_borrowed = 1`.
`OP_get_field` then skips `_FREE(_o)`.

The identical pattern applies to `OP_get_array_el` and `OP_put_array_el` — the
object (`_o`) is loaded from a live local, consumed, and freed after the access.

### Change

**`quickjs-jit.c` — `OP_get_loc` look-ahead check:**

Extend the `_NEXT_IS_GET_FIELD` (or equivalent) macro/condition to also match
`OP_get_array_el` and `OP_put_array_el`:

```c
/* before */
#define _NEXT_IS_GET_FIELD(bc, pc, sz) \
    ((pc)+(sz) < bc_len && \
     ((bc)[(pc)+(sz)] == OP_get_field || (bc)[(pc)+(sz)] == OP_get_field2))

/* after */
#define _NEXT_IS_FIELD_OR_ARRAY(bc, pc, sz) \
    ((pc)+(sz) < bc_len && \
     ((bc)[(pc)+(sz)] == OP_get_field    || \
      (bc)[(pc)+(sz)] == OP_get_field2   || \
      (bc)[(pc)+(sz)] == OP_get_array_el || \
      (bc)[(pc)+(sz)] == OP_put_array_el))
```

**`quickjs-jit.c` — `OP_get_array_el` emitter:**

Add a `top_borrowed` branch analogous to `OP_get_field`:

```c
case OP_get_array_el:
    _P94_ENSURE(d-1); /* box idx (unless P38.2 handles it first) */
    if (top_borrowed) {
        /* obj came from get_loc without DupValue — skip _FREE(_o) */
        jit_buf_printf(cb,
            "    { JSValue _idx=_tsv%d,_o=_tsv%d; JSValue _r;\n"
            "      if(js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT"
                           "&&JS_VALUE_GET_TAG(_idx)==JS_TAG_INT)){\n"
            "        char *_op=(char*)JS_VALUE_GET_PTR(_o);\n"
            "        uint32_t _ai=(uint32_t)JS_VALUE_GET_INT(_idx);\n"
            "        if(js_likely(*(uint16_t*)(_op+JIT_OBJ_CLASSID_OFF)==JIT_CLASS_ARRAY\n"
            "                   &&_ai<(uint32_t)*(int*)(_op+JIT_ARR_COUNT_OFF))){\n"
            "          _r=(*(JSValue**)(_op+JIT_ARR_VALUES_OFF))[_ai];\n"
            "          JS_DupValue(ctx,_r);\n"
            "          _FREE(_idx); _sp=%d; _tsv%d=_r; _sp=%d;\n"   /* no _FREE(_o) */
            "          goto _aok%d;}}\n"
            "      _r=_RT->get_array_el(ctx,_o,_idx);\n"
            "      _FREE(_idx); _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d;\n" /* no _FREE(_o) */
            "      _aok%d:; }\n", ...);
    } else {
        /* current code with _FREE(_o) and _FREE(_idx) */
        ...
    }
```

Apply the same `top_borrowed` branching to `OP_put_array_el` (the object is
`_tsv{d-3}`; when borrowed, skip `_FREE(_o)` there too).

### Note on interaction with P38.2

P38.1 and P38.2 are independent and can be layered: P38.1 controls whether
`_FREE(_o)` is emitted; P38.2 controls whether the index is boxed.  Implement
P38.1 first (simpler), then P38.2 on top.

---

## P38.2 — Typed Index Fast Path

### Problem

The loop counter `i` lives as a native `int64_t _jsi_i_3`.  The JIT boxes it to
JSValue just before `OP_get_array_el`, which immediately unboxes it.  This is:

```
JS_NewInt32(ctx, i)          → allocates JSValue on stack
JS_VALUE_GET_TAG(_idx)       → tag check (always JS_TAG_INT)
JS_VALUE_GET_INT(_idx)       → int extraction
_FREE(_idx)                  → FreeValue(int) no-op
```

The JIT type-inference system (P8/P9) tracks per-slot types via `_TI_TYPE(d-1)`
(or similar).  If the slot at depth `d-1` is `JIT_T_INT` (a typed integer
slot), the value is available as `_ti{d-1}` (a native `int64_t`), and the full
boxing round-trip can be skipped.

### Change

**`quickjs-jit.c` — `OP_get_array_el` emitter:**

Check the type of the index slot before boxing.  The type-inference pass
(`_TI_TYPE`) runs before the emitter loop.  In the emitter, the typed-slot
state is available via `_jit_slot_type[d-1]` (or whatever the actual variable
name is — read the existing `_P94_ENSURE` implementation to find it).

When the index slot is typed (`JIT_T_INT`):

```c
/* Fast path: index is already a native int64_t _ti{d-1} */
jit_buf_printf(cb,
    "    { JSValue _o=_tsv%d; JSValue _r;\n"
    "      if(js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT)){\n"
    "        char *_op=(char*)JS_VALUE_GET_PTR(_o);\n"
    "        uint32_t _ai=(uint32_t)_ti%d;\n"   /* native int, no unboxing */
    "        if(js_likely(*(uint16_t*)(_op+JIT_OBJ_CLASSID_OFF)==JIT_CLASS_ARRAY\n"
    "                   &&_ai<(uint32_t)*(int*)(_op+JIT_ARR_COUNT_OFF))){\n"
    "          _r=(*(JSValue**)(_op+JIT_ARR_VALUES_OFF))[_ai];\n"
    "          JS_DupValue(ctx,_r);\n"
    "          _FREE(_o); _sp=%d; _tsv%d=_r; _sp=%d;\n"
    "          goto _aok%d;}}\n"
    "      { JSValue _idx=_ti%d<=(int64_t)INT32_MAX&&_ti%d>=(int64_t)INT32_MIN\n"
    "            ?JS_MKVAL(JS_TAG_INT,(int32_t)_ti%d):JS_NewFloat64(ctx,(double)_ti%d);\n"
    "        _r=_RT->get_array_el(ctx,_o,_idx);\n"
    "        _FREE(_o); _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n"
    "      _aok%d:; }\n", ...);
```

Key changes vs current:
- No `_P94_ENSURE(d-1)` (no boxing of the index slot)
- The fast path uses `_ti{d-1}` directly as `uint32_t _ai`
- The tag check `JS_VALUE_GET_TAG(_idx) == JS_TAG_INT` is eliminated
- `_FREE(_idx)` is eliminated (int typed slot has no JSValue to free)
- The slow path still boxes the index for the helper call, but this is rare

When the index slot is NOT typed (generic JSValue `_tsv{d-1}`), fall through to
the existing code path unchanged.

### Identifying the typed slot

The existing `_P94_ENSURE(d)` macro in `quickjs-jit.c` already checks whether
slot `d` is typed:

```c
/* Approximate current implementation */
#define _P94_ENSURE(d) \
    if (_jit_typed_mask & (1u << (d))) { \
        /* emit box for _ti{d} → _tsv{d} */ \
    }
```

(Read the actual implementation; variable names may differ.)

In `OP_get_array_el`, instead of calling `_P94_ENSURE(d-1)` unconditionally,
check `_jit_typed_mask & (1u << (d-1))` first.  If the slot is typed, take the
fast path above.  If not, call `_P94_ENSURE(d-1)` and proceed with the existing
code.

---

## P38.3 — Inline `array.length` Fast Path

### Problem

`OP_get_length` currently emits a full `get_prop` helper call:

```c
JSValue _r = _RT->get_prop(ctx, _tsv0, (JSAtom)50u);
_sp=0; _CHK(_r); _FREE(_tsv0);
_ti0 = (tag==INT) ? (int64_t)val : (int64_t)as_float64;
_FREE(_r); _sp=1;
```

For a dense array, `length` is simply the `u.array.count` field at a fixed
offset.  The guard is identical to the first two checks in the array element
fast path: `JS_TAG_OBJECT` and `class_id == JIT_CLASS_ARRAY`.

### Change

**`quickjs-jit.c` — `OP_get_length` emitter** (currently around line 5058):

Wrap the existing emit in a fast-path guard:

```c
case OP_get_length:
    _P94_ENSURE(d-1);
    jit_buf_printf(cb,
        "    { JSValue _o=_tsv%d;\n"
        "      if(js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT)){\n"
        "        char *_op=(char*)JS_VALUE_GET_PTR(_o);\n"
        "        if(js_likely(*(uint16_t*)(_op+JIT_OBJ_CLASSID_OFF)==JIT_CLASS_ARRAY)){\n"
        "          _ti%d=(int64_t)(uint32_t)*(int*)(_op+JIT_ARR_COUNT_OFF);\n"
        "          _FREE(_o); _sp=%d; goto _lenok%d; }}\n"   /* fast: array */
        "      { JSValue _r=_RT->get_prop(ctx,_o,(JSAtom)%uu);\n"  /* slow: other */
        "        _sp=%d; _CHK(_r); _FREE(_o);\n"
        "        _ti%d=(JS_VALUE_GET_TAG(_r)==JS_TAG_INT)\n"
        "             ?(int64_t)JS_VALUE_GET_INT(_r)\n"
        "             :(int64_t)JS_VALUE_GET_FLOAT64(_r);\n"
        "        _FREE(_r); }\n"
        "      _lenok%d:; _sp=%d; }\n",
        d-1,                      /* _o = _tsv{d-1} */
        d-1,                      /* _ti{d-1} = count */
        d,                        /* _sp after fast path */
        pc,                       /* goto label */
        (unsigned)JS_ATOM_length, /* atom */
        d-1,                      /* _sp before CHK */
        d-1,                      /* _ti{d-1} from slow path */
        pc,                       /* label */
        d);                       /* _sp after slow path */
    break;
```

The result is stored in `_ti{d-1}` (typed int slot) as before, so downstream
opcodes that consume array length as a typed integer (e.g. the `< n` comparison
in `arr_sum`) continue to work without change.

Note: `JS_ATOM_length` is a compile-time constant available in `quickjs-jit.c`.
Verify the actual atom value (50 in the dump above) or use the symbolic constant
if it's accessible.

### String length

The same fast path could be extended to strings:
```c
if (class_id == JS_CLASS_STRING)
    _ti = (int64_t)(uint32_t)((JSString*)(ptr+STROBJ_STR_OFF))->len;
```
But string length access is less performance-critical; leave for a future phase.

---

## P38.4 — Tests, Benchmarks, Doc Update

### Test harness

Create `jit-tests/P38/` with `Makefile` and three test programs:

#### test_p38_1.c — P38.1: refcount integrity for array parameter

- A: `function f(arr) { var s=0; for(var i=0;i<arr.length;i++) s+=arr[i]; return s; }`
  Returns correct sum after JIT compilation.
- B: The `arr` object's refcount has not drifted after the call (same check as
  `test_p37_3.c` B).
- C: Works correctly when array contains non-integer values (slow path exercised).

#### test_p38_2.c — P38.2: typed index correctness

- A: Array indexed by a loop counter (typed int) returns correct values.
- B: Array indexed by a generic JSValue (untyped) still works (fallback path).
- C: Out-of-bounds index falls through to slow path without crashing.

#### test_p38_3.c — P38.3: inline array.length

- A: `arr.length` on a dense array returns the correct count without calling
  `get_prop` (verified by checking the typed result).
- B: `arr.length` on a non-array object falls back to `get_prop` correctly
  (no crash, correct result).
- C: Empty array (`[]`) returns length 0.

### Add P38 to top-level `jit-tests/Makefile`

Add `run-p38` and `clean-p38` targets alongside the existing phase targets.

### Benchmarks

After all code changes:

```sh
rm -f ~/.cache/qjs-jit/*.so ~/.cache/qjs-jit/*.skip
./qjs_nojit jit_perf_tests/bench_runner.js 2>/dev/null
./qjs_jit --jit-aot jit_perf_tests/bench_runner.js 2>/dev/null
./qjs_jit --jit-link jit_perf_tests/bench_runner.js 2>/dev/null  # warm
node jit_perf_tests/bench_runner.js 2>/dev/null
```

Focus on `arr_sum(10000) x1e3` (primary target) and verify no regression on
`prop_read`, `prop_write`, `sum_loop` (exercises `OP_get_loc` peephole).

Update `jit-docs/performance-benchmarks.md` with a P38 Results section.
Update `jit-docs/phase38-array-access.md` to mark sub-phases ✓ DONE.

Actual results (warm AOT, non-LTO build):

| Benchmark | Before P38 | After P38 | Node v24 |
|---|---:|---:|---:|
| `arr_sum(10000) x1e3` | 49 ms | **40 ms** | 9 ms |

Target was < 20 ms (not reached). Primary limit: `arr_sum` benchmark passes `arr` as a
function argument (not a local), so P38.1 borrow does not apply. P38.2 (typed index)
and P38.3 (inline length) contribute ~18% improvement.

---

## Implementation Order

```
P38.1  (refcount elision peephole extension)  — extend existing _top_borrowed logic
P38.2  (typed index fast path)                — conditional in get_array_el emitter
P38.3  (inline array.length)                  — new branch in get_length emitter
P38.4  (tests + bench)                        — test harness + benchmark update
```

P38.1 and P38.3 are fully independent.
P38.2 depends on understanding `_P94_ENSURE` internals but not on P38.1 or P38.3.
All three can be implemented in any order; layer them.

---

## What This Does Not Address

- **Array write amortization**: `put_array_el` with integer values still calls
  `JS_FreeValue(old)` on every write.  When old values are always integers this
  is a no-op branch, but the check still executes.
- **Hoisting class_id check out of loop**: GCC cannot prove `class_id` is loop-
  invariant (it's read via a void* cast).  An explicit hoist would require the
  emitter to track loop structure — out of scope for P38.
- **Typed array elements**: If we observe that all elements are integers (e.g.
  via a fill-time type hint), we could skip `JS_DupValue` on element loads.
  Deferred to a future type-specialization phase.

---

## Expected Outcome

P38.1 should have the largest single impact (same mechanism as P37.3 which gave
~50% improvement on `prop_read`).  P38.2 eliminates `JS_NewInt32` + a tag check
per element — likely 10–20% on top.  P38.3 helps `arr.length` lookups but only
fires once per `arr_sum` call (not per element), so its impact on `arr_sum` is
modest; it primarily benefits functions that call `.length` in a tight loop
(e.g. `while (arr.length > 0) arr.pop()`).

Combined, P38 aims to reduce `arr_sum` from 49 ms to under 20 ms, bringing the
gap to Node from 5.3× to approximately 2×.
