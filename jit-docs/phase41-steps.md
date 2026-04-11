# Phase 41 Implementation Steps

## Prerequisites

- P40 committed (done: 48f51c7)
- `quickjs-jit.h` has `JIT_VARREF_PVALUE_OFF = 24`
- Generated JIT C uses `_vrp{i}` for all var_ref access
- `js_jit_fb_func_kind(b)` accessor is available in `quickjs-jit.h`

## Step 1 — Verify accessor availability

Check that `js_jit_fb_func_kind` is declared in `quickjs-jit.h` and implemented in
`quickjs.c`. Search for it:

```sh
grep -n 'js_jit_fb_func_kind' quickjs.c quickjs-jit.h
```

Expected: declaration in `quickjs-jit.h` and a 1-line body in `quickjs.c`.
If missing, add:

```c
/* quickjs.c */
uint8_t js_jit_fb_func_kind(JSFunctionBytecode *b) { return b->func_kind; }

/* quickjs-jit.h */
uint8_t js_jit_fb_func_kind(JSFunctionBytecode *b);
```

## Step 2 — P41.1: Early JIT bypass in JS_CallInternal

**File:** `quickjs.c`

### 2a. Find the insertion point

Search for the block that starts with `b = p->u.func.function_bytecode;` inside
`JS_CallInternal`, just before the `if (unlikely(argc < b->arg_count || ...))` check.
This is currently around line 19548.

Pattern to locate:
```c
b = p->u.func.function_bytecode;

    if (unlikely(argc < b->arg_count || (flags & JS_CALL_FLAG_COPY_ARGV))) {
```

### 2b. Insert the fast-path block

After `b = p->u.func.function_bytecode;` and before the `arg_allocated_size` computation,
add:

```c
#ifdef CONFIG_JIT
    /* P41.1: Early JIT fast-path — bypass alloca and most JSStackFrame setup.
     *
     * Fires when all of:
     *   1. Function is already JIT-compiled (jit_func != NULL)
     *   2. var_ref_count == 0: no new JSVarRefs created → close_var_refs is a no-op
     *   3. has_simple_parameter_list: arg_count is exact; no rest/default/destructuring
     *   4. argc >= arg_count: no argv padding needed; caller's argv is large enough
     *   5. func_kind == JS_FUNC_NORMAL: generators/async need SF for coroutine resumption
     *   6. No COPY_ARGV flag: caller's argv is stable for the duration of the JIT call
     *
     * Sets only the fields that JIT code or exception handling actually reads:
     *   prev_frame, cur_func, new_target, arg_count, cur_pc, var_refs (NULL).
     * Skips alloca, var_buf/stack_buf/var_refs initialization, and close_var_refs. */
    {
        JSJITFunc _jf41 = __atomic_load_n(&b->jit_func, __ATOMIC_ACQUIRE);
        if (_jf41 != NULL &&
            b->var_ref_count == 0 &&
            b->has_simple_parameter_list &&
            argc >= b->arg_count &&
            b->func_kind == 0 /* JS_FUNC_NORMAL */ &&
            !(flags & JS_CALL_FLAG_COPY_ARGV))
        {
            sf->prev_frame   = rt->current_stack_frame;
            sf->cur_func     = (JSValue)func_obj;
            sf->new_target   = (JSValue)new_target;
            sf->arg_count    = argc;
            sf->cur_pc       = b->byte_code_buf; /* start of bytecode for backtrace */
            sf->var_refs     = NULL;              /* var_ref_count==0: never accessed */
            rt->current_stack_frame = sf;
            ctx = b->realm;                       /* must switch to callee's realm */
            JSValue _ret41 = _jf41(ctx, (JSValue)this_obj, b->arg_count, argv,
                                   b->cpool, p->u.func.var_refs);
            rt->current_stack_frame = sf->prev_frame;
            /* close_var_refs skipped: var_ref_count==0 guarantees it is a no-op. */
            if (unlikely(arg_allocated_size)) { /* never taken — but suppress warning */ }
            return _ret41;
        }
    }
#endif
```

Note: the `if (unlikely(arg_allocated_size))` at the end can be omitted — it's just to
silence a potential "unused variable" warning since `arg_allocated_size` is declared before
this block. Better: declare `arg_allocated_size` AFTER the fast-path block.

### 2c. Adjust variable declarations if needed

If `arg_allocated_size` is declared before the fast-path insertion point (e.g., at the top
of the function), you may need to move its declaration to just before the
`if (unlikely(argc < b->arg_count))` block. Check the existing declaration site.

If `arg_allocated_size` is declared at the top of JS_CallInternal (as `int i,
arg_allocated_size, ...`), just leave it — the fast path returns before using it.

## Step 3 — Build and verify P41.1

```sh
make CONFIG_JIT=y -j$(nproc) 2>&1 | grep -E 'error:|warning:.*error'
```

Run a quick smoke test:
```sh
cat > /tmp/test_p41.js << 'EOF'
function make_counter() {
    let n = 0;
    return () => ++n;
}
const counter = make_counter();
for (let i = 0; i < 200; i++) counter();
print(counter());   // should print 201
EOF
./qjs /tmp/test_p41.js   # uses CONFIG_JIT=y build
```

Run the full test suite:
```sh
make CONFIG_JIT=y test
```

Run the benchmark to measure improvement:
```sh
cat > /tmp/bench_p41.js << 'EOF'
function make_counter() { let n = 0; return () => ++n; }
const counter = make_counter();
for (let i = 0; i < 200; i++) counter();  // warm up JIT compilation
const t0 = Date.now();
for (let i = 0; i < 1000000; i++) counter();
print("closure_counter (interp caller): " + (Date.now()-t0) + "ms");
EOF
./qjs /tmp/bench_p41.js   # repeat 3 times for stable reading
```

Expected: improvement from ~38ms to ~30ms.

## Step 4 — P41.2: Slim fast-call for zero-arg closures in call IC

### 4a. Add `callee_is_fast` + `callee_func_val` to JSJITCallICEntry

**File:** `quickjs-jit.h`

Find `JSJITCallICEntry` struct definition. Add two new fields:

```c
typedef struct {
    void                *expected_func;
    JSFunctionBytecode  *expected_bc;
    JSJITFunc            direct_jit;
    JSValue             *callee_cpool;
    JSVarRef           **callee_var_refs;
    int                  callee_arg_count;
    uint64_t             callee_bc_hash;
    /* P41.2: fast-call flag and cached callee func object for cur_func update. */
    uint8_t              callee_is_fast;  /* 1 if zero-arg + no HOME_OBJECT */
    JSValue              callee_func_val; /* DUP'd ref for sf->cur_func update */
} JSJITCallICEntry;
```

### 4b. Add `js_jit_ic_fast_call` to vtable

**File:** `quickjs-jit.h` (in JSJITRuntime struct)

Add after `callIC_fill`:
```c
    JSValue (*jit_fast_call)(JSContext *ctx, JSValue this_val,
                             JSJITCallICEntry *ic);
```

**File:** `quickjs.c` (define the function):

```c
/* P41.2: Slim call-IC fast path for zero-arg closures with no HOME_OBJECT.
 * Skips: argc/argv padding, new_target save/restore, arg_count branching.
 * Only saves/restores sf->cur_func for backtrace quality. */
JSValue js_jit_ic_fast_call(JSContext *ctx, JSValue this_val,
                             JSJITCallICEntry *ic)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSStackFrame *sf = rt->current_stack_frame;
    JSValue saved = JS_UNDEFINED;
    if (likely(sf != NULL)) {
        saved = sf->cur_func;
        sf->cur_func = ic->callee_func_val;
    }
    JSValue ret = ic->direct_jit(ctx, this_val, 0, NULL,
                                 ic->callee_cpool, ic->callee_var_refs);
    if (likely(sf != NULL))
        sf->cur_func = saved;
    return ret;
}
```

**File:** `quickjs-jit.c` (register in vtable):
```c
    .jit_fast_call = js_jit_ic_fast_call,
```

### 4c. Populate `callee_is_fast` and `callee_func_val` in IC fill

**File:** `quickjs.c`, in `js_jit_callIC_fill` (around line 16327)

In the cold-miss path where IC fields are populated, after setting `direct_jit`:

```c
/* P41.2: mark as fast-callable if zero-arg and no HOME_OBJECT needed */
ic->callee_is_fast = (b->arg_count == 0 && !b->need_home_object) ? 1 : 0;
if (ic->callee_is_fast) {
    JS_FreeValue(ctx, ic->callee_func_val);  /* free previous (if warm re-fill) */
    ic->callee_func_val = JS_DupValue(ctx, func_obj);  /* caller must supply func */
}
```

Wait — `js_jit_callIC_fill` takes `JSValue func` (not `JSObject*`). The func JSValue is
the callee function object. Store it:
```c
ic->callee_func_val = JS_DupValue(ctx, func);
```

And in the fill function's warm-hit path (re-fill when direct_jit was NULL, now set):
```c
ic->callee_func_val = JS_DupValue(ctx, func);  /* refresh */
```

**Lifetime:** The IC entry is a static variable in generated C. When the IC is evicted
(megamorphic), free the old value:
```c
if (ic->callee_is_fast)
    JS_FreeValue(ctx, ic->callee_func_val);
ic->callee_is_fast = 0;
ic->callee_func_val = JS_UNDEFINED;
```

This requires calling `JS_FreeValue` at IC fill time when marking megamorphic. Check the
megamorphic path in `js_jit_callIC_fill` and add the free there.

### 4d. Emit fast-call in the call IC emitter

**File:** `quickjs-jit.c`, in the OP_call emitter (around line 5401–5471)

In the IC hot-path branch, currently:
```c
/* IC hit: */
ret = js_jit_ic_direct_call(ctx, this_val, nargs, args, &_cic, var_refs);
```

Change to emit a runtime check on `callee_is_fast`:
```c
jit_buf_printf(cb,
    "      if(_cic%d.callee_is_fast){\n"
    "        JSValue _r=%s(ctx,_tsv_this,&_cic%d);\n"  /* _RT->jit_fast_call */
    "        _CHK(_r); ... }\n"
    "      else {\n"
    "        JSValue _r=js_jit_ic_direct_call(ctx,_tsv_this,%d,_ca%d,&_cic%d,_vr);\n"
    ...
    "      }\n",
    ic_idx, /* _RT->jit_fast_call */ "(_RT->jit_fast_call)", ic_idx,
    nargs, call_idx, ic_idx);
```

Or simpler: emit `_RT->jit_fast_call` unconditionally for known zero-arg call sites
(where `nargs == 0` at emit time, detected from the call instruction's argc operand).

**Simpler approach (preferred):** At emit time, if the bytecode call instruction has
`argc == 0` (check `bc[pc+1]` for OP_call's argument count byte), emit the fast path
unconditionally rather than runtime-checking `callee_is_fast`. The IC fill will set
`callee_is_fast = 1` for eligible callees; if the callee turns out to be ineligible
(has home_object, non-zero args), the IC will set `callee_is_fast = 0` and the
`_RT->jit_fast_call` will still call `ic->direct_jit` correctly (since it passes 0 args).

Wait — if callee has args but we call with 0 args and `callee_is_fast = 0`, we need to
fall back to `js_jit_ic_direct_call`. So the simplest correct approach is the runtime check.

### 4e. Initialize `callee_func_val` in static IC declarations

In the emitter, every `static JSJITCallICEntry _cic{N} = {NULL,...}` needs to initialize
`callee_func_val`:

```c
static JSJITCallICEntry _cic11 = {NULL,NULL,NULL,NULL,NULL,0,0,0,JS_UNDEFINED};
```

The new fields are zero-initialized by default for C `= {NULL,...}` if the struct is
larger, but explicit is safer.

## Step 5 — Write tests

### `jit-tests/P41/Makefile`

Copy from P40/Makefile pattern:
```makefile
CC     = gcc
CFLAGS = -O2 -Wall -Wextra -I../.. -I../../.obj
LDFLAGS = ../../libquickjs.a -lm -ldl -lpthread

TESTS = test_p41_1 test_p41_2 test_p41_3

all: $(TESTS)

%: %.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

run: all
	@passed=0; failed=0; \
	for t in $(TESTS); do \
	    if ./$$t; then passed=$$((passed+1)); \
	    else echo "  FAIL: $$t"; failed=$$((failed+1)); fi; \
	done; \
	echo "P41: $$passed passed, $$failed failed"

clean:
	rm -f $(TESTS)
```

### `test_p41_1.c` — P41.1 correctness: interpreted→JIT closure

Test that a counter closure called many times from interpreted JS returns correct values:
```c
// JS: function make_counter(){ let n=0; return ()=>++n; }
// const c = make_counter();
// for(let i=0;i<200;i++) c(); // trigger JIT
// c(); c(); c(); // assert returns 201, 202, 203
```
- Verify sequential increment is correct
- Verify a string-capturing closure correctly frees old string on int overwrite

### `test_p41_2.c` — P41.1 exception path: exception propagates through fast bypass

Test that a closure that throws propagates the exception correctly:
```c
// JS: function make_thrower(){ let n=0; return ()=>{ if(++n>5) throw n; return n; }; }
// const f = make_thrower();
// for(let i=0;i<200;i++) { try { f(); } catch(e) {} }
// // Verify: n==200, 195 exceptions caught
```
- Verify exception is caught correctly
- Verify no refcount leaks (run with ASAN or check via final result)

### `test_p41_3.c` — P41.2 correctness: JIT-compiled outer function calls closure

Test that when a JIT-compiled function calls a closure via call IC, the fast-call path
works correctly:
```c
// JS: function make_counter(){ let n=0; return ()=>++n; }
// function bench(f, N){ let sum=0; for(let i=0;i<N;i++) sum+=f(); return sum; }
// const c = make_counter();
// for(let j=0;j<200;j++) bench(c,5);  // warm up bench's JIT
// assert(bench(c,10) == sum(1001..1010))
```
- Verify arithmetic correctness after JIT-to-JIT path

## Step 6 — Benchmark

Run both benchmark scenarios:

```sh
# Scenario 1: interpreted caller
cat > /tmp/bench_interp.js << 'EOF'
function make_counter() { let n = 0; return () => ++n; }
const counter = make_counter();
for (let i = 0; i < 200; i++) counter();
const t0 = Date.now();
for (let i = 0; i < 1000000; i++) counter();
print("interpreted caller: " + (Date.now()-t0) + "ms");
EOF

# Scenario 2: JIT-compiled caller
cat > /tmp/bench_jit.js << 'EOF'
function make_counter() { let n = 0; return () => ++n; }
function bench(f, N) { for (let i = 0; i < N; i++) f(); }
const counter = make_counter();
for (let j = 0; j < 200; j++) bench(counter, 5);  // warm up bench
const t0 = Date.now();
bench(counter, 1000000);
print("JIT caller: " + (Date.now()-t0) + "ms");
EOF

./qjs /tmp/bench_interp.js   # repeat 3 times
./qjs /tmp/bench_jit.js      # repeat 3 times
```

## Step 7 — Update docs

### `jit-docs/phase41-jit-call-bypass.md`

Fill in actual benchmark numbers in the "Expected Results" table.

### `jit-docs/performance-benchmarks.md`

Add §10 P41 Results section with before/after tables for both call scenarios.

## Step 8 — Update jit-tests/Makefile

Add P41 targets:
```makefile
run-p41:
    @make -C P41 run
clean-p41:
    @make -C P41 clean
```

Add `run-p41` to the `run` target and `clean-p41` to `clean`.

## Step 9 — Commit

```sh
git add quickjs.c quickjs-jit.h quickjs-jit.c \
        jit-docs/phase41-jit-call-bypass.md jit-docs/phase41-steps.md \
        jit-docs/performance-benchmarks.md \
        jit-tests/P41/ jit-tests/Makefile
git commit -m "jit: P41 — early JIT bypass in JS_CallInternal + slim IC fast-call"
```

## Verification Checklist

- [ ] `make CONFIG_JIT=y test` passes with no regressions
- [ ] Closure counter returns correct values at N=201, 202, 203
- [ ] Exception thrown by JIT closure propagates through fast bypass correctly
- [ ] closure_counter (interpreted caller) improved: 38ms → ~30ms
- [ ] closure_counter (JIT caller) improved: 15ms → ~12ms
- [ ] No ASAN errors in test_p41_2 (exception path)
- [ ] Generated JIT C still contains `_vrp{i}` preamble (P40 not regressed)
- [ ] `grep 'var_ref_value' ~/.cache/qjs-jit/*.c` returns no matches

## Implementation Notes

### Why `b->arg_count` not `argc` in the fast-path call

The JIT function signature has `int argc` but the JIT-generated code accesses
`argv[0..arg_count-1]` unconditionally (via `GEN_PUT_ARG`). Passing `b->arg_count` as
`argc` ensures the callee's argument-read logic is consistent. Since `argc >= b->arg_count`
is a precondition for the fast path, no out-of-bounds access occurs.

### Why generators/async are excluded

Generator and async functions use `JSStackFrame` as a coroutine resumption point:
- `sf->cur_sp` tracks the current stack pointer for resume
- `sf->cur_pc` tracks the current bytecode offset for resume
- `js_jit_gen_init_frame` stores and restores state from the SF

These functions MUST have a valid, persistent `JSStackFrame` that outlives the initial
call. The fast bypass allocates `sf_s` on the C stack of `JS_CallInternal` which is
valid for one call but then discarded — this is fine for normal functions that complete
in one call, but not for generators/async which expect to re-enter the same frame.

### Why `!(flags & JS_CALL_FLAG_COPY_ARGV)` is required

`JS_CALL_FLAG_COPY_ARGV` signals that the caller needs argv to be preserved (e.g., for
ES6 rest parameters via `JS_Call`). The fast path passes `argv` directly to the JIT
function which may write back to `argv[i]` via `GEN_PUT_ARG`. If the flag is set, we
cannot use the caller's `argv` directly — fall through to the normal path.

### Megamorphic IC cleanup for callee_func_val

When `js_jit_callIC_fill` marks an IC as megamorphic (different callees at the same
call site), it must free `ic->callee_func_val` before resetting the struct:
```c
if (ic->callee_is_fast && !JS_IsUndefined(ic->callee_func_val)) {
    JS_FreeValue(ctx, ic->callee_func_val);
    ic->callee_func_val = JS_UNDEFINED;
    ic->callee_is_fast = 0;
}
```
Failure to do this causes a memory leak when a call site becomes megamorphic.
