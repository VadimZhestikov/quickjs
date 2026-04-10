/* P9 test: closure var_refs correctness in JIT
 *
 * Verifies that JIT-compiled closures correctly capture and mutate their
 * enclosing variable via var_refs:
 *   A) A counter closure increments its captured variable on each call.
 *   B) Two independent closures over the same factory do not share state.
 *   C) A closure that captures multiple variables handles all of them correctly.
 *
 * Note: closures have var_refs; we use JS_Call (not js_jit_call_fb) to invoke
 * them so var_refs are properly threaded through the call.
 *
 * Build (from quickjs/):
 *   make -C jit_tests/P9
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

/* Call fn() with no args via JS_Call, return int result. */
static int call0i(JSContext *ctx, JSValue fn)
{
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(r)) { js_std_dump_error(ctx); fail("call0i: exception"); }
    int out;
    JS_ToInt32(ctx, &out, r);
    JS_FreeValue(ctx, r);
    return out;
}

/* Call fn() N times via JS_Call; just to warm up for JIT compilation. */
static void warmup(JSContext *ctx, JSValue fn, int n)
{
    for (int i = 0; i < n; i++) {
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) { js_std_dump_error(ctx); fail("warmup: exception"); }
        JS_FreeValue(ctx, r);
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
    /* A: counter closure captures and increments a single variable         */
    /* ------------------------------------------------------------------ */
    printf("=== A: counter closure var_ref correctness ===\n");

    eval_ok(ctx,
        "function makeCounter(start) {\n"
        "  return function() { return start++; };\n"
        "}\n"
        "var ctr = makeCounter(10);\n");

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctr = JS_GetPropertyStr(ctx, global, "ctr");

    /* Warm up to trigger JIT compilation of the inner closure */
    warmup(ctx, ctr, 3);   /* calls return 10,11,12 */
    js_jit_drain();
    js_jit_install_results();

    /* Verify the inner closure is JIT-compiled */
    JSFunctionBytecode *b_ctr = js_jit_get_callee_fb(ctr);
    if (!b_ctr) fail("A: no bytecode for ctr closure");
    if (js_jit_fb_get_tier(b_ctr) != 2) {
        fprintf(stderr, "FAIL A: closure not at tier 2 (tier=%d)\n",
                (int)js_jit_fb_get_tier(b_ctr));
        exit(1);
    }

    /* Continue calling via JS_Call (which threads var_refs correctly).
     * The warmup consumed 10,11,12 so next calls should return 13,14,15. */
    CHECK_INT(call0i(ctx, ctr), 13, "A: ctr()→13");
    CHECK_INT(call0i(ctx, ctr), 14, "A: ctr()→14");
    CHECK_INT(call0i(ctx, ctr), 15, "A: ctr()→15");

    printf("PASS A: counter closure at tier 2, correctly increments start\n");

    /* ------------------------------------------------------------------ */
    /* B: two independent closures over same factory don't share state       */
    /* ------------------------------------------------------------------ */
    printf("=== B: independent closures have independent var_refs ===\n");

    eval_ok(ctx,
        "var ctrA = makeCounter(0);\n"
        "var ctrB = makeCounter(100);\n");

    JSValue ctrA = JS_GetPropertyStr(ctx, global, "ctrA");
    JSValue ctrB = JS_GetPropertyStr(ctx, global, "ctrB");

    /* Both use the same function bytecode (already tier 2). */
    CHECK_INT(call0i(ctx, ctrA),   0, "B: ctrA()→0");
    CHECK_INT(call0i(ctx, ctrB), 100, "B: ctrB()→100");
    CHECK_INT(call0i(ctx, ctrA),   1, "B: ctrA()→1");
    CHECK_INT(call0i(ctx, ctrB), 101, "B: ctrB()→101");
    CHECK_INT(call0i(ctx, ctrA),   2, "B: ctrA()→2");

    printf("PASS B: independent closures do not share captured variable\n");

    /* ------------------------------------------------------------------ */
    /* C: closure capturing two variables                                   */
    /* ------------------------------------------------------------------ */
    printf("=== C: closure capturing two variables ===\n");

    eval_ok(ctx,
        "function makeAccum(base, step) {\n"
        "  return function() { base += step; return base; };\n"
        "}\n"
        "var acc = makeAccum(5, 3);\n");

    JSValue acc = JS_GetPropertyStr(ctx, global, "acc");

    warmup(ctx, acc, 2);   /* calls return 8, 11 */
    js_jit_drain();
    js_jit_install_results();

    JSFunctionBytecode *b_acc = js_jit_get_callee_fb(acc);
    if (!b_acc) fail("C: no bytecode for acc closure");
    if (js_jit_fb_get_tier(b_acc) != 2) {
        fprintf(stderr, "FAIL C: acc closure not at tier 2 (tier=%d)\n",
                (int)js_jit_fb_get_tier(b_acc));
        exit(1);
    }

    /* warmup consumed 8,11 → next should be 14,17,20 */
    CHECK_INT(call0i(ctx, acc), 14, "C: acc()→14");
    CHECK_INT(call0i(ctx, acc), 17, "C: acc()→17");
    CHECK_INT(call0i(ctx, acc), 20, "C: acc()→20");

    printf("PASS C: two-variable closure at tier 2, correct accumulation\n");

    /* ------------------------------------------------------------------ */
    JS_FreeValue(ctx, ctr);
    JS_FreeValue(ctx, ctrA);
    JS_FreeValue(ctx, ctrB);
    JS_FreeValue(ctx, acc);
    JS_FreeValue(ctx, global);
    js_jit_free();
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("ALL P9 TESTS PASSED\n");
    return 0;
}
