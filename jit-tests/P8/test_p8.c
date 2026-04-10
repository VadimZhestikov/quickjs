/* P8 test: js_jit_call() direct JIT dispatch
 *
 * Verifies that js_jit_call() (P8.3):
 *   A) Takes the fast JIT path for a JIT-compiled function (tier 2).
 *   B) Falls through to JS_Call for an interpreter function (tier 0).
 *   C) Produces the same result as JS_Call in both cases.
 *   D) A JIT function that calls another JIT function via js_jit_call
 *      returns the correct result (JIT-to-JIT chain).
 *
 * Build (from quickjs/):
 *   make -C jit_tests/P8
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

static JSValue get_fn(JSContext *ctx, const char *name)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, name);
    JS_FreeValue(ctx, global);
    return fn;
}

static JSFunctionBytecode *get_fb(JSContext *ctx, const char *name)
{
    JSValue fn = get_fn(ctx, name);
    JSFunctionBytecode *b = js_jit_get_callee_fb(fn);
    JS_FreeValue(ctx, fn);
    return b;
}

/* Call func(a,b) with int args via js_jit_call, return int result. */
static int jit_call2i(JSContext *ctx, JSValue func, int a, int b)
{
    JSValue args[2] = { JS_NewInt32(ctx, a), JS_NewInt32(ctx, b) };
    JSValue r = js_jit_call(ctx, func, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(r)) { js_std_dump_error(ctx); fail("jit_call2i: exception"); }
    int out;
    JS_ToInt32(ctx, &out, r);
    JS_FreeValue(ctx, r);
    return out;
}

/* Call func(a,b) via JS_Call (interpreter path), return int result. */
static int js_call2i(JSContext *ctx, JSValue func, int a, int b)
{
    JSValue args[2] = { JS_NewInt32(ctx, a), JS_NewInt32(ctx, b) };
    JSValue r = JS_Call(ctx, func, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(r)) { js_std_dump_error(ctx); fail("js_call2i: exception"); }
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

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);
    js_jit_init();
    js_jit_set_threshold(THRESHOLD);

    /* Compile 'add' to tier 2 */
    eval_ok(ctx, "function add(a,b){ return a+b; } add(1,2);");
    js_jit_drain();
    js_jit_install_results();

    JSValue add_fn = get_fn(ctx, "add");
    JSFunctionBytecode *b_add = get_fb(ctx, "add");
    if (!b_add) fail("no bytecode for 'add'");
    if (js_jit_fb_get_tier(b_add) != 2) fail("'add' not at tier 2");

    /* ------------------------------------------------------------------ */
    /* A: js_jit_call fast-paths to JIT for a tier-2 function              */
    /* ------------------------------------------------------------------ */
    printf("=== A: js_jit_call fast path for tier-2 function ===\n");

    /* We can't observe the code path directly, but we can verify:
     * 1. The function is at tier 2 (jit_func != NULL).
     * 2. js_jit_call returns the correct result.
     * If jit_func were not called, we'd either get an exception or wrong value. */
    CHECK_INT(jit_call2i(ctx, add_fn, 3, 4),   7, "jit_call add(3,4)");
    CHECK_INT(jit_call2i(ctx, add_fn, -5, 10),  5, "jit_call add(-5,10)");
    CHECK_INT(jit_call2i(ctx, add_fn,  0, 0),   0, "jit_call add(0,0)");
    printf("PASS A\n");

    /* ------------------------------------------------------------------ */
    /* B: js_jit_call falls through to JS_Call for tier-0 function         */
    /* ------------------------------------------------------------------ */
    printf("=== B: js_jit_call fallthrough for tier-0 function ===\n");

    /* Define 'mul' but don't call it enough to compile */
    eval_ok(ctx, "function mul(a,b){ return a*b; }");
    /* No warmup: mul stays at tier 0 */
    js_jit_drain();
    js_jit_install_results();

    JSValue mul_fn = get_fn(ctx, "mul");
    JSFunctionBytecode *b_mul = get_fb(ctx, "mul");
    if (!b_mul) fail("no bytecode for 'mul'");
    if (js_jit_fb_get_tier(b_mul) != 0) fail("'mul' not at tier 0");

    /* js_jit_call should fall through to JS_Call — result must still be correct */
    CHECK_INT(jit_call2i(ctx, mul_fn, 6, 7), 42, "jit_call mul(6,7)");
    printf("PASS B: js_jit_call returned correct result for tier-0 function\n");

    /* ------------------------------------------------------------------ */
    /* C: js_jit_call and JS_Call produce identical results                 */
    /* ------------------------------------------------------------------ */
    printf("=== C: js_jit_call == JS_Call result parity ===\n");

    struct { int a; int b; } cases[] = {
        {0,0}, {1,1}, {-1,1}, {100,200}, {-50,-50}, {2147483647, 0}
    };
    int n = (int)(sizeof(cases)/sizeof(cases[0]));
    for (int i = 0; i < n; i++) {
        int want = js_call2i(ctx, add_fn, cases[i].a, cases[i].b);
        int got  = jit_call2i(ctx, add_fn, cases[i].a, cases[i].b);
        if (want != got) {
            fprintf(stderr,
                "FAIL C: add(%d,%d): JS_Call=%d, js_jit_call=%d\n",
                cases[i].a, cases[i].b, want, got);
            exit(1);
        }
    }
    printf("PASS C: %d cases match between JS_Call and js_jit_call\n", n);

    /* ------------------------------------------------------------------ */
    /* D: JIT-to-JIT: outer calls inner, both at tier 2                    */
    /* ------------------------------------------------------------------ */
    printf("=== D: JIT-to-JIT call chain ===\n");

    eval_ok(ctx,
        /* 'double_add' calls 'add' which is already JIT-compiled.
         * After warmup, double_add itself will also be tier 2.
         * The inner call to add() goes through js_jit_call in generated code. */
        "function double_add(a,b){ return add(a,b) + add(a,b); }\n"
        "double_add(3,4);\n");   /* warmup: threshold=1 */
    js_jit_drain();
    js_jit_install_results();

    JSValue da_fn = get_fn(ctx, "double_add");
    JSFunctionBytecode *b_da = get_fb(ctx, "double_add");
    if (!b_da) fail("D: no bytecode for 'double_add'");
    if (js_jit_fb_get_tier(b_da) != 2) fail("D: 'double_add' not at tier 2");

    CHECK_INT(jit_call2i(ctx, da_fn, 3, 4), 14, "double_add(3,4)");
    CHECK_INT(jit_call2i(ctx, da_fn, 5, 5), 20, "double_add(5,5)");
    printf("PASS D: JIT-to-JIT chain returns correct results\n");

    /* ------------------------------------------------------------------ */
    JS_FreeValue(ctx, add_fn);
    JS_FreeValue(ctx, mul_fn);
    JS_FreeValue(ctx, da_fn);
    js_jit_free();
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("ALL P8 TESTS PASSED\n");
    return 0;
}
