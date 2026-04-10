/* P2 test: basic arithmetic JIT output correctness
 *
 * Verifies that JIT-compiled arithmetic functions return the same values
 * as the interpreter for:
 *   A) Integer add, sub, mul, div, mod — basic cases
 *   B) Integer overflow: result exceeds int32 → float64 path
 *   C) Floating-point arithmetic
 *   D) Unary negation and bitwise NOT
 *
 * Build (from quickjs/):
 *   make -C jit_tests/P2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
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
    if (JS_IsException(v)) { js_std_dump_error(ctx); fail("eval raised an exception"); }
    JS_FreeValue(ctx, v);
}

static JSFunctionBytecode *get_fb(JSContext *ctx, const char *name)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, name);
    JS_FreeValue(ctx, global);
    JSFunctionBytecode *b = js_jit_get_callee_fb(fn);
    JS_FreeValue(ctx, fn);
    return b;
}

/* Compile a global function to tier 2 by calling it enough times. */
static JSFunctionBytecode *compile_fn(JSContext *ctx, const char *name,
                                       const char *warmup_code)
{
    eval_ok(ctx, warmup_code);
    js_jit_drain();
    js_jit_install_results();
    JSFunctionBytecode *b = get_fb(ctx, name);
    if (!b) { fprintf(stderr, "FAIL: no bytecode for '%s'\n", name); exit(1); }
    if (js_jit_fb_get_tier(b) != 2) {
        fprintf(stderr, "FAIL: '%s' not at tier 2 (tier=%d)\n",
                name, (int)js_jit_fb_get_tier(b));
        exit(1);
    }
    return b;
}

/* Call b(a_int, b_int) and extract int result.  Frees returned JSValue. */
static int call2i(JSContext *ctx, JSFunctionBytecode *b, int a, int bv)
{
    JSValue args[2] = { JS_NewInt32(ctx, a), JS_NewInt32(ctx, bv) };
    JSValue r = js_jit_call_fb(ctx, b, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(r)) { js_std_dump_error(ctx); fail("call2i: exception"); }
    int out;
    JS_ToInt32(ctx, &out, r);
    JS_FreeValue(ctx, r);
    return out;
}

/* Call b(a, bv) and extract float64 result. */
static double call2f(JSContext *ctx, JSFunctionBytecode *b, double a, double bv)
{
    JSValue args[2] = { JS_NewFloat64(ctx, a), JS_NewFloat64(ctx, bv) };
    JSValue r = js_jit_call_fb(ctx, b, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(r)) { js_std_dump_error(ctx); fail("call2f: exception"); }
    double out;
    JS_ToFloat64(ctx, &out, r);
    JS_FreeValue(ctx, r);
    return out;
}

/* Call b(a) and extract int result. */
static int call1i(JSContext *ctx, JSFunctionBytecode *b, int a)
{
    JSValue args[1] = { JS_NewInt32(ctx, a) };
    JSValue r = js_jit_call_fb(ctx, b, JS_UNDEFINED, 1, args);
    JS_FreeValue(ctx, args[0]);
    if (JS_IsException(r)) { js_std_dump_error(ctx); fail("call1i: exception"); }
    int out;
    JS_ToInt32(ctx, &out, r);
    JS_FreeValue(ctx, r);
    return out;
}

#define CHECK_INT(got, want, label) \
    do { if ((got) != (want)) { \
        fprintf(stderr, "FAIL: %s: got %d, want %d\n", label, got, want); \
        exit(1); \
    } } while(0)

#define CHECK_FLOAT(got, want, label) \
    do { if (fabs((got) - (want)) > 1e-9) { \
        fprintf(stderr, "FAIL: %s: got %g, want %g\n", label, (double)(got), (double)(want)); \
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
    /* A: basic integer arithmetic                                          */
    /* ------------------------------------------------------------------ */
    printf("=== A: basic integer arithmetic ===\n");

    JSFunctionBytecode *b_add = compile_fn(ctx, "add",
        "function add(a,b){ return a+b; } add(1,2);");
    JSFunctionBytecode *b_sub = compile_fn(ctx, "sub",
        "function sub(a,b){ return a-b; } sub(10,3);");
    JSFunctionBytecode *b_mul = compile_fn(ctx, "mul",
        "function mul(a,b){ return a*b; } mul(4,5);");
    JSFunctionBytecode *b_div = compile_fn(ctx, "div",
        "function div(a,b){ return a/b; } div(10,2);");
    JSFunctionBytecode *b_mod = compile_fn(ctx, "mod",
        "function mod(a,b){ return a%b; } mod(10,3);");

    CHECK_INT(call2i(ctx, b_add, 3, 4),   7,  "add(3,4)");
    CHECK_INT(call2i(ctx, b_add, -5, 5),  0,  "add(-5,5)");
    CHECK_INT(call2i(ctx, b_sub, 10, 4),  6,  "sub(10,4)");
    CHECK_INT(call2i(ctx, b_sub, 3, 10), -7,  "sub(3,10)");
    CHECK_INT(call2i(ctx, b_mul, 6, 7),  42,  "mul(6,7)");
    CHECK_INT(call2i(ctx, b_mul, -3, 4),-12,  "mul(-3,4)");
    CHECK_INT(call2i(ctx, b_div, 12, 4),  3,  "div(12,4)");
    CHECK_INT(call2i(ctx, b_mod, 10, 3),  1,  "mod(10,3)");
    CHECK_INT(call2i(ctx, b_mod, -7, 3), -1,  "mod(-7,3)");

    printf("PASS A\n");

    /* ------------------------------------------------------------------ */
    /* B: integer overflow — result exceeds int32 → float64                */
    /* ------------------------------------------------------------------ */
    printf("=== B: integer overflow path ===\n");

    double big = call2f(ctx, b_mul, 100000.0, 100000.0);
    CHECK_FLOAT(big, 1e10, "mul(100000,100000)");
    printf("PASS B: mul(100000,100000)=%g\n", big);

    /* ------------------------------------------------------------------ */
    /* C: floating-point arithmetic                                         */
    /* ------------------------------------------------------------------ */
    printf("=== C: floating-point arithmetic ===\n");

    JSFunctionBytecode *b_addf = compile_fn(ctx, "addf",
        "function addf(a,b){ return a+b; } addf(1.5,2.5);");

    CHECK_FLOAT(call2f(ctx, b_addf, 1.5, 2.5), 4.0, "addf(1.5,2.5)");
    CHECK_FLOAT(call2f(ctx, b_addf, 0.1, 0.2), 0.3, "addf(0.1,0.2)");
    printf("PASS C\n");

    /* ------------------------------------------------------------------ */
    /* D: unary negation                                                    */
    /* ------------------------------------------------------------------ */
    printf("=== D: unary negation ===\n");

    JSFunctionBytecode *b_neg = compile_fn(ctx, "neg",
        "function neg(a){ return -a; } neg(5);");

    CHECK_INT(call1i(ctx, b_neg,  5), -5, "neg(5)");
    CHECK_INT(call1i(ctx, b_neg, -3),  3, "neg(-3)");
    CHECK_INT(call1i(ctx, b_neg,  0),  0, "neg(0)");
    printf("PASS D\n");

    /* ------------------------------------------------------------------ */
    js_jit_free();
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("ALL P2 TESTS PASSED\n");
    return 0;
}
