# Phase 6 — Comparison+Branch Fusion and Inline Property Cache

Phase 6 consists of two independent optimisations:

- **6.1** (commit `7b198ee`): Fuse comparison opcodes with the immediately following branch
- **6.2** (commit `2bc89d7`): Monomorphic inline property cache for `get_field`/`put_field`

---

## Phase 6.1 — Comparison+Branch Fusion

### Motivation

Without fusion, `a < b; if (!result)` translates to three separate C blocks:

```c
/* OP_lt */
{ JSValue _b=_s[--_sp], _a=_s[--_sp];
  if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT && JS_VALUE_GET_TAG(_b)==JS_TAG_INT)
      _s[_sp++] = JS_NewBool(ctx, JS_VALUE_GET_INT(_a) < JS_VALUE_GET_INT(_b));
  else { JSValue _r=_RT->lt(ctx,_a,_b); _CHK(_r); _s[_sp++]=_r; } }

/* OP_if_false target=L100 */
{ JSValue _v=_s[--_sp];
  int _b = (JS_VALUE_GET_TAG(_v)==JS_TAG_BOOL) ? JS_VALUE_GET_INT(_v)
                                                : JS_ToBool(ctx,_v);
  _FREE(_v);
  if(!_b) goto _L100; }
```

Every loop iteration: two `JSValue` push/pops, `JS_NewBool` allocation, `JS_FreeValue`,
tag check on the bool.  All of this is wasted work — the bool is only used as a branch
condition two instructions later.

Fusion collapses these into one C block with a direct `goto`.

---

### JitFuseInfo

```c
typedef struct {
    int fuse;      /* 1 = fusion is safe */
    int tgt;       /* goto target pc */
    int negate;    /* 1 = if_false (branch on !cond), 0 = if_true */
    int extra_sz;  /* bytes to skip (size of if_false/if_true instruction) */
} JitFuseInfo;
```

### Fusion eligibility check (jit_check_fuse)

```
given: comparison opcode at bytecode offset pc, size sz

next_pc = pc + sz
if next_pc >= bc_len: return {fuse=0}
if scan_is_target(sr, next_pc): return {fuse=0}   ← next instr is a branch target
                                                     (another goto lands here;
                                                      must emit label, can't skip)
nop = bc[next_pc]
if nop ∈ {OP_if_false, OP_if_true, OP_if_false8, OP_if_true8}:
    compute target pc from operand
    set negate = (nop == if_false or if_false8) ? 1 : 0
    set extra_sz = sizeof(nop instruction)
    return {fuse=1, tgt=target, negate=negate, extra_sz=extra_sz}
else:
    return {fuse=0}
```

The `scan_is_target` guard is critical: if another `goto` jumps directly to the
`if_false` instruction, that instruction needs its label.  Fusing would skip the label,
breaking the branch.

When fusion fires, `sz += fi.extra_sz` so the main loop advances past the absorbed
`if_false`/`if_true` instruction.

---

### Three generated-C patterns

#### 1. NUMBER×NUMBER fused (both operands are JIT_T_NUMBER in gen_st)

```c
{ JSValue _va=_s[_sp-2], _vb=_s[_sp-1]; _sp -= 2;
  double _da = (JS_VALUE_GET_TAG(_va)==JS_TAG_INT)
               ? (double)JS_VALUE_GET_INT(_va) : JS_VALUE_GET_FLOAT64(_va);
  double _db = (JS_VALUE_GET_TAG(_vb)==JS_TAG_INT)
               ? (double)JS_VALUE_GET_INT(_vb) : JS_VALUE_GET_FLOAT64(_vb);
  if(/* negate? ! */_da < _db) goto _L100; }
```

- No vtable call, no exception check, no bool allocation
- GCC with typed locals (`_ld[]`): the tag checks also disappear via CSE

#### 2. General fused (integer fast path + vtable fallback)

```c
{ JSValue _a=_s[_sp-2], _b=_s[_sp-1]; _sp-=2; int _cond;
  if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT && JS_VALUE_GET_TAG(_b)==JS_TAG_INT)
      _cond = (JS_VALUE_GET_INT(_a) < JS_VALUE_GET_INT(_b));
  else { JSValue _r=_RT->lt(ctx,_a,_b); _CHK(_r); _cond=JS_VALUE_GET_INT(_r); }
  if(/* negate? ! */_cond) goto _L100; }
```

- Integer case: one tag check, one compare, one branch
- Float/mixed case: vtable + exception check, but still no bool boxing

#### 3. Unfused fallback (produces bool on stack)

Emitted only when `jit_check_fuse` returns `{fuse=0}`.  This is the Phase 2 pattern.

---

### Gen-time type stack (gen_st)

Phase 6.1 adds a shadow type stack maintained during code generation:

```c
int    gen_stk_cap = max(stack_size + 8, 4);
uint8_t *gen_st   = calloc(gen_stk_cap, 1);   /* all JIT_T_JSVAL initially */
int    gen_sp     = 0;

#define _GS_PUSH(t)  do { if(gen_sp<gen_stk_cap) gen_st[gen_sp++]=(t); } while(0)
#define _GS_POP()    (gen_sp>0 ? gen_st[--gen_sp] : JIT_T_JSVAL)
#define _GS_TOP()    (gen_sp>0 ? gen_st[gen_sp-1]  : JIT_T_JSVAL)
#define _GS_TOP2()   (gen_sp>1 ? gen_st[gen_sp-2]  : JIT_T_JSVAL)
#define _GS_DROP(n)  do { gen_sp-=(n); if(gen_sp<0) gen_sp=0; } while(0)
```

At branch merge points (label emission):

```c
if (scan_is_target(sr, pc)) {
    memset(gen_st, JIT_T_JSVAL, gen_stk_cap);  /* conservative reset */
    gen_sp = 0;
    jit_buf_printf(cb, "_L%d:;\n", pc);
}
```

The `gen_st` stack is used by comparisons to select between GEN_CMP_FUSE_NUM and
GEN_CMP_FUSE_GEN:

```c
int _bn = (_GS_TOP2() == JIT_T_NUMBER && _GS_TOP() == JIT_T_NUMBER);
```

---

### Bug: OP_neq / OP_strict_neq fused branch inversion

**Root cause:** The vtable functions `_RT->eq(ctx,a,b)` return 1 when EQUAL.  The INT
fast path `a != b` returns 1 when NOT EQUAL.  These are opposite.

**Previous (buggy) code:**

```c
/* OP_neq: */
else GEN_CMP_FUSE_GEN("!=", "_RT->eq(ctx,_a,_b)", fi.tgt, !fi.negate);
/*                                                          ^^^^^^^^^
   Attempted fix: invert negate to compensate.
   BUT: the INT path (!=) and the vtable path (eq → !=) disagree on what _cond means. */
```

For `if_false` (fi.negate=1) after `neq`:
- INT path: `_cond = (a != b)` → 1 when NOT equal; `if(!_cond)` jumps when equal ✓
- Vtable path: `_cond = JS_VALUE_GET_INT(eq(a,b))` → 1 when equal; `if(!_cond)` jumps when NOT equal ✗

**Fix:**

```c
/* OP_neq: fused general path */
jit_buf_printf(cb,
    "    { JSValue _a=_s[_sp-2],_b=_s[_sp-1]; _sp-=2; int _cond;\n"
    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n"
    "        _cond=(JS_VALUE_GET_INT(_a)!=JS_VALUE_GET_INT(_b));\n"
    "      else{JSValue _r=_RT->eq(ctx,_a,_b);_CHK(_r);"
                "_cond=!JS_VALUE_GET_INT(_r);}\n"  /* ← negate vtable result */
    "      if(%s_cond) goto _L%d; }\n",
    fi.negate ? "!" : "", fi.tgt);
/* Now both paths: _cond=1 when NOT equal; fi.negate controls branch uniformly */
```

**Symptom:** DeltaBlue "Chain test failed." — the equality check in `OrderedCollection`
fired on the wrong path when one operand was an object (vtable path) and the other was
an integer (INT path).

---

## Phase 6.2 — Inline Property Cache

### Motivation

Every `obj.field` access in Phase 5 generates:

```c
{ JSValue _o = _s[--_sp];
  JSValue _r = _RT->get_prop(ctx, _o, (JSAtom)52u);
  _FREE(_o); _CHK(_r); _s[_sp++] = _r; }
```

`_RT->get_prop` calls `JS_GetProperty` which:
1. Checks if `obj` is an `OBJECT` (tag check)
2. Hashes the atom: `h = atom & shape->prop_hash_mask`
3. Reads `prop_hash_end(shape)[-h-1]` (hash chain head)
4. Walks the chain comparing atom at each node
5. Checks property flags (accessor? varref? GETSET?)
6. On success: `JS_DupValue` the result

For a monomorphic call site (same object shape every time), steps 2–5 are wasteful:
the shape never changes, so the slot index could be cached.

---

### Shape system recap

```
JSObject
  shape → JSShape           ← pointer-unique per property layout
  prop  → JSProperty[]      ← property values, indexed by slot

JSShape
  prop_hash_mask             ← hash table size - 1
  [hash table before struct] ← chain heads (uint32, 1-indexed)
  prop[]  → JSShapeProperty  ← .atom + .flags + .hash_next per slot
```

`obj->shape` is the IC key.  If it equals the cached value, the object's property layout
is identical to when the IC was filled — the slot index is valid.

---

### JSJITICEntry

```c
typedef struct {
    void    *shape;   /* JSShape* stored as void* (opaque outside quickjs.c) */
    uint32_t slot;    /* index into JSObject->prop[] */
} JSJITICEntry;   /* 8 bytes on 64-bit */
```

Stored as a function-local `static` in the generated C, one per call site, keyed by
bytecode PC offset (e.g., `_ic47` for the `get_field` at offset 47).

---

### IC helper functions (quickjs.c)

All implementations live in `quickjs.c` (where `JSObject`/`JSShape` internals are
accessible) and are declared in `quickjs-jit.h`.

```c
/* Shape guard: returns non-zero if obj is an OBJECT with the cached shape */
int js_jit_ic_check(JSValue obj, const JSJITICEntry *ic)
{
    if (ic->shape == NULL) return 0;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) return 0;
    return (void *)JS_VALUE_GET_OBJ(obj)->shape == ic->shape;
}

/* Fill IC for a get: only own simple data properties (JS_PROP_NORMAL) */
int js_jit_ic_fill_get(JSContext *ctx, JSValue obj, JSAtom atom, JSJITICEntry *ic)
{
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) return 0;
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    JSProperty *pr;
    JSShapeProperty *prs = find_own_property(&pr, p, atom);
    if (!prs || (prs->flags & JS_PROP_TMASK)) return 0;   /* not cacheable */
    ic->shape = p->shape;
    ic->slot  = (uint32_t)(pr - p->prop);
    return 1;
}

/* Fill IC for a put: writable own simple data properties only */
int js_jit_ic_fill_put(JSContext *ctx, JSValue obj, JSAtom atom, JSJITICEntry *ic)
{
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) return 0;
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    JSProperty *pr;
    JSShapeProperty *prs = find_own_property(&pr, p, atom);
    if (!prs) return 0;
    if ((prs->flags & (JS_PROP_TMASK | JS_PROP_WRITABLE)) != JS_PROP_WRITABLE) return 0;
    ic->shape = p->shape;
    ic->slot  = (uint32_t)(pr - p->prop);
    return 1;
}

/* IC hit: read slot (returns new reference) */
JSValue js_jit_ic_read(JSContext *ctx, JSValue obj, uint32_t slot)
{
    return JS_DupValue(ctx, JS_VALUE_GET_OBJ(obj)->prop[slot].u.value);
}

/* IC hit: write slot (transfers ownership of val) */
int js_jit_ic_write(JSContext *ctx, JSValue obj, JSValue val, uint32_t slot)
{
    set_value(ctx, &JS_VALUE_GET_OBJ(obj)->prop[slot].u.value, val);
    return 0;
}
```

---

### Generated C: OP_get_field with IC

```c
{ static JSJITICEntry _ic<PC> = {NULL, 0};
  JSValue _o = _s[--_sp], _r;
  if (likely(js_jit_ic_check(_o, &_ic<PC>)))
      _r = js_jit_ic_read(ctx, _o, _ic<PC>.slot);
  else {
      _r = _RT->get_prop(ctx, _o, (JSAtom)<atom>u);   /* slow path */
      js_jit_ic_fill_get(ctx, _o, (JSAtom)<atom>u, &_ic<PC>);
  }
  _FREE(_o); _CHK(_r); _s[_sp++] = _r; }
```

### Generated C: OP_put_field with IC

```c
{ static JSJITICEntry _ic<PC> = {NULL, 0};
  JSValue _v = _s[--_sp], _o = _s[--_sp]; int _ret;
  if (likely(js_jit_ic_check(_o, &_ic<PC>)))
      _ret = js_jit_ic_write(ctx, _o, _v, _ic<PC>.slot);
  else {
      _ret = _RT->set_prop(ctx, _o, (JSAtom)<atom>u, _v);
      js_jit_ic_fill_put(ctx, _o, (JSAtom)<atom>u, &_ic<PC>);
  }
  _FREE(_o); if (_ret < 0) goto _ex; }
```

---

### IC operation flow

```
First call to get_field (IC cold):
    js_jit_ic_check({NULL,0})  → miss
    _RT->get_prop(...)         → full hash walk
    js_jit_ic_fill_get(...)    → ic.shape = obj->shape
                                  ic.slot  = slot_index

All subsequent calls (same object shape):
    js_jit_ic_check({shape,slot}) → tag check + pointer compare → hit
    js_jit_ic_read(ctx, obj, slot) → JS_DupValue(obj->prop[slot].u.value)

Object with different shape (polymorphic case):
    js_jit_ic_check → miss (old shape != new shape)
    _RT->get_prop(...)  → full hash walk
    js_jit_ic_fill_get → overwrites IC with new shape
    (IC thrashes on polymorphic sites — monomorphic assumption breaks)
```

---

### What is cached and what is not

| Property type | get cached? | put cached? |
|---|---|---|
| Own simple data property (`JS_PROP_NORMAL`) | yes | yes (if writable) |
| Accessor property (`JS_PROP_GETSET`) | no | no |
| Variable reference (`JS_PROP_VARREF`) | no | no |
| Prototype-inherited property | no | no |
| Non-object value | no | no |

Non-cacheable sites always take the slow path.  The IC entry stays at `{NULL, 0}`.

---

## Combined Phase 6.1 + 6.2 performance

Measurements from `bench_gcc.js` (threshold=2, 8-second GCC warm-up):

```
Benchmark           Interp min    P6.1 min   P6.1 speedup   P6.2 min   P6.2 speedup
────────────────────────────────────────────────────────────────────────────────────
fib(30) ×1          101.08 ms    108.47 ms    0.93×           95.17 ms   1.12×
sum_loop(1e6) ×20   715.02 ms    943.50 ms    0.76×          795.04 ms   1.05×
sum_sq(1e6) ×20     563.95 ms    305.71 ms    1.84×          269.83 ms   2.62×
count_primes ×10      7.73 ms      3.20 ms    2.42×            2.91 ms   2.55×
arr_sum ×1000       328.32 ms    389.30 ms    0.84×          378.98 ms   0.95×
```

### Interpretation

**sum_sq (1.84× → 2.62×):** Phase 6.2 IC fires on any property accesses inside the
`sum_sq` harness.  `bench_gcc.js` wraps the sum_sq loop in an object that the JIT
accesses repeatedly — the IC eliminates those hash-walk overhead calls.

**count_primes (2.42× → 2.55×):** Phase 6.1 comparison fusion fuses the `i % j === 0`
check with the following `if_true`.  Phase 6.2 provides a small additional win.

**fib (0.93× → 1.12×):** Phase 6.2 IC fires on any property reads in the fibonacci
wrapper.  The core recursion is still vtable-limited but wrapper overhead is reduced.

**arr_sum (0.84× → 0.95×):** Still slightly below interpreter.  The inner loop uses
`OP_get_array_el` (vtable, no IC) and `OP_get_length` (correctly handled but still a
function call).  The IC reduces overhead elsewhere in the harness.

---

## Other bugs fixed in this phase

### OP_gt / OP_gte slow-path swap (commit `c71f8f7`)

- **Wrong:** OP_gt float path used `_RT->lte(b,a)` (b ≤ a, inclusive)
- **Wrong:** OP_gte float path used `_RT->lt(b,a)` (b < a, exclusive)
- **Correct:** OP_gt → `lt(b,a)`; OP_gte → `lte(b,a)`
- **Symptom:** Splay benchmark tree corruption — equal float keys sorted wrong, `size()` mismatch

### OP_tail_call_method missing return (commit `c71f8f7`)

- Generated C for `OP_tail_call_method` did not emit `return _r;`
- Function fell through past the tail-call result, returning garbage
- **Symptom:** EarleyBoyer crash

### OP_insert2 missing _DUP (commit `c71f8f7`)

- `OP_insert2` (stack: `a b → a a b`) duplicated a value without incrementing refcount
- **Symptom:** double-free for heap-typed values in `post_inc`/`post_dec` sequences
