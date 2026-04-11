# Phase 38 — Detailed Implementation Steps

Reference: `jit-docs/phase38-array-access.md` (motivation and design).

All line numbers are from the **current HEAD** after P37 (`bca61d8`).

---

## Prerequisite reading

Before writing any code, read these sections in `quickjs-jit.c`:

| Lines | What |
|---|---|
| 2762 | `_top_borrowed` declaration |
| 2769–2770 | per-iteration copy and reset of `_top_borrowed` |
| 3360–3401 | `_IS_INT`, `_IS_NUM`, `_CAP_LOC`, `GEN_GET_LOC`, `GEN_GET_LOC_BORROW` macros |
| 3408–3409 | `_NEXT_IS_GET_FIELD` macro |
| 3478–3484 | `OP_get_loc` emitter (current borrowed look-ahead) |
| 2730–2744 | `_P94_ENSURE` macro |
| 5002–5022 | `OP_get_array_el` emitter |
| 5026–5045 | `OP_put_array_el` emitter |
| 5050–5060 | `OP_get_length` emitter |

---

## P38.1 — Extend refcount-elision peephole to array access

### The problem (precise)

For `s += arr[i]` where `arr` is a function parameter, the emitter produces
per-iteration:

```
get_loc s   → _tsv0 = _DUP(_jsv_s)          (s pushed with DUP)
get_loc arr → _tsv1 = _DUP(argv[0])          (arr pushed with DUP ← expensive)
get_loc i   → _ti2  = _jsi_i  (typed int,    no DUP — already fast)
              _P94_ENSURE(2) boxes _ti2→_tsv2 (index boxing ← addressed in P38.2)
get_array_el→ ...  _FREE(_o) ...              (arr freed ← wasted refcount--)
```

`_DUP(arr)` + `_FREE(arr)` is 1M increments + 1M decrements on the same
object.

The P37.3 peephole works for `get_loc → get_field` (1-opcode look-ahead).
For `arr[i]` the pattern is `get_loc arr → get_loc i → get_array_el`
(2-opcode look-ahead needed).

### Design

**Replace `_top_borrowed` (bool) with `_borrowed_depth` (int).**

`_top_borrowed = 1` means "the top of stack (depth d-1) was pushed without
DupValue."  `_borrowed_depth = N` means "stack slot N was pushed without
DupValue."  This generalises to any depth, not just top-of-stack.

Existing `get_field` consumer checks `top_borrowed` (alias for
`_borrowed_depth == d-1`).  New `get_array_el` consumer checks
`_borrowed_depth == d-2` (the object slot, one below the index).

**Two-opcode look-ahead in `OP_get_loc`:**

```
current pc = P  (get_loc arr, size 3)
next_pc    = P+3
next2_pc   = P+3 + op_sz[bc[P+3]]   (usually P+6 for another get_loc)
```

Borrow `arr` (set `_borrowed_depth = d`) when:
1. `bc[next_pc] == OP_get_loc` AND `bc[next2_pc] == OP_get_array_el`
2. The local variable is not captured (`!_CAP_LOC(idx)`)
3. The local variable is not a typed int/float (`!_IS_INT(idx) && !_IS_NUM(idx)`)
   — typed locals already avoid refcounting via `_ti/tsd` slots; a JSVAL local
   is the one that actually has a refcount to skip

For safety, also verify there is no label/branch target at `next_pc` or
`next2_pc` (to avoid borrowing across control-flow edges).  The existing emitter
has a label-set; check membership.  If any label is present, fall back to normal.

**`_borrowed_depth` lifecycle:**

| Event | Action |
|---|---|
| Top of loop iteration | `int borrowed_depth_snap = _borrowed_depth;` (snapshot); do NOT reset `_borrowed_depth` here |
| `OP_get_loc` with 2-ahead match | `_borrowed_depth = d;` (no DupValue emitted) |
| `OP_get_loc` without match | `_borrowed_depth = -1;` (normal DupValue) |
| `OP_get_array_el` consumes it | `_borrowed_depth = -1;` after emitting the skip |
| Any branch/jump opcode | `_borrowed_depth = -1;` |
| Any label target | `_borrowed_depth = -1;` (before emitting the label) |
| `OP_get_field` consumes it | `_borrowed_depth = -1;` (currently uses `top_borrowed`; migrate to depth check) |

The snapshot `borrowed_depth_snap` is used in the current opcode's emitter.

### Step-by-step code changes

**Step 1.1 — `quickjs-jit.c` line 2762: replace `_top_borrowed`**

```c
/* Before */
int _top_borrowed = 0;

/* After */
int _borrowed_depth = -1;  /* P38.1: stack depth of slot pushed without DupValue; -1 = none */
```

**Step 1.2 — lines 2769–2770: update per-iteration snapshot**

```c
/* Before */
int top_borrowed = _top_borrowed;
_top_borrowed = 0;

/* After */
int top_borrowed = (_borrowed_depth != -1);          /* compat shim for get_field */
int borrowed_depth_snap = _borrowed_depth;           /* full depth for get_array_el */
/* do NOT reset _borrowed_depth here; individual opcodes own its lifecycle */
```

**Step 1.3 — line 3408: extend `_NEXT_IS_GET_FIELD` and add `_NEXT2_IS_ARRAY_GET`**

```c
/* Keep existing (1-opcode): */
#define _NEXT_IS_GET_FIELD(next_pc) \
    ((next_pc) < bc_len && \
     (bc[(next_pc)] == OP_get_field || bc[(next_pc)] == OP_get_field2))

/* New (2-opcode look-ahead for arr[i] pattern): */
#define _NEXT2_IS_ARRAY_GET(next_pc) \
    ((next_pc) < bc_len && \
     (bc[(next_pc)] == OP_get_loc || bc[(next_pc)] == OP_get_loc_check) && \
     (next_pc) + op_sz[bc[(next_pc)]] < bc_len && \
     bc[(next_pc) + op_sz[bc[(next_pc)]]] == OP_get_array_el)
```

`op_sz[]` is the opcode-size table already available in `jit_gen_c_str`.

**Step 1.4 — lines 3478–3484: update `OP_get_loc` look-ahead**

```c
/* Before */
case OP_get_loc: case OP_get_loc_check: case OP_get_loc_checkthis:
    if (_NEXT_IS_GET_FIELD(pc + sz))
        GEN_GET_LOC_BORROW((int)bc_u16(&bc[pc+1]));
    else
        GEN_GET_LOC((int)bc_u16(&bc[pc+1]));
    break;

/* After */
case OP_get_loc: case OP_get_loc_check: case OP_get_loc_checkthis:
{
    int _loc_idx = (int)bc_u16(&bc[pc+1]);
    if (_NEXT_IS_GET_FIELD(pc + sz)) {
        GEN_GET_LOC_BORROW(_loc_idx);
        /* _borrowed_depth updated inside GEN_GET_LOC_BORROW (step 1.5) */
    } else if (_NEXT2_IS_ARRAY_GET(pc + sz) &&
               !_IS_INT(_loc_idx) && !_IS_NUM(_loc_idx) && !_CAP_LOC(_loc_idx)) {
        /* 2-ahead: this get_loc pushes the array obj; next is get_loc idx;
         * after that is get_array_el.  Borrow obj — no DupValue. */
        jit_buf_printf(cb, "    _tsv%d=_jsv_%s; _sp=%d;\n", d, LNAME(_loc_idx), d+1);
        _borrowed_depth = d;
        _TI_PUSH(JIT_T_JSVAL);
    } else {
        GEN_GET_LOC(_loc_idx);
        _borrowed_depth = -1;
    }
    break;
}
```

**Step 1.5 — update `GEN_GET_LOC_BORROW` to use `_borrowed_depth`**

```c
/* Before */
#define GEN_GET_LOC_BORROW(idx) do { \
    if (_IS_INT(idx) || _IS_NUM(idx)) { \
        GEN_GET_LOC(idx); _top_borrowed = 0; \
    } else if (_CAP_LOC(idx)) { \
        jit_buf_printf(cb, "    _tsv%d=_DUP(_cap_buf[%d]); _sp=%d;\n", d, (idx), d+1); \
        _top_borrowed = 0; \
    } else { \
        jit_buf_printf(cb, "    _tsv%d=_jsv_%s; _sp=%d;\n", d, LNAME(idx), d+1); \
        _top_borrowed = 1; \
    } \
} while(0)

/* After */
#define GEN_GET_LOC_BORROW(idx) do { \
    if (_IS_INT(idx) || _IS_NUM(idx)) { \
        GEN_GET_LOC(idx); _borrowed_depth = -1; \
    } else if (_CAP_LOC(idx)) { \
        jit_buf_printf(cb, "    _tsv%d=_DUP(_cap_buf[%d]); _sp=%d;\n", d, (idx), d+1); \
        _borrowed_depth = -1; \
    } else { \
        jit_buf_printf(cb, "    _tsv%d=_jsv_%s; _sp=%d;\n", d, LNAME(idx), d+1); \
        _borrowed_depth = d;   /* was: _top_borrowed = 1 */ \
    } \
} while(0)
```

**Step 1.6 — update `OP_get_field` / `OP_get_field2` to use `borrowed_depth_snap`**

Both already check `top_borrowed`.  With the shim at step 1.2 (`top_borrowed =
(_borrowed_depth != -1)`), the existing check works if the borrowed slot is
always d-1 when get_field runs.  That remains true for the `get_loc → get_field`
pattern.  Also add the explicit reset:

At the end of both the hit path and miss path in `OP_get_field`:
```c
_borrowed_depth = -1;
```

(The shim `top_borrowed` is just a read-only snapshot; resetting `_borrowed_depth`
prevents the flag from leaking to the next opcode.)

**Step 1.7 — update `OP_get_array_el` emitter to use `borrowed_depth_snap`**

The object is at `_tsv{d-2}`.  Check `borrowed_depth_snap == d-2`:

```c
case OP_get_array_el:
    _P94_ENSURE(d-1);  /* index boxing: keep for now; P38.2 replaces this */
    _borrowed_depth = -1;  /* consume borrow */
    if (borrowed_depth_snap == d-2) {
        /* arr was pushed without DupValue — skip _FREE(_o) in both paths */
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
            "          _FREE(_idx); _sp=%d; _tsv%d=_r; _sp=%d;\n"  /* no _FREE(_o) */
            "          goto _aok%d;}}\n"
            "      _r=_RT->get_array_el(ctx,_o,_idx);\n"
            "      _FREE(_idx); _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d;\n" /* no _FREE(_o) */
            "      _aok%d:; }\n",
            d-1, d-2,
            d-2, d-2, d-1, pc,
            d-2, d-2, d-1, pc);
    } else {
        /* existing code with _FREE(_o) and _FREE(_idx) — unchanged */
        jit_buf_printf(cb, ...); /* current emit */
    }
    break;
```

**Step 1.8 — reset `_borrowed_depth` at branch opcodes**

Search for all opcodes that emit `goto` or branch labels in the main emitter
loop (e.g. `OP_if_true`, `OP_if_false`, `OP_goto`, `OP_return`, etc.) and add:

```c
_borrowed_depth = -1;
```

Also reset when emitting any label target (the label-emission code is near
branch handling; find where `_L%d:;` is emitted).

---

## P38.2 — Typed Index Fast Path

### The problem (precise)

`_P94_ENSURE(d-1)` at line 5002 boxes the index slot before `OP_get_array_el`.
When the index is a typed int (`gen_st[d-1] == JIT_T_INT`), this emits:

```c
{ int64_t _tv = _ti{d-1};
  _tsv{d-1} = ((int32_t)_tv == _tv) ? JS_NewInt32(ctx, (int32_t)_tv)
                                     : JS_NewFloat64(ctx, (double)_tv); }
```

Then the array fast path does:
```c
JS_VALUE_GET_TAG(_idx) == JS_TAG_INT   /* tag check — always true */
(uint32_t)JS_VALUE_GET_INT(_idx)       /* int extraction */
_FREE(_idx)                            /* FreeValue(int) — no-op */
```

All four operations are wasteful since `_ti{d-1}` is already a native `int64_t`.

### Design

In `OP_get_array_el`, before calling `_P94_ENSURE(d-1)`, check whether the
index slot is typed:

```c
int _idx_typed = (gen_sp > d-1 && gen_st[d-1] == JIT_T_INT);
```

(`gen_sp` and `gen_st` are already in scope at this point in the emitter.)

If `_idx_typed`, skip `_P94_ENSURE(d-1)` and emit a specialized fast path
using `_ti{d-1}` directly.  If not, call `_P94_ENSURE(d-1)` and use the
existing path.

### Step-by-step code changes

**Step 2.1 — `OP_get_array_el` emitter: detect typed index before boxing**

Replace the current `_P94_ENSURE(d-1)` at line 5002 with:

```c
case OP_get_array_el:
{
    int _idx_typed = (gen_sp > (d-1) && gen_st[d-1] == JIT_T_INT);
    int _obj_borrowed = (borrowed_depth_snap == d-2);   /* from P38.1 */
    _borrowed_depth = -1;

    if (_idx_typed) {
        /* Index is a native int64_t _ti{d-1}; no boxing or tag check needed.
         * Object may or may not be borrowed (P38.1). */
        /* Do NOT call _P94_ENSURE(d-1) — that would box the index we're skipping */
        _P94_ENSURE(d-2);  /* box obj slot if it's somehow typed (unlikely) */
        const char *_o_free = _obj_borrowed ? "" : "_FREE(_o);";
        jit_buf_printf(cb,
            "    { JSValue _o=_tsv%d; JSValue _r;\n"
            "      if(js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT)){\n"
            "        char *_op=(char*)JS_VALUE_GET_PTR(_o);\n"
            "        uint32_t _ai=(uint32_t)_ti%d;\n"       /* direct native int */
            "        if(js_likely(*(uint16_t*)(_op+JIT_OBJ_CLASSID_OFF)==JIT_CLASS_ARRAY\n"
            "                   &&_ai<(uint32_t)*(int*)(_op+JIT_ARR_COUNT_OFF))){\n"
            "          _r=(*(JSValue**)(_op+JIT_ARR_VALUES_OFF))[_ai];\n"
            "          JS_DupValue(ctx,_r);\n"
            "          %s _sp=%d; _tsv%d=_r; _sp=%d; goto _aok%d;}}\n"
            "      { int64_t _iv%d=_ti%d;\n"                /* box for slow path */
            "        JSValue _idx=((int32_t)_iv%d==(int32_t)_iv%d)\n"
            "            ?JS_MKVAL(JS_TAG_INT,(int32_t)_iv%d)\n"
            "            :JS_NewFloat64(ctx,(double)_iv%d);\n"
            "        _r=_RT->get_array_el(ctx,_o,_idx);\n"
            "        %s _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n"
            "      _aok%d:; }\n",
            d-2,                        /* _o = _tsv{d-2} */
            d-1,                        /* _ai = (uint32_t)_ti{d-1} */
            _o_free, d-2, d-2, d-1, pc,  /* fast path: cond free, sp, tsv, sp, goto */
            pc, d-1, pc, pc, pc, pc,    /* slow path: box _ti → _idx */
            _o_free, d-2, d-2, d-1, pc); /* slow path: free, sp, tsv, sp, label */
    } else {
        _P94_ENSURE(d-1);  /* existing box of index */
        if (_obj_borrowed) {
            /* P38.1 borrowed obj path (no _FREE(_o)) */
            jit_buf_printf(cb, ... /* borrowed variant from step 1.7 */ ...);
        } else {
            /* unchanged existing code */
            jit_buf_printf(cb, ... /* current code */ ...);
        }
    }
    break;
}
```

Note: the `_o_free` C string is either `"_FREE(_o);"` or `""` depending on
`_obj_borrowed`. This collapses the P38.1 and P38.2 variations into a 2×2
matrix: (borrowed obj) × (typed idx) → 4 paths emitted via the two flags.

**Step 2.2 — apply the same typed-index optimization to `OP_put_array_el`**

`OP_put_array_el` has the same structure but three operands (obj at d-3, idx at
d-2, val at d-1).  Apply the typed-index check for `d-2`:

```c
int _idx_typed = (gen_sp > (d-2) && gen_st[d-2] == JIT_T_INT);
```

When typed:
- Skip `_P94_ENSURE(d-2)`
- Use `_ti{d-2}` directly as `uint32_t _ai`

Keep `_P94_ENSURE(d-3)` (obj) and `_P94_ENSURE(d-1)` (val) unconditionally.

---

## P38.3 — Inline `array.length` Fast Path

### The problem (precise)

`OP_get_length` at line 5050 emits a full `_RT->get_prop` helper call.  For a
dense array, `u.array.count` (at `JIT_ARR_COUNT_OFF = 64`) is the length.  The
result is always stored in a typed int slot `_ti{d-1}` (the type-inference pass
assigns `JIT_T_INT` to `OP_get_length` results at line 867).

### Design

Wrap the existing emit in an inline fast path that checks
`class_id == JIT_CLASS_ARRAY` and reads the count field directly.  Fall through
to the existing `get_prop` call for non-array objects (strings, typed arrays,
custom `.length` getters).

### Step-by-step code changes

**Step 3.1 — replace the `OP_get_length` emitter body at lines 5050–5060**

```c
case OP_get_length:
    _P94_ENSURE(d-1);  /* box typed obj slot */
    jit_buf_printf(cb,
        "    { JSValue _o=_tsv%d;\n"
        /* fast path: dense array — direct count field read */
        "      if(js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT)){\n"
        "        char *_op=(char*)JS_VALUE_GET_PTR(_o);\n"
        "        if(js_likely(*(uint16_t*)(_op+JIT_OBJ_CLASSID_OFF)==JIT_CLASS_ARRAY)){\n"
        "          _ti%d=(int64_t)(uint32_t)*(int*)(_op+JIT_ARR_COUNT_OFF);\n"
        "          _FREE(_o); _sp=%d; goto _lenok%d; }}\n"
        /* slow path: any other object — call get_prop */
        "      { JSValue _r=_RT->get_prop(ctx,_o,(JSAtom)%uu);\n"
        "        _sp=%d; _CHK(_r); _FREE(_o);\n"
        "        _ti%d=(JS_VALUE_GET_TAG(_r)==JS_TAG_INT)\n"
        "             ?(int64_t)JS_VALUE_GET_INT(_r)\n"
        "             :(int64_t)JS_VALUE_GET_FLOAT64(_r);\n"
        "        _FREE(_r); }\n"
        "      _lenok%d:; _sp=%d; }\n",
        d-1,                          /* _o = _tsv{d-1} */
        d-1,                          /* _ti{d-1} = count (fast) */
        d, pc,                        /* _sp after fast path; goto label */
        (unsigned)JS_ATOM_length,     /* atom for slow path */
        d-1,                          /* _sp before _CHK */
        d-1,                          /* _ti{d-1} = length (slow) */
        pc, d);                       /* label; _sp after slow path */
    break;
```

No change to the surrounding type-inference pass: `OP_get_length` already
produces `JIT_T_INT` at line 867 regardless of this change.

---

## P38.4 — Tests, Benchmarks, Documentation

### Step 4.1 — Create `jit-tests/P38/` directory

Copy the structure from `jit-tests/P37/Makefile` (adjust paths and targets).

```makefile
QJS_DIR := $(realpath ../..)
CFLAGS  := -g -O0 -DCONFIG_JIT -I$(QJS_DIR)
OBJS    := $(QJS_DIR)/.obj/quickjs.o \
            $(QJS_DIR)/.obj/quickjs-jit.o \
            $(QJS_DIR)/.obj/quickjs-libc.o \
            $(QJS_DIR)/.obj/dtoa.o \
            $(QJS_DIR)/.obj/libregexp.o \
            $(QJS_DIR)/.obj/libunicode.o \
            $(QJS_DIR)/.obj/cutils.o
LIBS    := -lm -lpthread -ldl

.PHONY: all run clean
all: /tmp/test_p38_1 /tmp/test_p38_2 /tmp/test_p38_3

/tmp/test_p38_1: test_p38_1.c $(OBJS)
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)
/tmp/test_p38_2: test_p38_2.c $(OBJS)
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)
/tmp/test_p38_3: test_p38_3.c $(OBJS)
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

run: all
	@echo "=== P38.1: array-obj refcount elision ==="
	@/tmp/test_p38_1
	@echo "=== P38.2: typed index fast path ==="
	@/tmp/test_p38_2
	@echo "=== P38.3: inline array.length ==="
	@/tmp/test_p38_3
	@echo "=== ALL P38 TESTS COMPLETE ==="

clean:
	rm -f /tmp/test_p38_1 /tmp/test_p38_2 /tmp/test_p38_3
```

### Step 4.2 — Write `test_p38_1.c` (refcount elision correctness)

Use the C embedding API.  Pattern:

```c
/* A: arr_sum-style function returns correct value after JIT compilation */
const char *src =
    "function arr_sum(arr) {\n"
    "  var s = 0; var n = arr.length;\n"
    "  for (var i = 0; i < n; i++) s += arr[i];\n"
    "  return s;\n"
    "}\n"
    "var _arr = [];\n"
    "for (var k = 0; k < 100; k++) _arr.push(k);\n"
    "for (var k = 0; k < 200; k++) arr_sum(_arr);\n";  /* trigger JIT */
JSValue result = JS_Eval(ctx, "arr_sum(_arr)", ...);
/* expected: 0+1+...+99 = 4950 */
assert(JS_VALUE_GET_INT(result) == 4950);

/* B: object's refcount is not corrupted after the call */
/* Eval a function that returns arr, call arr_sum many times, check arr still works */
JS_Eval(ctx, "arr_sum(_arr); arr_sum(_arr); _arr.length", ...);
/* if refcount was corrupted, use-after-free would crash or return wrong value */

/* C: mixed-type array (some strings) hits slow path without crash */
JS_Eval(ctx, 
    "var _marr = [1, 'x', 2]; function ms(a){var s=0;for(var i=0;i<a.length;i++){var v=a[i];if(typeof v==='number')s+=v;}return s;} for(var k=0;k<200;k++)ms(_marr); ms(_marr)",
    ...);
/* expected: 3 */
```

### Step 4.3 — Write `test_p38_2.c` (typed index correctness)

```c
/* A: array[typed_int_loop_var] returns correct value */
const char *src =
    "function f(arr) {\n"
    "  var s = 0;\n"
    "  for (var i = 0; i < arr.length; i++) s += arr[i];\n"
    "  return s;\n"
    "}\n"
    "var a = [10, 20, 30];\n"
    "for (var k = 0; k < 200; k++) f(a);\n";
/* JIT-compiled f must return 60 */

/* B: array[jsval_idx] (non-typed index) still works */
const char *src2 =
    "function g(arr, idx) { return arr[idx]; }\n"
    "var a = [7, 8, 9]; var idx = 1;\n"
    "for (var k = 0; k < 200; k++) g(a, idx);\n";
/* JIT-compiled g(a, 1) must return 8 */

/* C: out-of-bounds typed index falls through to slow path (no crash) */
const char *src3 =
    "function h(arr) { return arr[999]; }\n"
    "var a = [1, 2, 3];\n"
    "for (var k = 0; k < 200; k++) h(a);\n";
/* h must return undefined, not crash */
```

### Step 4.4 — Write `test_p38_3.c` (inline array.length)

```c
/* A: arr.length on dense array returns correct count */
const char *src =
    "function f(arr) { return arr.length; }\n"
    "var a = [1, 2, 3, 4, 5];\n"
    "for (var k = 0; k < 200; k++) f(a);\n";
/* f(a) must return 5 */

/* B: arr.length after push returns updated count */
const char *src2 =
    "function g(arr) { arr.push(99); return arr.length; }\n"
    "var a = [];\n"
    "for (var k = 0; k < 200; k++) g(a);\n";
/* g(a) must return 201 after 200 calls */

/* C: obj.length on non-array object falls back to get_prop */
const char *src3 =
    "function h(o) { return o.length; }\n"
    "var o = { length: 42 };\n"
    "for (var k = 0; k < 200; k++) h(o);\n";
/* h(o) must return 42 */

/* D: empty array returns length 0 */
const char *src4 =
    "function z(arr) { return arr.length; }\n"
    "var a = [];\n"
    "for (var k = 0; k < 200; k++) z(a);\n";
/* z(a) must return 0 */
```

### Step 4.5 — Add P38 to `jit-tests/Makefile`

Read the current `jit-tests/Makefile` and add:

```makefile
run-p38:
	$(MAKE) -C P38 run

clean-p38:
	$(MAKE) -C P38 clean
```

Add `run-p38` to the `run` target dependencies alongside `run-p37`.

### Step 4.6 — Run benchmarks

```sh
# Rebuild (P38 changes require recompile)
make CONFIG_JIT=y -j$(nproc)

# Clear JIT cache (IC layout may change)
rm -f ~/.cache/qjs-jit/*.so ~/.cache/qjs-jit/*.skip

# Interpreter baseline
./qjs_nojit jit_perf_tests/bench_runner.js 2>/dev/null

# JIT cold (first run, populates cache)
./qjs_jit --jit-link jit_perf_tests/bench_runner.js 2>/dev/null

# JIT warm (second run, uses combined .so)
./qjs_jit --jit-link jit_perf_tests/bench_runner.js 2>/dev/null

# JIT AOT (uses cached .so)
./qjs_jit --jit-aot jit_perf_tests/bench_runner.js 2>/dev/null

# Node reference
node jit_perf_tests/bench_runner.js 2>/dev/null
```

Focus metrics: `arr_sum(10000) x1e3`, `prop_read`, `prop_write`, `sum_loop`
(to verify no regression on P37's get_loc peephole).

### Step 4.7 — Update documentation

**`jit-docs/performance-benchmarks.md`**: Add a "P38 Results" section (§7)
after the P37 section, with the benchmark table and commentary on which
sub-phase contributed what.

**`jit-docs/phase38-array-access.md`**: Mark each sub-phase as ✓ DONE.
Replace the "Target" table with actual measured values.

---

## Build and test sequence

Run after each numbered step to catch errors early:

| After step | Command |
|---|---|
| 1.5 (GEN_GET_LOC_BORROW) | `make CONFIG_JIT=y -j$(nproc) 2>&1 \| grep error` |
| 1.7 (get_array_el borrowed) | `make CONFIG_JIT=y && make -C jit-tests/P1 run && make -C jit-tests/P5 run` |
| 1.8 (branch resets) | `make -C jit-tests run` (full suite, skip P34.5 which is pre-broken) |
| 2.1 (typed idx get) | `make CONFIG_JIT=y && make -C jit-tests run` |
| 2.2 (typed idx put) | `make -C jit-tests run` |
| 3.1 (get_length) | `make CONFIG_JIT=y && make -C jit-tests run` |
| 4.5 (P38 tests added) | `make -C jit-tests/P38 run` |
| 4.6 (benchmarks) | full bench run |

---

## Correctness invariants to verify

1. **Refcount balance**: After any function call on an array parameter, the
   array's refcount must equal its pre-call value.  Test by calling the function
   many times and verifying the array is still usable.

2. **Typed-index out-of-bounds**: When `_ti{idx} >= arr.count` or
   `_ti{idx} < 0`, the fast path must fall through to the slow path, not access
   out-of-bounds memory.  `uint32_t _ai = (uint32_t)_ti{idx}` naturally handles
   negative indices (they wrap to large unsigned values, failing the count check).

3. **`_borrowed_depth` not set across branch targets**: If a branch label
   appears between the `get_loc arr` and `get_array_el`, the borrow must NOT
   activate.  The `_NEXT2_IS_ARRAY_GET` macro only checks bytecode positions;
   label presence at `next_pc` or `next2_pc` must also be checked and cause
   fallback to normal `GEN_GET_LOC`.

4. **`OP_get_length` non-array fallback**: Objects with custom `.length`
   getters (proxy, typed array, string, user object) must use the slow path.
   The class_id check guards this.  Verify with a plain object `{ length: 5 }`.

---

## Expected outcome

| Benchmark | Before P38 | Target after P38 | Node v24 |
|---|---:|---:|---:|
| `arr_sum(10000) x1e3` | 49 ms | < 20 ms | 9 ms |
| `prop_read(1e6)` | 92 ms | ~92 ms (no change) | 9 ms |
| `sum_loop(1e6)` | 27 ms | ~27 ms (no change) | 23 ms |

P38 should not affect any benchmark other than `arr_sum`.  If `prop_read` or
`sum_loop` regress, the `_borrowed_depth` reset logic or `_NEXT2_IS_ARRAY_GET`
look-ahead has a bug — recheck step 1.8.
