# Phase 45 — IC Integer-Typed Property Values

## Background

### Current property-access gap

After P43, warm-cache V8 benchmark results:

| Benchmark | JIT warm | Node v24 | Gap |
|---|---|---|---|
| prop_read(1e6) | ~85 ms | ~9 ms | **9.4×** |
| prop_write(1e6) | ~72 ms | ~12 ms | **6×** |
| Richards | 1268 | 28175 | 22× |
| DeltaBlue | 789 | 63268 | 80× |

The IC fast path (P37–P39) eliminated shape-change overhead and pointer chases.
The remaining cost is **value boxing/unboxing**: property values are always read
as `JSValue` (16-byte struct on 64-bit) even when the property is observed to
always hold a small integer.

### What P44 doesn't fix

P44 (mixed-type arithmetic) helps when the consumer of the property value is an
arithmetic op.  But the property read itself still returns `JIT_T_JSVAL`, so
`gen_st` downstream sees JSVAL, and subsequent operations box/unbox accordingly.

### The fix: IC value-tag observation

At IC fill time we already know the tag of the value being stored.  Recording
`JS_VALUE_GET_TAG(value)` in `JSJITICEntry` costs one byte per IC slot.  In the
generated C getter, when the IC hits AND the stored value-tag matches `JS_TAG_INT`,
we can read the property as a native `int64_t` and set `gen_st = JIT_T_INT`.

This allows the downstream arithmetic fast paths (P8.6, P44) to fire where they
previously couldn't, eliminating one `JS_VALUE_GET_TAG` + one conditional branch
per property-access-to-arithmetic chain.

---

## Architectural overview

### `JSJITICEntry` extension

```c
/* Current (P43): */
typedef struct {
    uintptr_t  shape;      /* cached shape pointer */
    uint32_t   shape_gen;  /* shape->shape_gen at fill time */
    uint16_t   slot;       /* property slot index */
    uint16_t   class_id;   /* JS_CLASS_OBJECT or class hint */
    void      *prop_arr;   /* P39.2: cached properties array pointer */
    JSRuntime *rt;         /* ABA guard: runtime that filled this entry */
} JSJITICEntry;

/* P45 addition: */
typedef struct {
    ...existing fields...
    uint8_t    val_tag;    /* P45: JS_VALUE_GET_TAG(value) at fill time,
                            *      JS_TAG_UNDEFINED = unknown/mixed */
} JSJITICEntry;
```

`val_tag = JS_TAG_UNDEFINED` (0) means "not yet observed" or "polymorphic type".
Any value tag other than JS_TAG_UNDEFINED means "always seen this tag so far".

### IC fill change

In `js_jit_ic_fill_get_field` (and `js_jit_ic_fill_get_field2`):

```c
/* After recording shape/slot/prop_arr: */
ic->val_tag = (uint8_t)JS_VALUE_GET_TAG(val);
```

On **IC eviction** (slot 0 replaced by a new shape):

```c
/* When promoting to bimorphic or evicting: */
if (ic->e[0].val_tag != new_entry.val_tag)
    ic->e[0].val_tag = JS_TAG_UNDEFINED;  /* mixed type observed */
```

On **megamorphic demotion** (>4 shapes → IC disabled):
- IC is already cleared; `val_tag` reset to 0 (JS_TAG_UNDEFINED) automatically.

### Generated C change

Current generated get_field fast path (simplified):

```c
if (JIT_IC_CHECK_FAST(_o, &_ic.e[0])) {
    JSValue _r = _ic.e[0].prop_arr[_ic.e[0].slot];
    JS_DupValue(ctx, _r);
    _tsv{d-1} = _r;   /* gen_st: JIT_T_JSVAL */
}
```

P45 addition: when `ic->val_tag == JS_TAG_INT` at code-gen time (i.e. the IC
entry observed an INT value), the emitter outputs an additional typed branch:

```c
if (JIT_IC_CHECK_FAST(_o, &_ic.e[0])) {
    JSValue _r = _ic.e[0].prop_arr[_ic.e[0].slot];
    /* P45: INT fast path — no DupValue needed (tag=INT → refcount-free) */
    if (js_likely(_ic.e[0].val_tag == JS_TAG_INT &&
                  JS_VALUE_GET_TAG(_r) == JS_TAG_INT)) {
        _ti{d-1} = JS_VALUE_GET_INT(_r);
        /* gen_st result: JIT_T_INT — no _tsv fill needed */
    } else {
        JS_DupValue(ctx, _r);
        _tsv{d-1} = _r;
    }
}
```

This is generated at code-gen time by checking the IC entry's `val_tag`.  The
`_ic.e[0].val_tag` field is a **runtime** check (the IC could be updated after
compilation); the gen_st output is set to `JIT_T_INT` **only** when the code-gen
time snapshot of `val_tag` is `JS_TAG_INT`.

> **Important:** `val_tag` being JS_TAG_INT at code-gen time is a hint, not a
> guarantee.  The generated code MUST still do the runtime `JS_VALUE_GET_TAG`
> check and fall to the `_tsv` path on mismatch.  This preserves correctness
> when the property later gets assigned a float or object.

---

## Sub-phases

### P45.1 — Struct extension + cache invalidation ✅ Done

**File:** `quickjs-jit.h`

`val_tag` consumes one byte from the existing `_pad[3]` array (now `_pad[2]`),
so `sizeof(JSJITICEntry)` is **unchanged** (no alignment change, no field offset
shift for `shape_gen`/`rt_gen`/`rt`).

```c
uint8_t   kind;     /* 0=general, 1=float64 typed slot (P8.6) */
uint8_t   val_tag;  /* JS_VALUE_GET_TAG(value) at IC fill time; 0=unknown/mixed (P45) */
uint8_t   _pad[2];  /* explicit padding (was _pad[3] before P45) */
```

**Cache invalidation:** `JIT_CODEGEN_VERSION` bumped from 4 to 5 in `quickjs-jit.h`.
Even though the struct size did not change, the generated code logic changed
(DupValue elision in IC hit paths), requiring all cached `.so` files to be rebuilt.

History comment updated:
```
5=P45 val_tag in JSJITICEntry for INT fast path on get_field
```

---

### P45.2 — IC fill: record val_tag ✅ Done

**File:** `quickjs.c` — `js_jit_ic_fill_get`

Added one line after setting `kind`:

```c
ic->kind      = (JS_VALUE_GET_TAG(pr->u.value) == JS_TAG_FLOAT64) ? 1 : 0;
ic->val_tag   = (uint8_t)JS_VALUE_GET_TAG(pr->u.value); /* P45: record value tag */
```

`js_jit_ic2_fill_get` (quadrimorphic IC fill) delegates to `js_jit_ic_fill_get`
for each slot, so `val_tag` is filled transitively.

**Eviction / megamorphic:** When `ic->shape = JIT_IC_MEGAMORPHIC` (5+ shapes),
the IC is effectively disabled (`JIT_IC_CHECK_FAST` always misses).  The `val_tag`
value in a megamorphic entry is never read.  No special handling needed.

**put_field:** Not extended — `put_field` doesn't return a value to the stack,
so the type hint serves no purpose there.  Deferred.

---

### P45.3 — Code emitter: typed get_field result ✅ Done (groundwork)

**File:** `quickjs-jit.c`, `case OP_get_field:` emitter.

**What was implemented:** The IC hit paths (all four IC slots in both
`top_borrowed` and non-`top_borrowed` variants) now emit a runtime INT check
that skips `JS_DupValue` for INT-tagged properties:

```c
#define _GF_DUP_OR_SKIP \
    "if(js_likely(JS_VALUE_GET_TAG(_r)!=JS_TAG_INT)) JS_DupValue(ctx,_r);\n"
```

**What was NOT implemented:** The full `gen_st=JIT_T_INT` optimization.

**Why:** Setting `gen_st=JIT_T_INT` for `OP_get_field` requires knowing `val_tag`
at **codegen time** — but static IC entries (`static JSJITICEntry2 _icN={{},0}`)
are zero-initialized in BSS and only filled at **runtime** (first JIT call).  At
codegen time, `val_tag==0` (= `JS_TAG_INT`=0 on this platform, but that's the
uninitialized state, not a real observation).

Setting `gen_st=INT` without a guarantee that `_ti{d-1}` always holds the correct
int64 would be an unsoundness bug (non-INT properties would leave `_ti` uninitialized
while downstream code reads it).

**Future P45b:** A warm-IC recompile pass (execute function once in JIT to fill ICs,
then recompile with val_tag known) would enable the full `gen_st=INT` optimization.
The `val_tag` field is now recorded at every IC fill, ready for that pass.

**gen_st tracking** stays `JIT_T_JSVAL` for `OP_get_field` (line ~7904 in `quickjs-jit.c`).

**Benefit of current implementation:** For INT-typed properties, the generated IC
hit path omits the `JS_DupValue` call.  Since `JS_DupValue` for INT is already a
near-no-op (one branch check that's always false for `tag>=0`), the runtime savings
are minimal — the primary value of P45 is the **`val_tag` infrastructure** for P45b.

---

### P45.4 — `jit_infer_types` update for OP_get_field ✅ Done (no change needed)

`jit_infer_types` continues to push `JIT_T_JSVAL` for `OP_get_field` (Option B).
Since P45.3 did not implement the `gen_st=INT` promotion, `jit_infer_types` is
also left unchanged.  Both passes are consistent: `OP_get_field` → JSVAL.

---

### P45.5 — Performance measurement ✅ Done

See `jit_perf_tests/RESULTS_P45.md` for detailed measurements.

**Key finding:** No measurable V8 benchmark improvement from P45 alone.  As analyzed
in P45.3 notes, the `gen_st=INT` propagation (which would give real speedups on
downstream arithmetic) was not implemented.  The DupValue elision for INT is already
near-free in GCC-optimized code (inlined away by the compiler).

P45's value is infrastructure: `val_tag` is now recorded at every IC fill, ready for
the P45b warm-IC recompile optimization.

---

### P45.6 — Correctness tests ✅ Done

**File:** `jit-tests/js/test_jit_p45_ic_int.js` (created)

Tests: INT property access, float64 property (must not use INT path), property
type change mid-run (INT → float), polymorphic IC (two shapes), multi-property
INT arithmetic, negative INT properties, zero, IC limit check.

All tests pass with `./qjs --jit-threshold-gcc=1 jit-tests/js/test_jit_p45_ic_int.js`.

---

### P45.7 — Documentation ✅ Done

- `jit-docs/phase45-steps.md` (this file): all sub-phases marked done with
  implementation notes explaining the gen_st=INT limitation.
- `jit_perf_tests/RESULTS_P45.md`: created with benchmark results and analysis.
- `jit-docs/performance-benchmarks.md`: P45 section added.

---

## Interaction with P44

P44 and P45 are synergistic:

- P44 alone: `arr[i]` (JSVAL) + `s` (NUMBER) → half-typed add, ~one tag check
  per iteration eliminated.
- P45 alone: `obj.x` (INT via IC) → `gen_st = JIT_T_INT` downstream, `obj.x +
  obj.y` uses the `_bn` fully-typed path.
- P44 + P45: `obj.x` (INT) + `arr[i]` (JSVAL) → still half-typed (P44), but
  the INT side skips boxing entirely.

**Recommended order:** Implement P44 first (purely additive codegen change, no
IC struct changes, no cache invalidation), then P45.

---

## Complexity Assessment

**Medium.** The struct change + IC fill recording (P45.1–P45.2) are mechanical.
The emitter change (P45.3) is the tricky part: the generated C must handle
three cases (IC miss / IC hit+INT / IC hit+non-INT) correctly for every IC
site.

Risk areas:
- **Stale `val_tag` after type change:** The runtime tag check in the generated
  C handles this — if `val_tag` says INT but the property is now FLOAT64, we
  fall through to the JSValue path and DupValue correctly.  No unsoundness.
- **`val_tag` coherence across IC eviction:** When slot 0 is evicted to make
  room for a new shape, the new entry's `val_tag` must be cleared to UNDEFINED
  if the new value's tag differs.  Failing this produces stale INT hints and
  silent wrong results.
- **Cache version bump:** Forgetting this means old `.so` files read `val_tag`
  from the wrong struct offset (it will be garbage).  Always bump
  `JIT_CACHE_VERSION` when `JSJITICEntry` changes.

---

## What is NOT in scope for P45

- `put_field` IC value type (write-side INT optimization) — deferred.
- `get_array_el` IC element type (array element type observation) — separate
  phase (P46 candidate).
- `jit_infer_types` IC-aware inference (Option A above) — P45b follow-on.
- Native IC machine-code stubs — architectural change, separate multi-phase effort.
