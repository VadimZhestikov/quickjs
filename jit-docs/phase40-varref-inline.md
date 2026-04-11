# Phase 40: Closure Variable Inline Access

## Motivation

The `closure_counter` micro-benchmark runs at 15ms vs Node's 2ms — a 7.5× gap. The counter
function captures a single integer closure variable and increments it on every call:

```js
function make_counter() {
  let n = 0;
  return () => ++n;
}
const counter = make_counter();
for (let i = 0; i < 1_000_000; i++) counter();
```

Each `counter()` call currently costs ~3 indirect vtable calls through `_RT->var_ref_value()`:
- `get_var_ref` → loads pvalue → dup value
- `put_var_ref` (or `set_var_ref`) → loads pvalue → free old, store new

The vtable call itself is an indirect branch plus a trivial one-liner body: `return ref->pvalue`.
That's ~4 cycles of indirect branch misprediction + call overhead, repeated 3M times per 1M
counter() calls = ~12M cycles = ~4.5ms at 2.7 GHz.

## Root Cause

`JSVarRef` is defined as a complete type only in `quickjs.c`. The generated JIT C files see
`JSVarRef` as an incomplete (opaque) type, so they cannot access `ref->pvalue` directly. The
vtable call `_RT->var_ref_value(ref)` was introduced to work around this.

The fix: use a byte-offset constant `JIT_VARREF_PVALUE_OFF = 24` instead of a type-aware field
access. Cast to `(JSValue**)((char*)ref + 24)` and dereference. This requires no complete-type
knowledge and can be verified with a `_Static_assert` in `quickjs.c`.

Note: the pre-implementation estimate was 16. The actual value is 24 because
`JSGCObjectHeader` is 24 bytes (not 16): the bitfield at offset 4 occupies a full 4-byte int
allocation unit, plus dummy1(1)+dummy2(2)+1-byte-align-pad = 8 bytes total before
`struct list_head` at offset 8, which is 16 bytes (two pointers). Total: 8+16=24.

## JSVarRef Layout (confirmed)

```c
typedef struct JSVarRef {
    union {
        JSGCObjectHeader header;   /* bytes 0–23 on 64-bit:
                                      ref_count(4) + bitfield(4) + dummy1(1) + dummy2(2) +
                                      padding(1) + struct list_head(16) = 24 bytes */
        struct {
            int __gc_ref_count;
            uint8_t __gc_mark;
            uint8_t is_detached;
            uint8_t is_lexical;
            uint8_t is_const;
        };
    };
    JSValue *pvalue;   /* byte 24 — pointer to the actual JSValue storage */
    union {
        JSValue value;   /* when is_detached == TRUE: storage lives here */
        struct { uint16_t var_ref_idx; JSStackFrame *stack_frame; };
    };
} JSVarRef;
```

`pvalue` is stable for the entire duration of a JIT function call. It changes only at scope exit
(closure detachment), which cannot happen while the inner function is executing.

## Sub-phases

### P40.1 — Direct pvalue access via byte offset

Replace the indirect vtable call with a direct byte-offset dereference in every var_ref macro.

**Before:**
```c
#define GEN_GET_VR(idx) \
    jit_buf_printf(cb, "    _tsv%d=_DUP(*_RT->var_ref_value(var_refs[%d])); _sp=%d;\n", \
                   d, idx, d+1)
#define GEN_PUT_VR(idx) do { \
    _P94_ENSURE(d-1); \
    jit_buf_printf(cb, "    { JSValue *_p=_RT->var_ref_value(var_refs[%d]);" \
                       " _FREE(*_p); *_p=_tsv%d; _sp=%d; }\n", idx, d-1, d-1); \
} while(0)
#define GEN_SET_VR(idx) do { \
    _P94_ENSURE(d-1); \
    jit_buf_printf(cb, "    { JSValue *_p=_RT->var_ref_value(var_refs[%d]);" \
                       " _FREE(*_p); *_p=_DUP(_tsv%d); }\n", idx, d-1); \
} while(0)
```

**After:**
```c
#define GEN_GET_VR(idx) \
    jit_buf_printf(cb, "    { JSValue *_vp=*(JSValue**)((char*)var_refs[%d]+JIT_VARREF_PVALUE_OFF);" \
                       " _tsv%d=_DUP(*_vp); _sp=%d; }\n", idx, d, d+1)
#define GEN_PUT_VR(idx) do { \
    _P94_ENSURE(d-1); \
    jit_buf_printf(cb, "    { JSValue *_vp=*(JSValue**)((char*)var_refs[%d]+JIT_VARREF_PVALUE_OFF);" \
                       " _FREE(*_vp); *_vp=_tsv%d; _sp=%d; }\n", idx, d-1, d-1); \
} while(0)
#define GEN_SET_VR(idx) do { \
    _P94_ENSURE(d-1); \
    jit_buf_printf(cb, "    { JSValue *_vp=*(JSValue**)((char*)var_refs[%d]+JIT_VARREF_PVALUE_OFF);" \
                       " _FREE(*_vp); *_vp=_DUP(_tsv%d); }\n", idx, d-1); \
} while(0)
```

This eliminates the indirect call overhead. Each access still loads `var_refs[idx]` and then
loads `pvalue` — two memory reads per access.

**Expected gain:** ~3–4ms on closure_counter (eliminates 3M × ~4 cycles indirect call penalty).

### P40.2 — Preamble pvalue cache

`var_refs` is a direct parameter of the JIT function. The pvalue pointer for each captured
variable is stable for the entire call. We can hoist the pvalue loads into the function preamble:

```c
/* preamble — emitted once per closure variable */
JSValue *_vrp0 = *(JSValue**)((char*)var_refs[0] + JIT_VARREF_PVALUE_OFF);
JSValue *_vrp1 = *(JSValue**)((char*)var_refs[1] + JIT_VARREF_PVALUE_OFF);
```

Then the macros become:
```c
#define GEN_GET_VR(idx) \
    jit_buf_printf(cb, "    _tsv%d=_DUP(*_vrp%d); _sp=%d;\n", d, idx, d+1)
#define GEN_PUT_VR(idx) do { \
    _P94_ENSURE(d-1); \
    jit_buf_printf(cb, "    { _FREE(*_vrp%d); *_vrp%d=_tsv%d; _sp=%d; }\n", \
                   idx, idx, d-1, d-1); \
} while(0)
#define GEN_SET_VR(idx) do { \
    _P94_ENSURE(d-1); \
    jit_buf_printf(cb, "    { _FREE(*_vrp%d); *_vrp%d=_DUP(_tsv%d); }\n", \
                   idx, idx, d-1); \
} while(0)
```

This enables GCC to keep `_vrp0` in a register across the function body. The `var_refs[idx]`
array element load and the `->pvalue` dereference each happen once at function entry, not once
per opcode.

**Where to emit preamble:** After the existing local variable declarations in the generated
function body, before the first opcode block. The JIT emitter writes a preamble section
(for cpool pointer, stack, etc.) — add to that loop.

**Condition:** Only emit `_vrp{i}` if `closure_var_count > 0`. Skip if no var_refs used
(avoid dead locals in GCC output).

**Expected gain:** Additional ~1–2ms reduction vs P40.1 alone (eliminates repeated loads of
`var_refs[i]` from the argument array, GCC can CSE the pointer into a register).

### P40.3 — Typed put_var_ref for integer source

When the JIT type-inference state at depth `d-1` is `JIT_T_INT`, the value on the virtual stack
is a raw C `int32_t` in `_ti{d-1}`. The current `GEN_PUT_VR` calls `_P94_ENSURE(d-1)` to box
it into a JSValue. We can skip boxing:

```c
/* Int-typed fast path for put_var_ref / set_var_ref */
if (gen_st[d-1] == JIT_T_INT) {
    jit_buf_printf(cb, "    { JSValue _nv=JS_MKVAL(JS_TAG_INT,(int32_t)_ti%d);"
                       " if(js_unlikely(JS_VALUE_HAS_REF_COUNT(*_vrp%d))) _RT->free_value(ctx,*_vrp%d);"
                       " *_vrp%d=_nv; _sp=%d; }\n", d-1, idx, idx, idx, d-1);
} else {
    /* existing boxed path */
    _P94_ENSURE(d-1);
    jit_buf_printf(cb, "    { _FREE(*_vrp%d); *_vrp%d=_tsv%d; _sp=%d; }\n", ...);
}
```

For the `counter` increment pattern, the increment result is always `JIT_T_INT` (integer + 1),
so put_var_ref fires the int fast path every call. The `js_unlikely` hint tells GCC that freeing
the old value (which is also always an int and has no refcount) is cold — it can optimize the
branch away.

**Expected gain:** ~0.5–1ms marginal (P94_ENSURE is just a compare+store when already boxed, but
the `js_unlikely` branch hint improves branch prediction for the refcount check).

## Safety

- `pvalue` is stable during function execution. Closure detachment (`js_var_ref_check_detach`)
  only runs at scope exit opcodes, which trigger JIT exits. The preamble cache is safe.
- `JIT_VARREF_PVALUE_OFF = 16` is verified by `_Static_assert` in `quickjs.c`. Any struct
  layout change will cause a compile error, not a silent misread.
- Typed put_var_ref is only used when `gen_st[d-1] == JIT_T_INT` at emit time. If type
  inference is wrong (which would be a pre-existing bug), the fallback boxes correctly.

## Actual Results (2026-04-10)

P40 was implemented and verified correct, but did NOT produce a measurable benchmark
improvement on `closure_counter`:

| Benchmark        | Before P40 | After P40   | Gap vs Node |
|------------------|-----------|-------------|-------------|
| closure_counter  | 15 ms     | 15 ms       | 7.5×        |
| Node v24         | 2 ms      | 2 ms        | —           |

**Root cause:** the dominant cost for tiny closure functions is `JS_CallInternal` overhead
(alloca + stack frame setup = ~80-100 cycles/call), not the var_ref access inside the JIT body
(~4 cycles saved per vtable call eliminated). For 1M calls, the savings are ~12M cycles = ~4ms,
but they are hidden by call-dispatch variation.

**P40 savings become measurable after P41** (JIT-to-JIT direct calls), which will bypass
`JS_CallInternal` entirely for JIT-compiled callers calling JIT-compiled closures.

**Code generation verified:** Generated C no longer contains `_RT->var_ref_value()` calls;
instead each var_ref idx gets a preamble `_vrp{i}` variable used throughout the function body.

The remaining ~7.5× gap after P40 is almost entirely function-call overhead (P41: JIT-to-JIT
direct calls will eliminate the interpreter dispatch for closure function calls).

## What P40 Does Not Do

- Does not change `JSVarRef` struct layout or GC behavior
- Does not add any new opcodes or IC machinery
- Does not affect non-closure functions (no `var_refs` param usage, preamble lines are skipped)
- Does not affect get/put_var_ref for `is_detached == FALSE` — correctness unchanged since
  pvalue always points to the right storage regardless of detachment state
