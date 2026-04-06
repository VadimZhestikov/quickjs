# Phase 18–24 — Unhandled Finalized Opcodes

## Status

**P18 — DONE** (implemented in `quickjs-jit.c`, tested, no regressions).

**P19 — DONE** (implemented in `quickjs-jit.c`, tested, no regressions).
  - Helpers added in `quickjs.c`: `js_jit_op_get_var_undef`, `js_jit_op_throw_error`,
    `js_jit_op_to_object`, `js_jit_op_to_propkey`, `js_jit_op_regexp`,
    `js_jit_op_set_name_computed`, `js_jit_op_set_proto`, `js_jit_op_set_home_object`,
    `js_jit_op_get_array_el2`, `js_jit_op_define_array_el`, `js_jit_op_push_bigint_i32`,
    `js_jit_op_close_loc`.
  - Function pointers added to `JSJITRuntime` in `quickjs-jit.h`.
  - Codegen cases added in `gen_body()` first and second switches in `quickjs-jit.c`.
  - Note: `get_array_el3` reuses `get_array_el2` vtable entry (DUPs the idx).
  - v8bench score: 886 (pre-P19/P20) → 1042 (post-P19/P20, +17.6%).

**P20 — DONE** (implemented in `quickjs-jit.c`, tested, no regressions).
  - Helpers added in `quickjs.c`: `js_jit_op_make_ref_pair`, `js_jit_op_make_var_ref`,
    `js_jit_op_get_ref_value`, `js_jit_op_put_ref_value`.
  - Function pointers: `make_ref_pair`, `make_var_ref`, `get_ref_value`, `put_ref_value`.
  - `make_var_ref_ref` reuses `make_ref_pair` with `var_refs[idx]` (outer closure ref).
  - `make_loc_ref`/`make_arg_ref` use `js_jit_fb_get_local_var_ref_idx` /
    `js_jit_fb_get_arg_var_ref_idx` at codegen time to map local/arg idx → `_sf_vrefs[]` idx.
  - `close_loc` maps through `js_jit_fb_get_local_var_ref_idx` to get `_sf_vrefs[]` index.

**P21 — DONE** (implemented in `quickjs-jit.c`, tested, no regressions).
  - Helpers added in `quickjs.c`: `js_jit_op_rest`, `js_jit_op_append`,
    `js_jit_op_copy_data_properties`.
  - Forward declarations for `js_append_enumerate` and `JS_CopyDataProperties` added before
    the P21 helpers (both are `static` and defined later in `quickjs.c`).
  - Function pointers: `rest`, `append`, `copy_data_properties`.
  - Key fix: `OP_append` gen_st case must invalidate type tracking on array/pos slots
    (`gen_st[gen_sp-3]` and `gen_st[gen_sp-2]` → `JIT_T_JSVAL`) after the call, since the
    helper writes new JSValues into those slots. Without this, `_P94_ENSURE` re-boxes stale
    `_ti` values (e.g., old pos=0 instead of updated pos=3), corrupting subsequent opcodes.
  - Test: `tests/test_jit_p21p22p23.js`.

**P22 — DONE** (implemented in `quickjs-jit.c`, tested, no regressions).
  - Helpers added in `quickjs.c`: `js_jit_op_private_symbol`, `js_jit_op_get_private_field`,
    `js_jit_op_put_private_field`, `js_jit_op_define_private_field`, `js_jit_op_private_in`.
  - Stack order for `put_private_field`: obj=sp[-3], val=sp[-2], prop=sp[-1]
    (prop and val in unusual order vs. typical patterns).
  - `private_in` wraps `js_operator_private_in` which does NOT free op1/op2 on error;
    the wrapper frees them explicitly on the error path.
  - Function pointers: `private_symbol`, `get_private_field`, `put_private_field`,
    `define_private_field`, `private_in`.

**P23 — DONE** (implemented in `quickjs-jit.c`, tested, no regressions).
  - Deferred opcodes (added to `scan_is_unsupported`): `check_ctor` (needs `new_target`),
    `init_ctor` (needs `new_target` + `func_obj`), `define_class`/`define_class_computed`
    (need `JSStackFrame *sf` for closure creation). Functions containing these opcodes are
    excluded from JIT compilation.
  - Implemented opcodes: `check_ctor_return` (inlined using `JS_IsObject`/`JS_IsUndefined`),
    `check_brand`, `add_brand`, `get_super` (inlined using `JS_GetPrototype`),
    `get_super_value`, `put_super_value`, `define_method`, `define_method_computed`.
  - Helpers added in `quickjs.c`: `js_jit_op_check_brand`, `js_jit_op_add_brand`,
    `js_jit_op_get_super_value`, `js_jit_op_put_super_value`, `js_jit_op_define_method`,
    `js_jit_op_define_method_computed`.
  - Function pointers: `check_brand`, `add_brand`, `get_super_value`, `put_super_value`,
    `define_method`, `define_method_computed`.
  - Tests: `tests/test_jit_p21p22p23.js` covers all implemented opcodes.

**P25 — DONE** (for-in and generic iterators — verified regression test, already implemented).
  - `OP_for_in_start`, `OP_for_in_next`, `OP_iterator_next`, `OP_iterator_call` were
    all implemented in the P15 phase. Regression tests added in `tests/test_jit_p25p26p27p28.js`.

**P26 — DONE** (constructor / class-definition — implemented without signature change).
  - Key insight: `new_target` and `func_obj` are accessible via
    `ctx->rt->current_stack_frame` (set by `JS_CallInternal` before invoking the JIT
    function). `var_refs` is already a JIT function parameter. No signature extension needed.
  - Removed `check_ctor`, `init_ctor`, `define_class`, `define_class_computed` from
    `scan_is_unsupported` — classes are now fully JIT-compilable.
  - Helpers added in `quickjs.c`: `js_jit_op_check_ctor`, `js_jit_op_init_ctor`,
    `js_jit_op_define_class`, `js_jit_op_define_class_computed`.
  - Forward declarations added for static `js_op_define_class` and `js_dynamic_import`.
  - `define_class` error-path: `js_op_define_class` frees the inputs (parent_class, bfunc)
    on failure and sets `sp[-2]=sp[-1]=JS_UNDEFINED`. The JIT codegen sets `_sp=d-2`
    before the call, so the exception handler does not double-free consumed slots.
  - `define_class_computed`: key slot (`sp[-3]`) is read-only; parent and bfunc are consumed.
    On failure only parent/bfunc are freed by `js_op_define_class`; key stays valid.
  - Function pointers: `check_ctor`, `init_ctor`, `define_class`, `define_class_computed`.
  - Tests: `tests/test_jit_p25p26p27p28.js`.

**P27 — DONE** (dynamic import).
  - `OP_import`: stack effect 2-in 1-out (specifier, options → promise).
  - Helper `js_jit_op_import` wraps `js_dynamic_import(ctx, specifier, options)` which
    does NOT consume its inputs; wrapper frees both after the call.
  - Forward declaration added for static `js_dynamic_import`.
  - Function pointer: `import_op`.
  - Tests: `tests/test_jit_p25p26p27p28.js`.

**P28 — DONE** (OP_eval excluded from JIT).
  - `OP_eval` added to `scan_is_unsupported`. Functions containing direct `eval()` are
    excluded from JIT compilation and run interpreted.
  - Rationale: `OP_eval` needs the full scope chain (`scope_idx + ARG_SCOPE_END`) which
    is not available in the JIT function signature without major refactoring.
  - Tests: `tests/test_jit_p25p26p27p28.js` verifies that eval-using functions still
    run correctly (in interpreter mode).

## Overview

57 finalized opcodes (listed in `quickjs-opcode.h` as `DEF(...)`) currently reach the
`default:` case in `gen_body()` and cause JIT compilation to bail.  They are distinct from
the 9 opcodes explicitly rejected by `scan_is_unsupported()` (covered in phase13-todo.md).

The table below lists every group, the estimated effort, and the assigned phase.

```
Phase  Group                         Opcodes                              Effort
───────────────────────────────────────────────────────────────────────────────────
P18    Trivial type-tests            is_null, is_undefined, …             ~1 day
P18    Stack shuffle extras          dup3, nip1, insert3/4, perm3/4/5,   ~1 day
                                     rot4l/5l, swap2
P19    Simple utility ops            close_loc, get_var_undef,            ~1 day
                                     throw_error, to_object, to_propkey,
                                     regexp, set_name_computed,
                                     set_proto, set_home_object
P19    Array element helpers         get_array_el2, get_array_el3,        ~0.5 day
                                     define_array_el, push_bigint_i32
P20    Ref-slot ops (lvalue refs)    get_ref_value, put_ref_value,        ~2 days
                                     make_loc_ref, make_arg_ref,
                                     make_var_ref, make_var_ref_ref
P21    Spread / rest / copy          append, copy_data_properties, rest   ~1 day
P22    Private fields                private_symbol, get_private_field,   ~2 days
                                     put_private_field,
                                     define_private_field, private_in
P23    Class / OOP machinery         define_class, define_class_computed, ~3 days
                                     define_method, define_method_computed,
                                     check_brand, add_brand,
                                     check_ctor, check_ctor_return,
                                     init_ctor, get_super,
                                     get_super_value, put_super_value
P24    Dynamic eval / import         eval, import                         deferred
───────────────────────────────────────────────────────────────────────────────────
```

`OP_invalid` must never appear in valid bytecode; no action needed.

---

## Phase 18 — Trivial Type-Tests and Stack Shuffles (~1 day)

### Trivial type-test opcodes

All six emit one `JSValue` → `int` test.  The pattern is identical to existing
`OP_typeof_*` cases.

| Opcode                         | Interpreter equivalent                               |
|--------------------------------|------------------------------------------------------|
| `OP_is_null`                   | `JS_VALUE_GET_TAG(sp[-1]) == JS_TAG_NULL`            |
| `OP_is_undefined`              | `JS_VALUE_GET_TAG(sp[-1]) == JS_TAG_UNDEFINED`       |
| `OP_is_undefined_or_null`      | tag is NULL or UNDEFINED                             |
| `OP_typeof_is_undefined`       | **(already handled — verify)**                       |
| `OP_typeof_is_function`        | **(already handled — verify)**                       |

Generated C pattern for `OP_is_null` (consumes top of stack, pushes bool int):

```c
case OP_is_null:
    _ti0 = (JS_VALUE_GET_TAG(_tsv0) == JS_TAG_NULL);
    _FREE(_tsv0);
    _tsv0 = JS_NewBool(ctx, (int)_ti0);
    break;
```

`OP_is_undefined_or_null`:

```c
{ int _tag = JS_VALUE_GET_TAG(_tsv0);
  _ti0 = (_tag == JS_TAG_NULL || _tag == JS_TAG_UNDEFINED);
  _FREE(_tsv0);
  _tsv0 = JS_NewBool(ctx, (int)_ti0); }
```

### Stack-shuffle extras

These mirror existing `OP_dup`, `OP_nip`, `OP_insert2`, etc. patterns already in the
codegen.  They only move `JSValue` slots; no heap allocation, no _CHK.

**`OP_dup3`** — duplicate top-3 slots (push copies of sp[-3], sp[-2], sp[-1]):

```
_tsv3 = _DUP(_tsv0); _tsv4 = _DUP(_tsv1); _tsv5 = _DUP(_tsv2);
// then shift _sp accordingly
```

The generated C uses the local `_tsv*` spill array.  Follow the pattern in `OP_dup2`.

**`OP_nip1`** — remove sp[-2] (keep sp[-1], sp[-3], etc.):

```c
_FREE(_tsv1); _tsv1 = _tsv0; _sp--;  // renumber
```

**`OP_insert3`** / **`OP_insert4`** — insert a copy of sp[-1] at depth 3 or 4:

```c
// insert3: push sp[-1] below sp[-3]
_tsv3 = _DUP(_tsv0);
// shift existing slots up one
```

**`OP_perm3`** / **`OP_perm4`** / **`OP_perm5`** — cyclic permutation.  Each can be
expressed as a handful of local variable swaps.  Follow the interpreter exactly.

**`OP_rot4l`** / **`OP_rot5l`** — rotate left: sp[-4..sp-1] → sp[-3..sp-1], sp[-4].

**`OP_swap2`** — swap sp[-1]↔sp[-3] and sp[-2]↔sp[-4] (double-slot swap).

Implementation note: the codegen tracks the logical stack depth in `g->sp`.  These ops
must update `g->sp` correctly even if they don't change the depth.

---

## Phase 19 — Simple Utility Ops (~1.5 days)

### `OP_close_loc` — close an upvalue slot

Interpreter:
```c
close_lexical_var(ctx, sf, get_u16(pc)-1, 1);
```

Generated C:
```c
case OP_close_loc:
    _RT->close_loc(ctx, sf, (int)(uint16_t)IMM16 - 1);
    break;
```

`close_loc` must be a new runtime helper (thin wrapper around `close_lexical_var`).

### `OP_get_var_undef` — like `OP_get_var` but returns `undefined` if not found

Generated C: same as `OP_get_var` but pass `0` for the `throw_ref_error` argument to
`js_get_variable`:

```c
case OP_get_var_undef: {
    JSAtom _a = (JSAtom)IMM32;
    JSValue _r = _RT->get_var_undef(ctx, _a);
    _CHK(_r); PUSH(_r);
    break; }
```

### `OP_throw_error` — throw a fixed error string (e.g. temporal dead zone)

Interpreter:
```c
JS_ThrowError(ctx, get_u8(pc), js_get_opcode_str(pc+1));
```

Generated C:
```c
case OP_throw_error:
    _RT->throw_error(ctx, (int)IMM8, (const char *)IMM_STR);
    goto _ex;
```

The `IMM_STR` is a pointer into the bytecode stream; in C IR the codegen writes the
address as a `(const char*)` cast of the embedded literal.

### `OP_to_object` — `ToObject(sp[-1])`

Interpreter calls `js_toObject` which returns a new heap value.

```c
case OP_to_object: {
    JSValue _r = JS_ToObject(ctx, _tsv0);
    _FREE(_tsv0); _CHK(_r); _tsv0 = _r;
    break; }
```

### `OP_to_propkey` / `OP_to_propkey2`

`to_propkey`: `js_to_property_key(ctx, sp[-1])` (in-place, may throw).
`to_propkey2`: same but also leaves original below it.

```c
case OP_to_propkey: {
    JSValue _r = _RT->to_propkey(ctx, _tsv0);
    _FREE(_tsv0); _CHK(_r); _tsv0 = _r;
    break; }
```

### `OP_regexp` — create a RegExp object from `(pattern, flags)` on stack

```c
case OP_regexp: {
    JSValue _r = js_jit_regexp(ctx, _tsv1, _tsv0);
    _FREE(_tsv1); _FREE(_tsv0); _sp -= 2;
    _CHK(_r); PUSH(_r);
    break; }
```

`js_jit_regexp` is a thin wrapper around `js_regexp_constructor_internal`.

### `OP_set_name_computed` — set the `name` property of a function from TOS

```c
case OP_set_name_computed:
    _RT->set_function_name(ctx, _tsv0, _tsv1);
    _FREE(_tsv1); _sp--;
    break;
```

### `OP_set_proto` — `Object.setPrototypeOf(obj, proto)`

```c
case OP_set_proto:
    if (JS_SetPrototypeInternal(ctx, _tsv1, _tsv0, TRUE) < 0) goto _ex;
    _FREE(_tsv0); _sp--;
    break;
```

### `OP_set_home_object` — record home object for `super` in methods

```c
case OP_set_home_object:
    js_method_set_home_object(ctx, _tsv0, _tsv1);
    break;  // does not consume stack
```

### Array element helpers

**`OP_get_array_el2`** — like `OP_get_array_el` but does not consume the object:

```c
case OP_get_array_el2: {
    JSValue _r = JS_GetPropertyValue(ctx, _tsv1, _DUP(_tsv0));
    _FREE(_tsv0); _sp--;  // index consumed; obj stays
    _CHK(_r); PUSH(_r);
    break; }
```

**`OP_get_array_el3`** — 3-arg variant for destructuring; see interpreter.

**`OP_define_array_el`** — `JS_DefinePropertyValueUint32` for array literal with spread.

**`OP_push_bigint_i32`** — push `BigInt(i32_literal)`:

```c
case OP_push_bigint_i32: {
    JSValue _r = JS_NewBigInt64(ctx, (int64_t)(int32_t)IMM32);
    PUSH(_r);
    break; }
```

---

## Phase 20 — Reference Slots (`make_*_ref`, `get_ref_value`, `put_ref_value`) (~2 days)

These opcodes implement the "reference" type used by `delete`, `++`, `--`, and
`for-of` lvalue patterns.  The interpreter represents a reference as two consecutive
stack slots: `(base_obj_or_flag, property_atom_or_index)`.

### Opcodes

| Opcode            | Description                                       |
|-------------------|---------------------------------------------------|
| `OP_make_loc_ref`  | push ref to local var (slot index + sentinel)    |
| `OP_make_arg_ref`  | push ref to argument slot                        |
| `OP_make_var_ref`  | push ref to captured/global var                  |
| `OP_make_var_ref_ref` | push ref to a var-ref slot (upvalue)          |
| `OP_get_ref_value` | load value from TOS reference pair              |
| `OP_put_ref_value` | store value into TOS reference pair             |

### Strategy

In the JIT's C IR, represent the two-slot reference as two adjacent `_tsv*` locals
(just as in the interpreter's stack).  The `make_*_ref` ops push them; `get_ref_value`
and `put_ref_value` call the existing runtime helpers `JS_GetReference` /
`JS_SetReference` (or inline the logic for the common `loc` / `arg` cases).

```c
case OP_make_loc_ref: {
    /* push (JS_UNDEFINED with tag=JS_TAG_CATCHOFFSET encoded as var idx, atom) */
    uint32_t _idx = (uint32_t)IMM16;
    JSAtom   _a   = (JSAtom)IMM32_2;
    _tsv_N   = JS_NewInt32(ctx, (int32_t)_idx);   // sentinel/index slot
    _tsv_N1  = JS_AtomToValue(ctx, _a);
    _sp += 2;
    break; }

case OP_get_ref_value: {
    JSValue _r = _RT->get_ref_value(ctx, _tsv1, _tsv0);
    /* do NOT free the ref pair — get_ref_value borrows them */
    _CHK(_r); PUSH(_r);
    break; }

case OP_put_ref_value: {
    JSValue _val = _tsv0; _sp--;
    int _rc = _RT->put_ref_value(ctx, _tsv1, _tsv0, _val);
    _FREE(_tsv1); _FREE(_tsv0); _sp -= 2;
    if (_rc < 0) goto _ex;
    break; }
```

The `make_var_ref_ref` case must reach into the `var_refs` array; the JIT already has
`var_refs` as a parameter so this is straightforward.

---

## Phase 21 — Spread / Rest / Copy (~1 day)

These three opcodes finalize array/object spread patterns.  Phases 16–17 already
handled `OP_delete` and `OP_apply`; these are the remaining spread helpers.

### `OP_append` — append iterable to array accumulator

Interpreter: `js_append_enumerate(ctx, sp)` on `(array, iterator_value)` pair.

```c
case OP_append: {
    int _rc = js_append_enumerate(ctx, &_tsv0);  /* sp-1 = array */
    if (_rc < 0) goto _ex;
    if (_rc) { _FREE(_tsv0); _sp--; }  /* consumed if done */
    break; }
```

Signature: `js_append_enumerate(ctx, JSValue *sp)` — already exported in `quickjs.c`.

### `OP_copy_data_properties` — `Object.assign`-style copy with exclusion mask

Interpreter: `js_copy_data_properties(ctx, sp[-1-mask_bits], sp[-1], exclusion_list)`.

```c
case OP_copy_data_properties: {
    uint8_t _mask = (uint8_t)IMM8;
    /* stack layout depends on mask; see interpreter for exact indexing */
    int _rc = _RT->copy_data_properties(ctx, _tsv_dest, _tsv_src, _mask);
    if (_rc < 0) goto _ex;
    break; }
```

### `OP_rest` — collect remaining args into an array

Used in `function f(a, b, ...rest)`.

```c
case OP_rest: {
    uint32_t _first = (uint32_t)IMM16;
    JSValue _r = _RT->build_rest(ctx, argc, argv, _first);
    _CHK(_r); PUSH(_r);
    break; }
```

`build_rest` allocates a new Array and copies `argv[_first..argc-1]` into it.

---

## Phase 22 — Private Fields (~2 days)

Private class fields are implemented via `JSPrivateField` objects keyed by a
`OP_private_symbol`-created symbol.  All five opcodes call existing internal
functions; the JIT wraps them in runtime helpers.

| Opcode                  | Internal function                             |
|-------------------------|-----------------------------------------------|
| `OP_private_symbol`     | `js_new_private_symbol(ctx, atom)`            |
| `OP_get_private_field`  | `JS_GetPrivateField(ctx, obj, sym)`           |
| `OP_put_private_field`  | `JS_SetPrivateField(ctx, obj, sym, val)`      |
| `OP_define_private_field` | `JS_DefinePrivateField(ctx, obj, sym, val)` |
| `OP_private_in`         | `JS_PrivateFieldIn(ctx, sym, obj)`            |

All are straightforward C-call wrappers in generated code; no special codegen needed.

```c
case OP_get_private_field: {
    JSValue _r = JS_GetPrivateField(ctx, _tsv1, _tsv0);
    _FREE(_tsv1); _FREE(_tsv0); _sp -= 2;
    _CHK(_r); PUSH(_r);
    break; }

case OP_private_in: {
    int _b = JS_PrivateFieldIn(ctx, _tsv1, _tsv0);
    _FREE(_tsv1); _FREE(_tsv0); _sp -= 2;
    if (_b < 0) goto _ex;
    PUSH(JS_NewBool(ctx, _b));
    break; }
```

`OP_private_symbol` uses the `cpool` entry (an atom index), same pattern as
`OP_push_atom_value`.

---

## Phase 23 — Class / OOP Machinery (~3 days)

This phase is the most complex.  The 12 opcodes in this group interact with the
class instantiation machinery, prototype chains, and `super`.

### Subgroups

**Class definition** (`OP_define_class`, `OP_define_class_computed`):

These pop `(proto, parent_proto)` from the stack, call `js_define_class_internal`, and
push `(class_obj, proto)`.  The interpreter uses `JS_NewObjectProtoClass` and a
multi-step setup sequence.  Generate a runtime helper `js_jit_define_class(ctx, ...)`.

**Method definition** (`OP_define_method`, `OP_define_method_computed`):

```c
case OP_define_method: {
    JSAtom _a = (JSAtom)IMM32;
    int _flags = (int)IMM8;
    int _rc = js_define_method(ctx, _tsv1, _a, _tsv0, _flags);
    _FREE(_tsv0); _sp--;
    if (_rc < 0) goto _ex;
    break; }
```

**Brand checks** (`OP_check_brand`, `OP_add_brand`):

Used for private methods.  `check_brand` throws if `this` doesn't have the brand;
`add_brand` installs it.

```c
case OP_check_brand:
    if (JS_CheckBrand(ctx, _tsv0, _tsv1) < 0) goto _ex;
    break;

case OP_add_brand:
    if (JS_AddBrand(ctx, _tsv0, _tsv1) < 0) goto _ex;
    _FREE(_tsv0); _sp--;
    break;
```

**Constructor helpers** (`OP_check_ctor`, `OP_check_ctor_return`, `OP_init_ctor`):

Used inside `constructor()` bodies.  `check_ctor` throws if not called as `new`.
`init_ctor` calls `js_init_ctor_object` which sets `this` for the derived class.

```c
case OP_check_ctor:
    if (!js_is_constructor(ctx, this_val)) {
        JS_ThrowTypeError(ctx, "..."); goto _ex; }
    break;
```

**`super` access** (`OP_get_super`, `OP_get_super_value`, `OP_put_super_value`):

`OP_get_super` pushes `Object.getPrototypeOf(home_obj)`.

`OP_get_super_value` / `OP_put_super_value` are property get/put on the super prototype
with `this` as receiver.

```c
case OP_get_super: {
    JSValue _r = JS_GetPrototype(ctx, _tsv0);
    _FREE(_tsv0); _CHK(_r); _tsv0 = _r;
    break; }

case OP_get_super_value: {
    /* stack: (this, home_obj, prop) */
    JSValue _proto = JS_GetPrototype(ctx, _tsv1);
    _FREE(_tsv1); _CHK(_proto);
    JSValue _r = JS_GetPropertyValue(ctx, _proto, _tsv0);
    _FREE(_proto); _FREE(_tsv0); _sp -= 2;
    _CHK(_r); PUSH(_r);
    break; }
```

Implementation order within P23:
1. `check_ctor` / `check_ctor_return` / `init_ctor` (standalone, no dependencies)
2. `define_method` / `define_method_computed` (needed before class definition)
3. `check_brand` / `add_brand`
4. `get_super` / `get_super_value` / `put_super_value`
5. `define_class` / `define_class_computed` (depends on all of the above)

---

## Phase 24 — Dynamic `eval` and `import` (Deferred)

### `OP_eval`

`eval()` called at runtime may introduce new bindings in the enclosing scope.
QuickJS handles this by re-entering the interpreter.  A JIT-compiled function that
reaches `OP_eval` should bail out (deoptimize) rather than attempt to inline the
evaluation.  Two strategies:

1. **Scan-time rejection**: Add `OP_eval` to `scan_is_unsupported()` so functions
   containing `eval` are never JIT-compiled.  Simple, conservative.

2. **Runtime deopt**: Emit a call to `_RT->eval(ctx, ...)` and trust that the eval
   result only writes to the `JSGlobalObject` (not local vars).  Only safe when `eval`
   does not appear in a `with` or use `arguments`.

Recommendation: strategy 1.  `eval`-heavy code is never a JIT target anyway.

### `OP_import`

Dynamic `import()` returns a Promise.  It requires the module loader and the
Promise machinery.  Unlike `eval`, it does not invalidate local bindings; the JIT
can emit a direct call to `js_dynamic_import(ctx, specifier)`.

```c
case OP_import: {
    JSValue _r = JS_DynamicImport(ctx, _tsv0);
    _FREE(_tsv0); _CHK(_r); _tsv0 = _r;
    break; }
```

`JS_DynamicImport` already exists in `quickjs.c`.  This is a safe, self-contained
wrapper call — implement in P24 alongside or after P23.

---

## Dependency Graph

```
P18 (type-tests, stack)   ← no deps — start immediately
P19 (utility, arrays)     ← no deps — start in parallel with P18
P20 (ref slots)           ← needs P18 (type-tests for ref guard)
P21 (spread/rest)         ← needs P17 (apply/spread already done)
P22 (private fields)      ← needs P19 (symbol push pattern)
P23 (class/OOP)           ← needs P22 (brand uses private symbols)
P24 (eval/import)         ← independent; eval likely stays rejected
```

---

## Eligibility Scan Updates

Each phase must also remove the corresponding opcodes from any eligibility guards in
`js_jit_is_eligible()` or `scan_is_unsupported()` once they are implemented, so that
functions using these opcodes become eligible for JIT compilation.

Currently `scan_is_unsupported()` rejects only the 9 opcodes listed in phase13-todo.md.
The 57 opcodes in this document are not rejected by the scan — they simply cause a
codegen bail.  After implementation each phase's opcodes require **no scan change**;
they will automatically be handled by the new `case` arms.

---

## Implementation Note — gen_st Update Bug for Shuffle Ops

When a shuffle opcode (insert3/4, perm3/4/5, rot4l/5l, swap2, nip1) rearranges stack
slots, the `gen_st[]` type-tracking array in `gen_body` must be updated to reflect the
new slot positions.  The standard `_gs_drop`/`_gs_push` mechanism only adds/removes
entries from the *top*; it does not update types for existing slots.

If `gen_st[slot] == JIT_T_INT` and that slot's JSValue is moved (via shuffle) to a
different slot `gen_st[new_slot]`, the next `_P94_ENSURE(new_slot)` would generate:

```c
_tsv{new_slot} = box(_ti{new_slot});   // WRONG: _ti{new_slot} is stale!
```

overwriting the shuffled JSValue without freeing it → **refcount leak**.

**Fix**: for all P18 shuffle ops, use direct `gen_st[i] = JIT_T_JSVAL` updates in the
second switch of `gen_body` (not `_gs_push`/`_gs_drop`).  Because `_P94_ENSURE` boxes
all INT/NUMBER slots to `_tsv` *before* the shuffle code runs, after the shuffle every
involved slot holds a JSValue; marking them all `JIT_T_JSVAL` is correct.

The same issue exists in principle for the existing `OP_swap` / `OP_rot3l` / `OP_rot3r`
cases, but in practice those ops appear in JSVAL-only contexts (after property reads /
function calls) so `gen_st` is already JSVAL at those slots.  P18 shuffle ops appear in
compound-assignment patterns where integer push_i32 appears immediately before the
shuffle, triggering the bug.

## Completion Criteria

A phase is complete when:
1. All opcodes in the phase have `case` arms in `gen_body()`.
2. `./qjs tests/test_builtin.js` and `./qjs tests/test_std.js` pass.
3. `./qjs jit_perf_tests/v8bench/test_eb_only.js` passes with `--jit-warmup`.
4. No new ASAN/UBSAN errors from `make CONFIG_ASAN=y`.
5. `./qjs --jit-warmup jit_perf_tests/v8bench/run_qjs.js` runs without assertion failures.
