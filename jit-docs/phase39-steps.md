# Phase 39 — Detailed Implementation Steps

Reference: `jit-docs/phase39-property-ic-fast.md` (motivation and design).

All line numbers are from the current HEAD after P38 (`e17fc4a`).

---

## Prerequisite reading

Before writing any code, read these sections:

| File | Lines | What |
|---|---|---|
| `quickjs-jit.h` | 494–516 | `JSJITICEntry` struct fields |
| `quickjs-jit.h` | 609–641 | `JIT_IC_CHECK` and `JIT_IC_CHECK_FAST` macros |
| `quickjs-jit.h` | 675–693 | `JSJITICEntry2` struct and bimorphic fill declarations |
| `quickjs.c` | 17220–17278 | `js_jit_ic_fill_get` and `js_jit_ic_fill_put` |
| `quickjs-jit.c` | 2757–2780 | `_borrowed_depth` declaration and per-iteration snapshot |
| `quickjs-jit.c` | 3386–3401 | `GEN_GET_LOC_BORROW` macro |
| `quickjs-jit.c` | 3408–3424 | `_NEXT_IS_GET_FIELD` and `_NEXT2_IS_ARRAY_GET` macros |
| `quickjs-jit.c` | 3492–3546 | `OP_get_loc` and `_GEN_GET_LOC_N` emitters |
| `quickjs-jit.c` | 4914–5035 | `OP_get_field`, `OP_get_field2`, `OP_put_field` emitters |

---

## P39.1 — Remove Redundant `prop_count > slot` Condition

### What and why

`JIT_IC_CHECK_FAST` (line 635) currently ends with:

```c
     *(const uint32_t*)((const char*)(ic)->shape + JIT_SHAPEIC_SHAPEGEN_OFF) == (ic)->shape_gen && \
     (uint32_t)*(const int *)((const char*)(ic)->shape + JIT_SHAPEIC_PROPCOUNT_OFF) > (ic)->slot)
```

The last condition reads `shape->prop_count` from memory and checks `> slot`.
It is redundant: the preceding `shape_gen` match proves the shape is unchanged
since fill time; at fill time `find_own_property` already guaranteed
`slot < prop_count`.  This saves one memory load per IC hit.

### Step 1.1 — `quickjs-jit.h` line 635: trim `JIT_IC_CHECK_FAST`

Remove the final condition (the `prop_count` line):

```c
/* BEFORE (lines 635–641): */
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

### Step 1.2 — `quickjs-jit.h` line 609: trim `JIT_IC_CHECK` identically

Same removal from `JIT_IC_CHECK` (the non-fast variant at line 609):

```c
/* BEFORE (lines 609–617): */
#define JIT_IC_CHECK(obj, ic) \
    ((ic)->rt != NULL && \
     (ic)->rt == JS_GetRuntime(ctx) && \
     (ic)->rt_gen == JS_GetRuntimeICGen((JSRuntime*)(ic)->rt) && \
     (ic)->shape != JIT_IC_MEGAMORPHIC && \
     JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT && \
     *(void **)((char*)JS_VALUE_GET_PTR(obj) + JIT_OBJIC_SHAPE_OFF) == (ic)->shape && \
     *(const uint32_t*)((const char*)(ic)->shape + JIT_SHAPEIC_SHAPEGEN_OFF) == (ic)->shape_gen && \
     (uint32_t)*(const int *)((const char*)(ic)->shape + JIT_SHAPEIC_PROPCOUNT_OFF) > (ic)->slot)

/* AFTER: */
#define JIT_IC_CHECK(obj, ic) \
    ((ic)->rt != NULL && \
     (ic)->rt == JS_GetRuntime(ctx) && \
     (ic)->rt_gen == JS_GetRuntimeICGen((JSRuntime*)(ic)->rt) && \
     (ic)->shape != JIT_IC_MEGAMORPHIC && \
     JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT && \
     *(void **)((char*)JS_VALUE_GET_PTR(obj) + JIT_OBJIC_SHAPE_OFF) == (ic)->shape && \
     *(const uint32_t*)((const char*)(ic)->shape + JIT_SHAPEIC_SHAPEGEN_OFF) == (ic)->shape_gen)
```

### Step 1.3 — `quickjs.c` line 17211: update `js_jit_ic_check` function

The callable C function mirrors the macro.  Remove the redundant check there too:

```c
/* BEFORE: */
    if (ic->slot >= (uint32_t)p->shape->prop_count)
        return 0;
    return get_shape_prop(p->shape)[ic->slot].atom == ic->atom;

/* AFTER: */
    return get_shape_prop(p->shape)[ic->slot].atom == ic->atom;
```

(The atom check already protects against slot being out of range because an
out-of-range slot would either not contain the expected atom or would be
undefined memory — either way causing a miss or safe failure.)

### Step 1.4 — `quickjs-jit.h`: remove unused `JIT_SHAPEIC_PROPCOUNT_OFF`

Grep confirms `JIT_SHAPEIC_PROPCOUNT_OFF` is used only in the two macros just
changed.  Remove the define at line 542:

```c
/* REMOVE this line: */
#define JIT_SHAPEIC_PROPCOUNT_OFF 44
```

Update the comment block above the offset defines (lines 525–541) to remove the
`JSShape.prop_count = byte 44` entry.

### Verification

Build and run all existing JIT tests:

```sh
make CONFIG_JIT=y -j$(nproc) && make -C jit-tests run-all 2>&1 | grep -E 'PASS|FAIL'
```

All tests must pass.  The change removes one memory read per IC hit — no
observable behavior change.

---

## P39.2 — Cache `obj->prop` Array Pointer in IC Entry

### What and why

Every IC fast-path hit currently does:

```c
JSValue *_pp = *(JSValue**)((char*)JS_VALUE_GET_PTR(_o) + JIT_OBJ_PROP_OFF);
JSValue _r   = _pp[_ic.e[0].slot];
```

Two dependent pointer loads: `obj → prop_arr`, then `prop_arr → slot`.
Adding `void *prop_arr` to `JSJITICEntry` and filling it at miss time turns this
into one dependent load: `IC → prop_arr → slot`.  The pointer dereference
`obj → prop_arr` (a separate L1 load cycle on the pointer chain) is eliminated.

`p->prop` is stable as long as the shape is stable: property additions always
create a new shape (bumping `shape_gen`), which fails the IC check and triggers
a refill that updates `prop_arr`.

### Step 2.1 — `quickjs-jit.h` line 494: add `prop_arr` to `JSJITICEntry`

Insert after the `shape` field, before `slot`:

```c
typedef struct {
    void     *shape;    /* JSShape* — opaque outside quickjs.c */
    void     *prop_arr; /* JSObject.prop (JSProperty*) cached at fill time.    ← ADD
                         * Valid while shape_gen matches: property add/remove
                         * always transitions to a new shape (new shape_gen),
                         * invalidating the IC and forcing a refill with updated
                         * prop_arr.  Never read when shape==NULL or MEGAMORPHIC. */
    uint32_t  slot;     /* index into JSObject->prop[] */
    uint32_t  atom;     /* JSAtom at slot — ABA guard */
    uint8_t   kind;     /* 0=general, 1=float64 typed slot (P8.6) */
    uint8_t   _pad[3];
    uint32_t  shape_gen;
    uint32_t  rt_gen;
    void     *rt;
} JSJITICEntry;
```

`JSJITICEntry` grows from 40 to 48 bytes.  `JSJITICEntry2` (two entries + `n`
byte) grows from ~88 to ~104 bytes.  Both are static per-callsite; negligible.

### Step 2.2 — `quickjs.c` line 17238: fill `prop_arr` in `js_jit_ic_fill_get`

After `ic->shape = p->shape;` add the new field:

```c
    ic->shape     = p->shape;
    ic->prop_arr  = p->prop;       /* ← ADD: cache prop array pointer */
    ic->slot      = (uint32_t)(pr - p->prop);
    ic->atom      = prs->atom;
    ic->kind      = (JS_VALUE_GET_TAG(pr->u.value) == JS_TAG_FLOAT64) ? 1 : 0;
    ic->shape_gen = p->shape->shape_gen;
    ic->rt_gen    = ctx->rt->jit_ic_gen;
    ic->rt        = ctx->rt;
```

### Step 2.3 — `quickjs.c` line 17271: fill `prop_arr` in `js_jit_ic_fill_put`

Same addition in `js_jit_ic_fill_put`:

```c
    ic->shape     = p->shape;
    ic->prop_arr  = p->prop;       /* ← ADD */
    ic->slot      = (uint32_t)(pr - p->prop);
    ic->atom      = prs->atom;
    ic->shape_gen = p->shape->shape_gen;
    ic->rt_gen    = ctx->rt->jit_ic_gen;
    ic->rt        = ctx->rt;
```

### Step 2.4 — `quickjs-jit.c`: replace all 8 emitter sites

There are 8 occurrences of the `JIT_OBJ_PROP_OFF` string in `quickjs-jit.c`
(lines 4924, 4928, 4949, 4953, 4983, 4987, 5018, 5022).  Replace every
occurrence of:

```c
"          JSValue *_pp=*(JSValue**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
```

with:

```c
"          JSValue *_pp=(JSValue*)_ic%d.e[N].prop_arr;\n"
```

where `%d` expands to `pc` and `N` is 0 or 1 depending on which IC slot is
being used.

**Important:** The format string already has a `pc` argument for `_ic%d` earlier
in the same `jit_buf_printf` call.  The new `prop_arr` access does NOT add a
new `%d` — it references the same `_ic%d` variable already in scope.  So the
argument list does not change; only the format string changes.

The two affected slots per emitter:
- `e[0]` hit path: `"(JSValue*)_ic%d.e[0].prop_arr"` (pc argument already provided)
- `e[1]` hit path: `"(JSValue*)_ic%d.e[1].prop_arr"` (same pc argument)

**`OP_get_field2`** (lines 4983, 4987) uses `_tsv%d` for the object rather than
`_o`, but the IC variable `_ic%d` is the same.  The replacement pattern is
identical:

```c
/* BEFORE (get_field2 e[0] path): */
"          JSValue *_pp=*(JSValue**)((char*)JS_VALUE_GET_PTR(_tsv%d)+JIT_OBJ_PROP_OFF);\n"

/* AFTER: */
"          JSValue *_pp=(JSValue*)_ic%d.e[0].prop_arr;\n"
```

The `_tsv%d` argument that was passed for the old `JS_VALUE_GET_PTR` call is
now unused in the format string — remove it from the argument list.

Concretely, for `OP_get_field2` (around line 4979):

```c
/* BEFORE format + args (simplified): */
jit_buf_printf(cb,
    "...if(JIT_IC_CHECK_FAST(_tsv%d,&_ic%d.e[0])){\n"
    "    JSValue *_pp=*(JSValue**)((char*)JS_VALUE_GET_PTR(_tsv%d)+JIT_OBJ_PROP_OFF);\n"
    "    _r=_pp[_ic%d.e[0].slot]...\n"
    ...
    d-1, pc,     /* _tsv%d, e[0] check */
    d-1,         /* JS_VALUE_GET_PTR(_tsv%d) ← this arg goes away */
    pc,          /* e[0].slot */
    ...);

/* AFTER format + args: */
jit_buf_printf(cb,
    "...if(JIT_IC_CHECK_FAST(_tsv%d,&_ic%d.e[0])){\n"
    "    JSValue *_pp=(JSValue*)_ic%d.e[0].prop_arr;\n"
    "    _r=_pp[_ic%d.e[0].slot]...\n"
    ...
    d-1, pc,     /* _tsv%d, e[0] check */
    pc,          /* _ic%d.e[0].prop_arr (same pc, no separate arg needed) */
    pc,          /* e[0].slot */
    ...);
```

Wait — `"(JSValue*)_ic%d.e[0].prop_arr"` uses one `%d` (for `pc`).  The
original `"*(JSValue**)((char*)JS_VALUE_GET_PTR(_tsv%d)+JIT_OBJ_PROP_OFF)"`
also uses one `%d` (for `d-1`).  So argument count is the same; only the value
changes: from `d-1` to `pc`.

Verify after editing that the argument position and count in every
`jit_buf_printf` call match the `%d`/`%u` specifiers in the new format string.
Compile with `CONFIG_WERROR=y` to catch format mismatches.

### Step 2.5 — Remove `JIT_OBJ_PROP_OFF` define if now unused

After all 8 replacements, grep for remaining uses of `JIT_OBJ_PROP_OFF`:

```sh
grep -n 'JIT_OBJ_PROP_OFF' quickjs-jit.c quickjs-jit.h quickjs.c
```

If zero results, remove the define from `quickjs-jit.h` (line 539) and update
the surrounding comment block.

### Step 2.6 — Verification

Build and run all tests:

```sh
make CONFIG_JIT=y -j$(nproc) && make -C jit-tests run-all 2>&1 | grep -E 'PASS|FAIL'
```

Additionally, force a cache miss scenario by clearing the JIT cache and running
a property-access JS script to verify the IC fills correctly:

```sh
rm -f ~/.cache/qjs-jit/*.so ~/.cache/qjs-jit/*.skip
echo 'var o={x:1}; function f(o){ var s=0; for(var i=0;i<1000;i++) s+=o.x; return s; } print(f(o));' | ./qjs --jit-link /dev/stdin
```

Expected output: `1000`.

---

## P39.3 — Put-Field Borrow: Skip `_FREE(_o)` for Borrowed Objects

### What and why

`prop_write` benchmark: `o.x = i` inside a loop.

The emitter currently:
1. Loads `o` via `get_loc` with `DupValue` (refcount++).
2. Loads `i` via `get_loc` (typed int — no JSValue refcount op).
3. `put_field` stores `i` into `o.x`, then calls `_FREE(_o)` (refcount--).

The refcount increment and decrement on `o` cancel out for every iteration —
pure overhead.  P37.3 added borrow elision for `get_loc → get_field` (prop_read
pattern).  P39.3 adds the write equivalent: `get_loc [obj] → get_loc [val] →
put_field`.

The existing `_borrowed_depth` machinery already handles this: a plain
`get_loc` that does not match any look-ahead goes through the `else` branch in
`OP_get_loc`, which does **not** reset `_borrowed_depth`.  So a borrow set by
the first `get_loc [obj]` survives through the second `get_loc [val]` (typed)
and is visible to `put_field` via `borrowed_depth_snap`.  We only need:
1. A look-ahead macro that recognises the pattern.
2. The borrow to be set in `get_loc [obj]`.
3. A `borrowed_depth_snap` check in `put_field` to skip `_FREE(_o)`.

### Step 3.1 — `quickjs-jit.c` around line 3420: add `_NEXT2_IS_PUT_FIELD`

Insert after the existing `_NEXT2_IS_ARRAY_GET` macro:

```c
/* P39.3: 2-opcode look-ahead: next is any get_loc variant AND next+sz is put_field.
 * Detects the o.x = val pattern and enables borrow elision for obj on the write side.
 * _IS_GET_LOC_OP is already defined above. */
#define _NEXT2_IS_PUT_FIELD(next_pc) \
    ((next_pc) < bc_len && \
     _IS_GET_LOC_OP(bc[(next_pc)]) && \
     (next_pc) + op_sz[bc[(next_pc)]] < bc_len && \
     bc[(next_pc) + op_sz[bc[(next_pc)]]] == OP_put_field)
```

Also add `#undef _NEXT2_IS_PUT_FIELD` near line 3561, alongside the other
`#undef` lines for the look-ahead macros.

### Step 3.2 — `quickjs-jit.c` around line 3492: add borrow branch in `OP_get_loc`

In the `OP_get_loc` / `OP_get_loc_check` / `OP_get_loc_checkthis` case, add a
third `else if` branch after `_NEXT2_IS_ARRAY_GET`:

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
            } else if (_NEXT2_IS_PUT_FIELD(pc + sz) &&          /* ← ADD */
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

### Step 3.3 — `quickjs-jit.c` around line 3519: same in `OP_get_loc8`

```c
        case OP_get_loc8:
        {
            int _loc8 = (int)bc[pc+1];
            if (_NEXT_IS_GET_FIELD(pc + sz))
                GEN_GET_LOC_BORROW(_loc8);
            else if (_NEXT2_IS_ARRAY_GET(pc + sz) &&
                     !_IS_INT(_loc8) && !_IS_NUM(_loc8) && !_CAP_LOC(_loc8)) {
                jit_buf_printf(cb, "    _tsv%d=_jsv_%s; _sp=%d;\n", d, LNAME(_loc8), d+1);
                _borrowed_depth = d;
            } else if (_NEXT2_IS_PUT_FIELD(pc + sz) &&          /* ← ADD */
                       !_IS_INT(_loc8) && !_IS_NUM(_loc8) && !_CAP_LOC(_loc8)) {
                jit_buf_printf(cb, "    _tsv%d=_jsv_%s; _sp=%d;\n", d, LNAME(_loc8), d+1);
                _borrowed_depth = d;
            } else { GEN_GET_LOC(_loc8); }
            break;
        }
```

### Step 3.4 — `quickjs-jit.c` around line 3534: same in `_GEN_GET_LOC_N` macro

The `_GEN_GET_LOC_N(n)` macro is used for `OP_get_loc0..3`:

```c
#define _GEN_GET_LOC_N(n) do { \
    if (_NEXT_IS_GET_FIELD(pc + sz)) GEN_GET_LOC_BORROW(n); \
    else if (_NEXT2_IS_ARRAY_GET(pc + sz) && \
             !_IS_INT(n) && !_IS_NUM(n) && !_CAP_LOC(n)) { \
        jit_buf_printf(cb, "    _tsv%d=_jsv_%s; _sp=%d;\n", d, LNAME(n), d+1); \
        _borrowed_depth = d; \
    } else if (_NEXT2_IS_PUT_FIELD(pc + sz) &&                  /* ← ADD */ \
               !_IS_INT(n) && !_IS_NUM(n) && !_CAP_LOC(n)) { \
        jit_buf_printf(cb, "    _tsv%d=_jsv_%s; _sp=%d;\n", d, LNAME(n), d+1); \
        _borrowed_depth = d; \
    } else { GEN_GET_LOC(n); } \
} while(0)
```

### Step 3.5 — `quickjs-jit.c` around line 5007: add borrow check to `OP_put_field`

The `put_field` emitter must:
- Emit `_FREE(_o)` when the object was NOT borrowed (`borrowed_depth_snap != d-2`).
- Skip `_FREE(_o)` when the object WAS borrowed (`borrowed_depth_snap == d-2`).
- Always reset `_borrowed_depth = -1` at the end (borrow consumed).

Replace the current single `jit_buf_printf` call in `OP_put_field` with a
conditional:

```c
        case OP_put_field: {
            uint32_t atom = bc_u32(&bc[pc+1]);
            _P94_ENSURE(d-1);
            int _pf_borrowed = (borrowed_depth_snap == d-2);   /* P39.3 */
            if (_pf_borrowed) {
                /* obj was loaded without DupValue — skip _FREE(_o) */
                jit_buf_printf(cb,
                    "    { static JSJITICEntry2 _ic%d={{},0};\n"
                    "      JSValue _v=_tsv%d, _o=_tsv%d; _sp=%d; int _ret;\n"
                    "      if (js_likely(JIT_IC_CHECK_FAST(_o,&_ic%d.e[0]))){\n"
                    "          JSValue *_pp=(JSValue*)_ic%d.e[0].prop_arr;\n"
                    "          JSValue _old=_pp[_ic%d.e[0].slot]; _pp[_ic%d.e[0].slot]=_v;\n"
                    "          JS_FreeValue(ctx,_old); _ret=0;}\n"
                    "      else if (js_likely(_ic%d.n>=2&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[1]))){\n"
                    "          JSValue *_pp=(JSValue*)_ic%d.e[1].prop_arr;\n"
                    "          JSValue _old=_pp[_ic%d.e[1].slot]; _pp[_ic%d.e[1].slot]=_v;\n"
                    "          JS_FreeValue(ctx,_old); _ret=0;}\n"
                    "      else { _ret=_RT->set_prop(ctx,_o,(JSAtom)%uu,_v);\n"
                    "             js_jit_ic2_fill_put(ctx,_o,(JSAtom)%uu,&_ic%d); }\n"
                    "      if(_ret<0) goto _ex; }\n",      /* ← no _FREE(_o) */
                    pc,
                    d-1, d-2, d-2,
                    pc,
                    pc, pc, pc,
                    pc, pc,
                    pc, pc, pc,
                    atom, atom, pc);
            } else {
                /* original code with _FREE(_o) */
                jit_buf_printf(cb,
                    "    { static JSJITICEntry2 _ic%d={{},0};\n"
                    "      JSValue _v=_tsv%d, _o=_tsv%d; _sp=%d; int _ret;\n"
                    "      if (js_likely(JIT_IC_CHECK_FAST(_o,&_ic%d.e[0]))){\n"
                    "          JSValue *_pp=(JSValue*)_ic%d.e[0].prop_arr;\n"
                    "          JSValue _old=_pp[_ic%d.e[0].slot]; _pp[_ic%d.e[0].slot]=_v;\n"
                    "          JS_FreeValue(ctx,_old); _ret=0;}\n"
                    "      else if (js_likely(_ic%d.n>=2&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[1]))){\n"
                    "          JSValue *_pp=(JSValue*)_ic%d.e[1].prop_arr;\n"
                    "          JSValue _old=_pp[_ic%d.e[1].slot]; _pp[_ic%d.e[1].slot]=_v;\n"
                    "          JS_FreeValue(ctx,_old); _ret=0;}\n"
                    "      else { _ret=_RT->set_prop(ctx,_o,(JSAtom)%uu,_v);\n"
                    "             js_jit_ic2_fill_put(ctx,_o,(JSAtom)%uu,&_ic%d); }\n"
                    "      _FREE(_o); if(_ret<0) goto _ex; }\n",  /* ← _FREE(_o) */
                    pc,
                    d-1, d-2, d-2,
                    pc,
                    pc, pc, pc,
                    pc, pc,
                    pc, pc, pc,
                    atom, atom, pc);
            }
            _borrowed_depth = -1; /* P39.3: borrow consumed by put_field */
            break;
        }
```

Note: both branches are identical except for the `_FREE(_o)` line.  The `_pp`
lines already use `prop_arr` from P39.2.  If P39.2 has not been applied yet,
keep `*(JSValue**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF)` in the
non-borrowed branch for now and update both branches together in P39.2.

### Step 3.6 — Verification

Build and run all tests:

```sh
make CONFIG_JIT=y -j$(nproc) && make -C jit-tests run-all 2>&1 | grep -E 'PASS|FAIL'
```

Manually verify `prop_write` result is correct:

```sh
echo 'function prop_write(o,n){for(var i=0;i<n;i++)o.x=i;} var o={x:0}; prop_write(o,1000); print(o.x);' | ./qjs --jit-link /dev/stdin
```

Expected output: `999`.

Also verify refcount integrity (no leak from skipped `_FREE`):

```sh
echo '
var o = {x:0};
var before = __jit_refcount ? __jit_refcount(o) : -1;
function prop_write(o,n){for(var i=0;i<n;i++)o.x=i;}
prop_write(o,1000000);
print(o.x);  // 999999
' | ./qjs --jit-link /dev/stdin
```

If `__jit_refcount` is not available, the correctness of `o.x = 999999` is the
primary check.  Use the ASAN build for leak detection:

```sh
make CONFIG_JIT=y CONFIG_ASAN=y -j$(nproc)
echo 'function pw(o,n){for(var i=0;i<n;i++)o.x=i;} var o={x:0}; pw(o,100000); print(o.x);' \
    | .obj/asan/qjs --jit-link /dev/stdin
```

---

## P39.4 — Create `jit-tests/P39/` Test Suite

### Makefile

Create `jit-tests/P39/Makefile` following the P37/P38 pattern:

```makefile
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -I../.. -I../../.obj
LDFLAGS = -L../.. -lquickjs -lm -ldl -lpthread

TESTS = test_p39_1 test_p39_2 test_p39_3

all: $(TESTS)

%: %.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

run: all
	@for t in $(TESTS); do \
	    echo -n "  $$t: "; \
	    ./$$t && echo "PASS" || echo "FAIL"; \
	done

clean:
	rm -f $(TESTS)
```

### `test_p39_1.c` — IC check correctness (P39.1)

```c
/* Tests: IC check without prop_count condition is still correct.
 * A: basic hit after warm-up returns correct value
 * B: IC miss after shape change (new property) recovers correctly
 * C: megamorphic after 3 shapes — always misses, get_prop fallback */
```

Structure:
- Create a QuickJS runtime + context.
- Compile and run `function f(o) { return o.x; }` via `JS_Eval`.
- Call `f({x:42})` 10 times (warm up IC).
- Assert return value is 42.
- Call `f` with a new object `{x:99, y:0}` (different shape → IC miss → refill).
- Assert return value is 99.
- Call `f` with objects of 3 different shapes in rotation (megamorphic).
- Assert all return correct values.

### `test_p39_2.c` — `prop_arr` caching correctness (P39.2)

```c
/* Tests: cached prop_arr is correct and invalidates on shape change.
 * A: basic property read via prop_arr cache returns correct value
 * B: after adding a property (shape change), IC misses and refills correctly
 * C: bimorphic: two objects with same shape layout but different prop_arr
 *    instances both return correct values from their respective IC entries */
```

Structure:
- A: compile `function f(o){return o.x;}`, warm up with `{x:10}`, assert 10.
- B: compile `function g(o){return o.x;}`, warm up with `{x:5}`.
  Then do `o.newprop = 1` (forces shape transition), call `g(o)` again.
  Assert returns 5 (IC misses on first call after shape change, refills, then hits).
- C: compile `function h(o){return o.x;}`, create `o1={x:1}` and `o2={x:2}`.
  Alternate calls `h(o1)` and `h(o2)` 20 times.  Assert all correct values.
  (Exercises bimorphic IC's two `prop_arr` entries independently.)

### `test_p39_3.c` — put_field borrow refcount integrity (P39.3)

```c
/* Tests: put_field borrow (skipped _FREE) does not corrupt refcount.
 * A: basic prop_write returns correct value
 * B: object refcount is the same before and after the JIT call
 * C: borrow does NOT fire for captured variable (closures) */
```

Structure:
- A: compile `function pw(o,n){for(var i=0;i<n;i++)o.x=i;}`.
  Call `pw(obj, 10000)`. Assert `obj.x == 9999`.
- B: use `JS_VALUE_GET_OBJ(obj)->header.ref_count` to read refcount before and
  after the call.  Assert refcount is equal (no leak from borrow skip).
- C: compile `function make(){var o={x:0}; return function(n){for(var i=0;i<n;i++)o.x=i; return o.x;};}`.
  The `o` here is captured.  Call the returned function and assert correct
  result (verifies captured-variable path uses normal DupValue/FREE, not borrow).

### Step 4.1 — Update `jit-tests/Makefile`

Add P39 targets alongside existing P37, P38:

```makefile
run-p39:
	$(MAKE) -C P39 run

clean-p39:
	$(MAKE) -C P39 clean

run-all: run-p37 run-p38 run-p39

clean-all: clean-p37 clean-p38 clean-p39
```

---

## P39.5 — Benchmark and Doc Update

### Benchmarks

After all code changes, rebuild `qjs_jit` (non-LTO for comparison with P38
baseline, then LTO for the strongest result):

```sh
# Rebuild
make CONFIG_JIT=y -j$(nproc)

# Clear cache
rm -f ~/.cache/qjs-jit/*.so ~/.cache/qjs-jit/*.skip

# Warm AOT run
./qjs_jit --jit-aot jit_perf_tests/bench_runner.js 2>/dev/null
./qjs_jit --jit-aot jit_perf_tests/bench_runner.js 2>/dev/null   # second = warm

# Node reference
node jit_perf_tests/bench_runner.js 2>/dev/null
```

Focus on:
- `prop_read(1e6)`: P39.1 + P39.2 reduce IC check + property load (expect 15–30% improvement)
- `prop_write(1e6)`: same + P39.3 borrow (expect 30–50% improvement)
- All others: should be unaffected (regression check)

### Doc updates

1. Update `jit-docs/phase39-property-ic-fast.md`:
   - Mark each sub-phase ✓ DONE with actual before/after numbers.
   - Add "Actual results" section with the benchmark table.

2. Update `jit-docs/performance-benchmarks.md`:
   - Add a `## 8. P39 Results` section (same format as §6 P37 and §7 P38).
   - Include which sub-phase contributed what.

---

## Implementation Order

```
P39.1  (remove prop_count check)    — 3 edits in .h + 1 in .c
P39.2  (add prop_arr field + fill)  — 1 struct edit + 2 fill edits + 8 emitter edits
P39.3  (put_field borrow)           — 1 macro + 3 look-ahead branches + 1 emitter change
P39.4  (tests P39/*)                — 3 test files + Makefile update
P39.5  (bench + docs)               — benchmark run + 2 doc updates
```

P39.1 is fully independent — do it first (simplest, zero risk).
P39.2 depends on the `JSJITICEntry` struct change; do it second (all 8 emitter
sites must be updated atomically before building).
P39.3 is fully independent from P39.1 and P39.2 — can be done in any order,
but logically after P39.2 so the `put_field` template already uses `prop_arr`.
Tests (P39.4) must come after all code changes are compiled and verified.
