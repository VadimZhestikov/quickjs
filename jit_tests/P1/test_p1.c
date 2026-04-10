/* P1 test: threshold/tier promotion
 *
 * Verifies that:
 *   A) A function reaches tier 2 (JIT-compiled) after being called >= threshold times.
 *   B) A function stays at tier 0 when called fewer times than the threshold.
 *   C) An ineligible function (contains eval) stays at tier 0 and has jit_no_compile=1.
 *
 * Build (from quickjs/):
 *   make -C jit_tests/P1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "quickjs-libc.h"
#include "quickjs-jit.h"

#define THRESHOLD 3

static void fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

/* Eval a script; die on exception. */
static void eval_ok(JSContext *ctx, const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        js_std_dump_error(ctx);
        fail("eval raised an exception");
    }
    JS_FreeValue(ctx, v);
}

/* Get the bytecode of a global function by name. */
static JSFunctionBytecode *get_fb(JSContext *ctx, const char *name)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, name);
    JS_FreeValue(ctx, global);
    JSFunctionBytecode *b = js_jit_get_callee_fb(fn);
    JS_FreeValue(ctx, fn);
    return b;
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);
    js_jit_init();
    js_jit_set_threshold(THRESHOLD);

    /* ------------------------------------------------------------------ */
    /* A: function reaches tier 2 after >= threshold calls                  */
    /* ------------------------------------------------------------------ */
    printf("=== A: tier promotion after threshold ===\n");

    eval_ok(ctx,
        "function add(a,b){ return a+b; }\n"
        "for(var i=0;i<10;i++) add(i,i+1);\n");

    js_jit_drain();
    js_jit_install_results();

    JSFunctionBytecode *b_add = get_fb(ctx, "add");
    if (!b_add)
        fail("A: could not get bytecode for 'add'");

    uint8_t tier = js_jit_fb_get_tier(b_add);
    if (tier != 2) {
        fprintf(stderr, "FAIL: A: expected tier 2, got %d\n", (int)tier);
        exit(1);
    }
    JSJITFunc fn = js_jit_fb_get_func(b_add);
    if (!fn)
        fail("A: jit_func is NULL but tier==2");

    printf("PASS A: add() at tier %d, jit_func=%p\n", (int)tier, (void*)fn);

    /* ------------------------------------------------------------------ */
    /* B: function stays at tier 0 when called fewer times than threshold   */
    /* ------------------------------------------------------------------ */
    printf("=== B: tier 0 below threshold ===\n");

    /* Define a new function and call it (threshold-1) times = 2 times */
    eval_ok(ctx,
        "function cold(a,b){ return a*b; }\n"
        "cold(1,2); cold(2,3);\n");   /* 2 calls < THRESHOLD=3 */

    js_jit_drain();
    js_jit_install_results();

    JSFunctionBytecode *b_cold = get_fb(ctx, "cold");
    if (!b_cold)
        fail("B: could not get bytecode for 'cold'");

    uint8_t tier_cold = js_jit_fb_get_tier(b_cold);
    if (tier_cold != 0) {
        fprintf(stderr, "FAIL: B: expected tier 0, got %d\n", (int)tier_cold);
        exit(1);
    }
    printf("PASS B: cold() at tier %d (not JIT-compiled)\n", (int)tier_cold);

    /* ------------------------------------------------------------------ */
    /* C: ineligible function (eval) never reaches tier 2                   */
    /* ------------------------------------------------------------------ */
    printf("=== C: ineligible function stays at tier 0 ===\n");

    eval_ok(ctx,
        "function witheval(s){ return eval(s); }\n"
        "for(var i=0;i<20;i++) witheval('1+1');\n");

    js_jit_drain();
    js_jit_install_results();

    JSFunctionBytecode *b_eval = get_fb(ctx, "witheval");
    if (!b_eval)
        fail("C: could not get bytecode for 'witheval'");

    uint8_t tier_eval = js_jit_fb_get_tier(b_eval);
    if (tier_eval != 0) {
        fprintf(stderr, "FAIL: C: expected tier 0 for eval function, got %d\n",
                (int)tier_eval);
        exit(1);
    }
    if (!js_jit_fb_jit_no_compile(b_eval))
        fail("C: expected jit_no_compile=1 for eval function");

    printf("PASS C: witheval() at tier %d, jit_no_compile=1\n", (int)tier_eval);

    /* ------------------------------------------------------------------ */
    js_jit_free();
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("ALL P1 TESTS PASSED\n");
    return 0;
}
