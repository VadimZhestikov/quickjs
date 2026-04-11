# Phase 40 Implementation Steps

## Prerequisites

- P39 committed (done: 66530f4)
- `quickjs-jit.h` contains `JSJITICEntry` with `prop_arr`, `JIT_IC_CHECK_FAST` without prop_count check
- `quickjs.c` has `js_jit_ic_fill_get/put` setting `ic->prop_arr`
- `quickjs-jit.c` var_ref macros use `_RT->var_ref_value()` vtable (lines ~3722–3734)

## Step 1 — Add `JIT_VARREF_PVALUE_OFF` to `quickjs-jit.h`

**File:** `quickjs-jit.h`

Find the block of `JIT_SHAPEIC_*` and related offset defines. Add after them:

```c
/* Byte offset of JSVarRef.pvalue within the struct.
 * Verified by _Static_assert in quickjs.c.
 * JSGCObjectHeader union occupies bytes 0-15 (16 bytes on 64-bit). */
#define JIT_VARREF_PVALUE_OFF 16
```

## Step 2 — Add `_Static_assert` in `quickjs.c`

**File:** `quickjs.c`

Find where `JSVarRef` is typedef'd (search for `typedef struct JSVarRef`). Add immediately after
the closing `} JSVarRef;`:

```c
_Static_assert(offsetof(JSVarRef, pvalue) == JIT_VARREF_PVALUE_OFF,
               "JSVarRef.pvalue offset mismatch — update JIT_VARREF_PVALUE_OFF");
```

This requires `#include "quickjs-jit.h"` to be present in `quickjs.c` (it already is, for the
IC structs). Verify the build compiles cleanly.

## Step 3 — P40.1: Replace `GEN_GET_VR`, `GEN_PUT_VR`, `GEN_SET_VR` macros

**File:** `quickjs-jit.c`, lines ~3722–3734

Replace the three macros:

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

Build and verify closure tests pass. Run `./qjs_jit tests/test_jit_closure.js` (or equivalent).

## Step 4 — P40.2: Emit pvalue preamble in generated function

**File:** `quickjs-jit.c`

### 4a. Locate the preamble emission section

Find where the JIT emitter writes per-function declarations before the opcode loop. Look for
the section that emits local variable declarations like `JSValue *cpool = ...` or stack setup.
This is in `jit_emit_function` (or equivalent emitter function), approximately after the
opening `{` of the generated C function.

Search for: `"    JSValue *cpool"` or the stack-setup emit — the preamble block.

### 4b. Add `closure_var_count` access

Find `s->closure_var_count` in the emitter context. The `JSFunctionBytecode *b` struct has
`b->closure_var_count`. Confirm the emitter has access to `b` (it does, it's used throughout
for opcode iteration).

### 4c. Emit `_vrp{i}` declarations

In the preamble block, after existing declarations, add:

```c
/* P40.2: cache pvalue pointers for all captured variables */
if (b->closure_var_count > 0) {
    for (int vri = 0; vri < b->closure_var_count; vri++) {
        jit_buf_printf(cb,
            "    JSValue *_vrp%d=*(JSValue**)((char*)var_refs[%d]+JIT_VARREF_PVALUE_OFF);\n",
            vri, vri);
    }
}
```

### 4d. Update the three macros to use `_vrp{idx}`

Replace the P40.1 macros with:

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

Build and verify. Inspect a generated `.c` file for a closure function to confirm `_vrp0` etc.
appear at the top and are used in opcode bodies.

## Step 5 — P40.3: Typed fast path for put_var_ref with int source

**File:** `quickjs-jit.c`

Find the `OP_put_var_ref` and `OP_set_var_ref` emitter cases (grep for `put_var_ref`).

### 5a. Update `GEN_PUT_VR` to check int type

Replace the `GEN_PUT_VR` macro usage in `OP_put_var_ref` with inline logic (or update the macro
to take a `gen_st` argument). Simpler: expand inline in the emitter case:

```c
case OP_put_var_ref: {
    int idx = get_u16(pc + 1);
    pc += 3;
    if (gen_st[d-1] == JIT_T_INT) {
        jit_buf_printf(cb,
            "    { JSValue _nv=JS_MKVAL(JS_TAG_INT,(int32_t)_ti%d);"
            " if(js_unlikely(JS_VALUE_HAS_REF_COUNT(*_vrp%d))) _RT->free_value(ctx,*_vrp%d);"
            " *_vrp%d=_nv; _sp=%d; }\n",
            d-1, idx, idx, idx, d-1);
    } else {
        GEN_PUT_VR(idx);
    }
    d--;
    break;
}
```

Do the same for `OP_set_var_ref` (which leaves the value on the stack, i.e., does not decrement
`d`):

```c
case OP_set_var_ref: {
    int idx = get_u16(pc + 1);
    pc += 3;
    if (gen_st[d-1] == JIT_T_INT) {
        jit_buf_printf(cb,
            "    { JSValue _nv=JS_MKVAL(JS_TAG_INT,(int32_t)_ti%d);"
            " if(js_unlikely(JS_VALUE_HAS_REF_COUNT(*_vrp%d))) _RT->free_value(ctx,*_vrp%d);"
            " *_vrp%d=_nv; }\n",
            d-1, idx, idx, idx);
    } else {
        GEN_SET_VR(idx);
    }
    /* d unchanged — value stays on stack */
    break;
}
```

Build. Run closure tests including `test_closure_int.js` (to be created).

## Step 6 — Write tests

**Directory:** `jit-tests/P40/`

Create `jit-tests/P40/Makefile` (copy pattern from P39):

```makefile
CC      = gcc
CFLAGS  = -O2 -g -Wall -I../.. -I../../.obj
LDFLAGS = -ldl -lpthread

TESTS = test_p40_1 test_p40_2 test_p40_3

all: $(TESTS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $< ../../quickjs.c ../../quickjs-libc.c \
	    ../../libregexp.c ../../libunicode.c ../../cutils.c \
	    ../../quickjs-jit.c $(LDFLAGS)

run: all
	./test_p40_1 && echo "P40.1 PASS"
	./test_p40_2 && echo "P40.2 PASS"
	./test_p40_3 && echo "P40.3 PASS"

clean:
	rm -f $(TESTS) *.o /tmp/qjs_jit_p40_*.so /tmp/qjs_jit_p40_*.c
```

### test_p40_1.c — Direct pvalue access correctness

Test that get/put/set var_ref correctly reads and writes through the byte-offset path.

```c
/* Verify that closure variable access via JIT_VARREF_PVALUE_OFF is correct.
 * Run: counter_closure() 5 times, expect values 1..5. */
```

- Create a JSRuntime+JSContext
- Enable JIT
- Run JS: `(function(){ let n=0; const f=()=>++n; f();f();f(); return n; })()`
- Assert result == 3

### test_p40_2.c — Preamble cache correctness (multiple var_refs)

Test a closure that captures two variables:

```js
(function() {
    let a = 10, b = 20;
    const f = () => { a += b; b++; };
    f(); f(); f();
    return a + b;  // 10+20+21+22 + 23 = 96
})()
```

Assert result == 96.

### test_p40_3.c — Typed put_var_ref refcount integrity

Test that the int-typed fast path correctly handles freeing a previous non-int value:

```js
(function() {
    let x = "hello";   // string, has refcount
    const f = () => { x = 42; };  // put_var_ref with int source, must free "hello"
    f();
    return x;
})()
```

Assert result == 42 (no leak, no crash under ASAN).

## Step 7 — Benchmark

Run `closure_counter` before and after:

```sh
./qjs_jit jit_perf_tests/v8bench/closure_counter.js  # or equivalent
```

Expected: 15ms → ~10ms.

Also run property benchmarks to confirm no regression in P39 results.

## Step 8 — Update docs

### `jit-docs/phase40-varref-inline.md`

Fill in actual benchmark results in the Expected Results table.

### `jit-docs/performance-benchmarks.md`

Add §9 P40 Results section:

```markdown
## §9 Phase 40 — Closure Variable Inline Access

### Setup
- Benchmark: closure_counter (1M calls to a closure incrementing an integer variable)
- Baseline: after P39

### Results

| Sub-phase | closure_counter | Notes |
|-----------|----------------|-------|
| Before P40 | 15 ms | vtable call per var_ref access |
| P40.1 done | ~11 ms | direct byte-offset dereference |
| P40.2 done | ~10 ms | preamble cache, pvalue in register |
| P40.3 done | ~9.5 ms | int-typed put_var_ref skips boxing |
```

## Step 9 — Update `jit-tests/Makefile`

Add P40 targets:

```makefile
run-p40:
	$(MAKE) -C P40 run

clean-p40:
	$(MAKE) -C P40 clean
```

Add `run-p40` to the `run-all` target and `clean-p40` to `clean-all`.

## Step 10 — Commit

```sh
git add quickjs-jit.h quickjs.c quickjs-jit.c \
        jit-docs/phase40-varref-inline.md jit-docs/phase40-steps.md \
        jit-docs/performance-benchmarks.md \
        jit-tests/P40/ jit-tests/Makefile
git commit -m "jit: P40 — closure variable inline access via pvalue byte offset"
```

## Verification Checklist

- [ ] `_Static_assert` compiles without error
- [ ] `make` succeeds with no new warnings
- [ ] All P40 C tests pass
- [ ] All existing P38/P39 tests still pass
- [ ] `closure_counter` benchmark improved by ≥3ms
- [ ] No ASAN errors on test_p40_3 (refcount integrity)
- [ ] Generated `.c` for closure function shows `_vrp0` in preamble
- [ ] `grep 'var_ref_value' /tmp/qjs_jit_*.c` returns no matches after P40.2
