/* P10 test: exception propagation through JIT code
 *
 * When a function is at tier 2, JS_Call invokes the JIT-compiled native code
 * directly via the hot probe.  Verifies that:
 *   A) An explicit `throw` in JIT code produces JS_EXCEPTION at the JS_Call caller.
 *   B) The runtime is usable after the exception is cleared.
 *   C) An exception raised by a helper (e.g., reading property of undefined)
 *      propagates through JIT code.
 *   D) Exception from a nested JIT call propagates to the outer JIT caller.
 *
 * Note: functions that use `new Error(...)` capture `Error` as a closure variable.
 * We use JS_Call (not js_jit_call_fb) so that var_refs are properly threaded from
 * the live closure object — the same path the hot probe uses when tier==2.
 *
 * Build (from quickjs/):
 *   make -C jit_tests/P10
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "quickjs-libc.h"
#include "quickjs-jit.h"

#define THRESHOLD 1

static void fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static void eval_ok(JSContext *ctx, const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { js_std_dump_error(ctx); fail("eval exception"); }
    JS_FreeValue(ctx, v);
}

/* Get a global function JSValue by name. */
static JSValue get_fn(JSContext *ctx, const char *name)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, name);
    JS_FreeValue(ctx, global);
    return fn;
}

/* Get the bytecode of a global function; verify it's at tier 2. */
static JSFunctionBytecode *require_tier2(JSContext *ctx, const char *name)
{
    JSValue fn = get_fn(ctx, name);
    JSFunctionBytecode *b = js_jit_get_callee_fb(fn);
    JS_FreeValue(ctx, fn);
    if (!b) { fprintf(stderr, "FAIL: no bytecode for '%s'\n", name); exit(1); }
    if (js_jit_fb_get_tier(b) != 2) {
        fprintf(stderr, "FAIL: '%s' not at tier 2 (tier=%d)\n",
                name, (int)js_jit_fb_get_tier(b));
        exit(1);
    }
    return b;
}

/* Call fn(a,b) via JS_Call, return int.  Die on exception. */
static int call2i(JSContext *ctx, JSValue fn, int a, int b)
{
    JSValue args[2] = { JS_NewInt32(ctx, a), JS_NewInt32(ctx, b) };
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(r)) { js_std_dump_error(ctx); fail("call2i: unexpected exception"); }
    int out;
    JS_ToInt32(ctx, &out, r);
    JS_FreeValue(ctx, r);
    return out;
}

/* Call fn(a,b) and expect an exception.  Returns 1 if exception raised, 0 if not. */
static int call2_expect_exc(JSContext *ctx, JSValue fn, int a, int b)
{
    JSValue args[2] = { JS_NewInt32(ctx, a), JS_NewInt32(ctx, b) };
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (!JS_IsException(r)) { JS_FreeValue(ctx, r); return 0; }
    /* Clear the pending exception before continuing */
    JS_FreeValue(ctx, JS_GetException(ctx));
    return 1;
}

/* Warmup fn N times via JS_Call. */
static void warmup2(JSContext *ctx, JSValue fn, int n)
{
    for (int i = 0; i < n; i++) {
        JSValue args[2] = { JS_NewInt32(ctx, i+1), JS_NewInt32(ctx, i+2) };
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        if (JS_IsException(r)) { JS_FreeValue(ctx, JS_GetException(ctx)); }
        else JS_FreeValue(ctx, r);
    }
}

#define CHECK_INT(got, want, label) \
    do { if ((got) != (want)) { \
        fprintf(stderr, "FAIL: %s: got %d, want %d\n", label, got, want); \
        exit(1); \
    } } while(0)

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);
    js_jit_init();
    js_jit_set_threshold(THRESHOLD);

    /* ------------------------------------------------------------------ */
    /* A: explicit throw — JS_EXCEPTION propagates to JS_Call caller       */
    /* ------------------------------------------------------------------ */
    printf("=== A: explicit throw propagates JS_EXCEPTION ===\n");

    eval_ok(ctx,
        "function safe_div(a,b) {\n"
        "  if (b === 0) throw new Error('division by zero');\n"
        "  return a / b;\n"
        "}\n"
        "safe_div(10, 2);\n");   /* warmup — triggers JIT compilation */
    js_jit_drain();
    js_jit_install_results();

    JSValue div_fn = get_fn(ctx, "safe_div");
    require_tier2(ctx, "safe_div");

    /* Non-throwing call must succeed */
    CHECK_INT(call2i(ctx, div_fn, 10, 2), 5, "A: safe_div(10,2)");

    /* Throwing call must raise JS_EXCEPTION */
    if (!call2_expect_exc(ctx, div_fn, 5, 0))
        fail("A: safe_div(5,0) did not raise JS_EXCEPTION");

    printf("PASS A: JS_EXCEPTION raised by throw and cleared cleanly\n");

    /* ------------------------------------------------------------------ */
    /* B: runtime is usable after exception is cleared                      */
    /* ------------------------------------------------------------------ */
    printf("=== B: runtime usable after cleared exception ===\n");

    CHECK_INT(call2i(ctx, div_fn, 20, 4),  5, "B: safe_div(20,4) after exc");
    CHECK_INT(call2i(ctx, div_fn, -6, 3), -2, "B: safe_div(-6,3) after exc");
    printf("PASS B\n");

    /* ------------------------------------------------------------------ */
    /* C: exception from helper (reading property of undefined)            */
    /* ------------------------------------------------------------------ */
    printf("=== C: exception from helper propagates ===\n");

    eval_ok(ctx,
        "function get_x(obj) { return obj.x; }\n"
        "get_x({x:1});\n");
    js_jit_drain();
    js_jit_install_results();

    JSValue get_x_fn = get_fn(ctx, "get_x");
    require_tier2(ctx, "get_x");

    /* Call with valid object */
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue obj_ok;
        {
            JSValue v = JS_Eval(ctx, "({x:42})", 7, "<t>", JS_EVAL_TYPE_GLOBAL);
            obj_ok = v;
        }
        if (JS_IsException(obj_ok)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            fail("C: failed to create test object");
        }
        JSValue r = JS_Call(ctx, get_x_fn, JS_UNDEFINED, 1, &obj_ok);
        JS_FreeValue(ctx, obj_ok);
        JS_FreeValue(ctx, global);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            fail("C: get_x({x:42}) unexpectedly threw");
        }
        int x;
        JS_ToInt32(ctx, &x, r);
        JS_FreeValue(ctx, r);
        if (x != 42) {
            fprintf(stderr, "FAIL C: get_x({x:42}) returned %d\n", x);
            exit(1);
        }
    }

    /* Call with undefined — must raise TypeError */
    {
        JSValue undef = JS_UNDEFINED;
        JSValue r = JS_Call(ctx, get_x_fn, JS_UNDEFINED, 1, &undef);
        if (!JS_IsException(r)) {
            JS_FreeValue(ctx, r);
            fail("C: get_x(undefined) did not raise exception");
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    printf("PASS C: helper-raised exception propagates through JIT\n");

    /* ------------------------------------------------------------------ */
    /* D: exception from nested JIT call propagates to outer JIT caller    */
    /* ------------------------------------------------------------------ */
    printf("=== D: nested JIT exception propagation ===\n");

    eval_ok(ctx,
        "function outer(a,b) { return safe_div(a,b) * 2; }\n"
        "outer(4,2);\n");
    js_jit_drain();
    js_jit_install_results();

    JSValue outer_fn = get_fn(ctx, "outer");
    require_tier2(ctx, "outer");

    /* Normal call */
    CHECK_INT(call2i(ctx, outer_fn, 4, 2), 4, "D: outer(4,2)");

    /* Nested throw: safe_div(5,0) → outer propagates JS_EXCEPTION */
    if (!call2_expect_exc(ctx, outer_fn, 5, 0))
        fail("D: outer(5,0) did not propagate JS_EXCEPTION");

    /* Runtime usable after nested exception */
    CHECK_INT(call2i(ctx, outer_fn, 10, 5), 4, "D: outer(10,5) after nested exc");

    printf("PASS D: nested JIT exception propagates to outer JIT caller\n");

    /* ------------------------------------------------------------------ */
    JS_FreeValue(ctx, div_fn);
    JS_FreeValue(ctx, get_x_fn);
    JS_FreeValue(ctx, outer_fn);
    js_jit_free();
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("ALL P10 TESTS PASSED\n");
    return 0;
}
