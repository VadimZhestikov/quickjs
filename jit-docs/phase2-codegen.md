# Phase 2 — Bytecode-to-C Code Generator

**Commit:** `fe6d9a0`
**Goal:** Translate QuickJS bytecode to compilable C source.

---

## Overview

The code generator walks the bytecode array of a `JSFunctionBytecode` and emits C source
into a growable buffer.  The resulting C file, when compiled by GCC, produces a function
with the `JSJITFunc` signature that implements the same semantics as the interpreter but
without the dispatch loop.

The approach is purely translational — one C block per bytecode instruction — with a
preliminary scan pass to collect branch targets so that `goto` labels can be emitted at
the right positions.

---

## Data structures

### JSJITCodeBuf

```c
typedef struct {
    char   *buf;    /* malloc'd NUL-terminated C source */
    size_t  len;    /* bytes written (excluding NUL) */
    size_t  cap;    /* allocated capacity */
    int     error;  /* set on OOM; subsequent writes no-op */
} JSJITCodeBuf;
```

Initial capacity: 4096 bytes.  Doubles on overflow.  A typical function generates 2–20 kB
of C source.

### JSJITScanResult

```c
typedef struct {
    int *targets;    /* sorted array of branch-target bytecode offsets */
    int  ntargets;   /* number of entries */
    int  unsupported;/* set if an unsupported opcode was found */
} JSJITScanResult;
```

### JSJITRuntime vtable

```c
typedef struct {
    /* Arithmetic (consume both operands, return new ref) */
    JSValue (*add)(JSContext *, JSValue, JSValue);
    JSValue (*sub)(JSContext *, JSValue, JSValue);
    JSValue (*mul)(JSContext *, JSValue, JSValue);
    JSValue (*div)(JSContext *, JSValue, JSValue);
    JSValue (*mod)(JSContext *, JSValue, JSValue);
    JSValue (*pow)(JSContext *, JSValue, JSValue);
    /* Bitwise */
    JSValue (*shl)(JSContext *, JSValue, JSValue);
    JSValue (*sar)(JSContext *, JSValue, JSValue);
    JSValue (*shr)(JSContext *, JSValue, JSValue);
    JSValue (*band)(JSContext *, JSValue, JSValue);
    JSValue (*bor)(JSContext *, JSValue, JSValue);
    JSValue (*bxor)(JSContext *, JSValue, JSValue);
    /* Unary */
    JSValue (*neg)(JSContext *, JSValue);
    JSValue (*plus)(JSContext *, JSValue);
    JSValue (*bnot)(JSContext *, JSValue);
    JSValue (*type_of)(JSContext *, JSValue);
    /* Comparisons — gt/gte generated as reversed lt/lte */
    JSValue (*lt)(JSContext *, JSValue, JSValue);
    JSValue (*lte)(JSContext *, JSValue, JSValue);
    JSValue (*eq)(JSContext *, JSValue, JSValue);
    JSValue (*strict_eq)(JSContext *, JSValue, JSValue);
    /* Reference counting */
    JSValue (*dup)(JSContext *, JSValue);
    void    (*free)(JSContext *, JSValue);
    /* Property access */
    JSValue (*get_prop)(JSContext *, JSValue obj, JSAtom atom);
    int     (*set_prop)(JSContext *, JSValue obj, JSAtom atom, JSValue val);
    JSValue (*get_array_el)(JSContext *, JSValue obj, JSValue idx);
    int     (*set_array_el)(JSContext *, JSValue obj, JSValue idx, JSValue val);
    /* Calls */
    JSValue (*call)(JSContext *, JSValue func, JSValue this, int argc, JSValue *argv);
    JSValue (*call_constructor)(JSContext *, JSValue ctor, JSValue new_tgt,
                                int argc, JSValue *argv);
    /* Exceptions */
    JSValue (*throw_type_error)(JSContext *, const char *fmt, ...);
    JSValue (*throw_val)(JSContext *, JSValue val);
    /* Closure variable pointer */
    JSValue *(*var_ref_value)(JSVarRef *ref);
} JSJITRuntime;

extern const JSJITRuntime js_jit_rt;   /* singleton, defined in quickjs-jit.c */
```

The generated C references this as `_RT` (a preprocessor define in the preamble).
Since `_RT` is `const`, GCC can devirtualise calls through it at -O2.

---

## Scan pass (scan_body)

Before any C is emitted, a single linear walk collects branch targets and checks for
unsupported opcodes:

```
for each instruction in bytecode:
    if opcode is branch (if_false, if_true, goto, ...):
        compute target offset
        insert into targets[] (sorted, deduplicated)
    if opcode is unsupported:
        set unsupported = 1
        return early
```

**Unsupported opcodes** (set `jit_no_compile`):
- `OP_catch`, `OP_gosub`, `OP_nip_catch` — try/catch/finally require unwind tables
- `OP_with_*` — dynamic scope resolution
- `OP_for_in_*`, `OP_iterator_*` — iterator protocol
- `OP_apply`, `OP_apply_eval` — spread argument lists
- `OP_delete`, `OP_delete_var` — property deletion

Functions containing any of these fall back to interpretation permanently.

The scan is O(n) in bytecode length.  Maximum tracked targets: 4096.  Functions with
more branch targets are also rejected.

---

## Generated C structure

### Preamble (gen_preamble)

```c
#include <stdint.h>
#include "quickjs.h"
#include "quickjs-jit.h"
#define _RT  (&js_jit_rt)
#define _DUP(v)  JS_DupValue(ctx, (v))
#define _FREE(v) JS_FreeValue(ctx, (v))
#define _CHK(v)  do { if (js_unlikely(JS_VALUE_GET_TAG(v) == JS_TAG_EXCEPTION)) \
                          goto _ex; } while(0)
#define _BOOL(v) (JS_VALUE_GET_TAG(v) == JS_TAG_BOOL \
                  ? JS_VALUE_GET_INT(v) : JS_ToBool(ctx, (v)))

/* JS function: <name> */
JSValue __jit_f_<hex_addr>(JSContext *ctx, JSValue this_val,
                            int argc, JSValue *argv,
                            JSValue *cpool, JSVarRef **var_refs)
{
    JSValue _s[<stack_size>];      /* evaluation stack */
    JSValue _l[<var_count>];       /* local variables  */
    int _sp = 0;                   /* stack pointer    */
    /* Phase 5 addition: */
    double _ld[<var_count>];       /* NUMBER-typed locals */

    /* Initialise locals to undefined */
    for (int _i = 0; _i < <var_count>; _i++) _l[_i] = JS_UNDEFINED;
    /* Initialise typed locals to 0.0 */
    for (int _i = 0; _i < <var_count>; _i++)
        if (local_type[_i] == JIT_T_NUMBER) _ld[_i] = 0.0;
```

### Instruction loop structure

Each bytecode instruction becomes a C block:

```c
    /* pc=0 */
    _s[_sp++] = JS_NewInt32(ctx, 0);        /* OP_push_0 */

    /* pc=1 — branch target: label emitted */
_L1:;
    /* pc=1 */
    { JSValue _a = _s[--_sp], _b = _s[--_sp];
      ... }                                  /* OP_add */

    /* pc=2 */
    { JSValue _o = _s[--_sp];
      JSValue _r = _RT->get_prop(ctx, _o, (JSAtom)52u);
      _FREE(_o); _CHK(_r); _s[_sp++] = _r; }  /* OP_get_field */
```

Branch targets get a label `_L<offset>:;` immediately before the instruction.
The scan pass ensures every offset that can be a `goto` target gets a label.

### Exception epilogue

```c
_ex:
    /* Free all live stack slots */
    while (_sp > 0) _FREE(_s[--_sp]);
    /* Free all live locals */
    for (int _i = 0; _i < <var_count>; _i++) _FREE(_l[_i]);
    return JS_EXCEPTION;
}
```

---

## Opcode coverage

The following opcode families are translated directly to C:

| Family | Examples | Notes |
|---|---|---|
| Push constants | `push_i32`, `push_0`–`push_7`, `push_minus1` | inline `JS_NewInt32` |
| Push special | `push_null`, `push_undefined`, `push_true`, `push_false` | inline constants |
| Local get/put | `get_loc`, `put_loc`, `get_loc0`–`get_loc3` | direct `_l[i]` access |
| Argument get/put | `get_arg`, `put_arg`, `get_arg0`–`get_arg3` | `argv[i]` |
| Closure vars | `get_var_ref`, `put_var_ref`, `get_var_ref0`–`get_var_ref3` | via `var_ref_value()` |
| Arithmetic | `add`, `sub`, `mul`, `div`, `mod` | INT fast path + vtable fallback |
| Bitwise | `shl`, `sar`, `shr`, `and`, `or`, `xor` | INT only; vtable for non-int |
| Comparisons | `lt`, `lte`, `gt`, `gte`, `eq`, `neq`, `strict_eq`, `strict_neq` | Phase 6.1 fusion |
| Branches | `if_false`, `if_true`, `goto`, `if_false8`, `if_true8`, `goto8`, `goto16` | `goto _L<n>` |
| Property access | `get_field`, `get_field2`, `put_field` | Phase 6.2 IC |
| Array access | `get_array_el`, `put_array_el` | vtable |
| Calls | `call`, `call0`–`call3`, `call_method`, `tail_call_method` | vtable |
| Return | `return` | free locals + return |
| Inc/dec | `inc_loc`, `dec_loc`, `post_inc`, `post_dec` | Phase 5 fast path |
| Object construction | `object`, `array_from`, `define_field` | vtable |
| Globals | `get_var`, `put_var`, `get_global`, `put_global` | vtable |

---

## Integer fast paths

Several opcodes have inline integer fast paths to avoid vtable calls on the common case:

```c
/* OP_add — example of integer fast path */
{ JSValue _a = _s[_sp-2], _b = _s[_sp-1];
  if (JS_VALUE_GET_TAG(_a) == JS_TAG_INT &&
      JS_VALUE_GET_TAG(_b) == JS_TAG_INT) {
      int64_t _r = (int64_t)JS_VALUE_GET_INT(_a) + JS_VALUE_GET_INT(_b);
      _s[_sp-2] = (_r == (int32_t)_r) ? JS_NewInt32(ctx, (int32_t)_r)
                                       : JS_NewFloat64(ctx, (double)_r);
      _sp--;
  } else {
      JSValue _r = _RT->add(ctx, _a, _b);
      _CHK(_r);
      _s[_sp-2] = _r;
      _sp--;
  } }
```

This keeps integer loops entirely in the fast path without touching the vtable.

---

## Generated C example

For a simple counter loop `for (let i=0; i<100; i++) s += i`:

```c
/* push 0 */  _s[_sp++] = JS_NewInt32(ctx, 0);          /* s = 0 */
/* push 0 */  _s[_sp++] = JS_NewInt32(ctx, 0);          /* i = 0 */
/* put_loc 1*/ { JSValue _v = _s[--_sp]; _FREE(_l[1]); _l[1] = _v; }   /* store i */
/* put_loc 0*/ { JSValue _v = _s[--_sp]; _FREE(_l[0]); _l[0] = _v; }   /* store s */

_L8:;
/* get_loc 1 */  _s[_sp++] = _DUP(_l[1]);               /* push i */
/* push 100 */   _s[_sp++] = JS_NewInt32(ctx, 100);
/* lt + if_false */ (Phase 6.1 fused — see phase6 doc)

/* get_loc 0 */  _s[_sp++] = _DUP(_l[0]);               /* push s */
/* get_loc 1 */  _s[_sp++] = _DUP(_l[1]);               /* push i */
/* add */        { JSValue _a=_s[_sp-2], _b=_s[_sp-1];
                   if(IS_INT(_a) && IS_INT(_b)) { ... }
                   else { JSValue _r=_RT->add(ctx,_a,_b); ... } }
/* put_loc 0 */  { JSValue _v = _s[--_sp]; _FREE(_l[0]); _l[0] = _v; }

/* inc_loc 1 */  { /* Phase 5: if local_type[1]==NUMBER: _ld[1] += 1.0 */ }
/* goto _L8 */   goto _L8;

_L<end>:;
/* get_loc 0 */  _s[_sp++] = _DUP(_l[0]);
/* return */     { JSValue _r = _s[--_sp];
                   while(_sp>0) _FREE(_s[--_sp]);
                   for(int _i=0; _i<var_count; _i++) _FREE(_l[_i]);
                   return _r; }
```

---

## Limitations (as of Phase 2)

- No type information — every local is `JSValue` (addressed in Phase 5)
- No comparison-branch fusion — `lt` + `if_false` are two separate blocks (addressed in Phase 6.1)
- Every property access is a vtable call (addressed in Phase 6.2)
- Try/catch/iterators/spread not supported — function is marked `jit_no_compile`
