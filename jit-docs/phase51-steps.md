# Phase 51 — Warm-IC Recompile: Speculative INT add for JSVAL+JSVAL OP_add

## Background

P45b–P50 added warm-IC recompile for property reads, array access, put_field,
var_ref, and put_array_el.  P51 extends the same infrastructure to `OP_add`,
covering the **both-JSVAL** code path where both operands have unknown type at
JIT codegen time.

In numeric hot loops where both add operands come from function arguments,
closure vars, or object properties (all JSVAL gen_st at cold compile time), the
cold path must always perform two `JS_VALUE_GET_TAG` calls plus a runtime branch
tree.  With warm INT hints, P51 emits a single speculative `_ti + _ti` add with
no tag checks.

---

## Architecture

### `__jit_vt_HASH[]` array extension

P51 appends `n_ad * 2` entries after the P50 `n_pa` slots:

```
__jit_vt_HASH[0 .. n_gf-1]                                ← get_field (P45b)
__jit_vt_HASH[n_gf .. n_gf+n_ae-1]                        ← get_array_el (P46)
__jit_vt_HASH[n_gf+n_ae .. +n_pf-1]                       ← put_field (P48)
__jit_vt_HASH[n_gf+n_ae+n_pf .. +n_vr-1]                  ← get_var_ref* (P49)
__jit_vt_HASH[n_gf+n_ae+n_pf+n_vr .. +n_pa-1]             ← put_array_el (P50)
__jit_vt_HASH[n_gf+n_ae+n_pf+n_vr+n_pa .. +n_ad*2-1]      ← OP_add operands (P51)
```

Two slots per `OP_add` site: slot `ad_idx*2` holds the observed left operand tag
(`_ta`), slot `ad_idx*2+1` holds the right operand tag (`_tb`).

`n_ad` counts ALL `OP_add` opcodes in the bytecode (including typed ones —
consistent with how `n_vr` counts all `get_var_ref*`).  Typed OP_add sites never
write their hint slots, leaving them at BSS zero (= JS_TAG_INT), which is
harmless since those sites don't read hints.

`n_ad` is stored in `JSFunctionBytecode.jit_n_ad` (uint16_t).

---

## Cold path

When `vt_hints == NULL` (first compile), the both-JSVAL OP_add path emits full
runtime tag checks AND records both operand tags before branching:

```c
{ JSValue _b=_tsv{d-1},_a=_tsv{d-2};
  int _ta=JS_VALUE_GET_TAG(_a),_tb=JS_VALUE_GET_TAG(_b);
  __jit_vt_HASH[base + ad_idx*2]   = (uint8_t)_ta;   /* P51: record left tag */
  __jit_vt_HASH[base + ad_idx*2+1] = (uint8_t)_tb;   /* P51: record right tag */
  if(_ta==JS_TAG_INT&&_tb==JS_TAG_INT){
    int64_t _r64=(int64_t)JS_VALUE_GET_INT(_a)+JS_VALUE_GET_INT(_b);
    _tsv{d-2}=...; _sp={d-1};
  } else if(numeric+numeric) {
    _tsv{d-2}=JS_NewFloat64(ctx,_da+_db); _sp={d-1};
  } else {
    _sp={d-2}; JSValue _r=_RT->add(ctx,_a,_b); _CHK(_r);
    _tsv{d-2}=_r; _sp={d-1};
  }
}
```

---

## Warm path

After `JIT_WARM_THRESHOLD_GCC` JIT calls, `js_jit_schedule_warm_recompile` is
invoked.  It copies `__jit_vt_HASH[]` (now including n_ad*2 slots) to
`b->jit_vt_hints`, then queues a warm recompile.

In the warm gen_body, when both hint slots are 0 (JS_TAG_INT), the speculative
INT path is emitted instead:

```c
/* Warm speculative INT+INT: skip tag checks entirely */
_ti{d-2} = (int64_t)JS_VALUE_GET_INT(_tsv{d-2})
          + (int64_t)JS_VALUE_GET_INT(_tsv{d-1});
_sp = {d-1};
```

No `_FREE` calls are needed — INT JSValues are immediate (no refcount).

The gen_st secondary switch also propagates INT for the result slot when warm
hints say both INT, so downstream ops (sub, mul, comparison, return) see `_ti`
and avoid boxing.

---

## gen_st propagation

The gen_st sub-switch OP_add case:

```c
case OP_add: {
    if (both operands are NUMBER/INT gen_st) → INT or NUMBER (existing logic)
    else if (both JSVAL and warm hints both INT) → INT    /* P51 new */
    else → JSVAL
    ad_idx++;
}
```

`ad_idx` is shared between the main switch (reads for hint indexing) and the
gen_st switch (increments), the same pattern used by `vr_idx`.

---

## Files changed

| File | Change |
|------|--------|
| `quickjs.c` | `jit_n_ad` field in `JSFunctionBytecode`; `js_jit_fb_get/set_n_ad` accessors |
| `quickjs-jit.h` | Accessor declarations; `JIT_CODEGEN_VERSION` bumped to 12 |
| `quickjs-jit.c` | Scan loop counts `n_ad`; `__jit_vt_[]` extended by `n_ad*2`; cold path records tags; warm path emits speculative INT; gen_st switch uses hints; `js_jit_schedule_warm_recompile` reads `n_ad` |
| `jit-tests/js/test_jit_p51_add_warm_int.js` | New JS test: 6 cases covering correct values through warm recompile |
| `jit-tests/js/Makefile` | New test registered |

---

## JIT_CODEGEN_VERSION

Bumped to **12**.  The `__jit_vt_[]` array layout changed (n_ad*2 more slots),
so cached cold `.so` files from JCV 11 must be invalidated.
