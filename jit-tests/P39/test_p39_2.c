/* P39.2 test: prop_arr caching correctness
 *
 * Verifies that caching p->prop in JSJITICEntry.prop_arr at IC fill time
 * is correct: the cached pointer is valid while shape_gen matches, and is
 * properly refreshed on shape change (shape transition invalidates the IC).
 *
 * A: f({x:42}) returns correct value after warm-up (cached prop_arr used)
 * B: After adding a property to force shape change, IC refills with new
 *    prop_arr; f(o) still returns the correct value
 * C: Two objects with same shape layout: both IC hits return correct values
 *    from their respective prop arrays (bimorphic IC)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "quickjs-libc.h"
#include "quickjs-jit.h"

static void fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static JSValue eval_ok(JSContext *ctx, const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { js_std_dump_error(ctx); fail("eval exception"); }
    return v;
}

int main(void)
{
    int all_pass = 1;

    printf("=== P39.2: prop_arr caching correctness ===\n");

    /* ------------------------------------------------------------------ */
    /* A: Cached prop_arr returns correct value                             */
    /* ------------------------------------------------------------------ */
    printf("=== A: prop_arr cache hit returns correct value ===\n");
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        JSValue r = eval_ok(ctx,
            "function f(o) { return o.x; }\n"
            "var obj = {x: 42};\n"
            "var result = 0;\n"
            "for (var i = 0; i < 200; i++) result = f(obj);\n"
            "result;\n"
        );
        int iv = 0;
        JS_ToInt32(ctx, &iv, r);
        JS_FreeValue(ctx, r);
        if (iv != 42) {
            fprintf(stderr, "  A: FAIL — expected 42, got %d\n", iv);
            all_pass = 0;
        } else {
            printf("  A: PASS — f({x:42}) == 42 (prop_arr cache correct)\n");
        }

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* ------------------------------------------------------------------ */
    /* B: Shape change → IC misses, refills with new prop_arr, still works  */
    /* ------------------------------------------------------------------ */
    printf("=== B: shape change invalidates prop_arr, refill correct ===\n");
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        /* Warm up with {x:5}, then add a property to force shape transition */
        JSValue r = eval_ok(ctx,
            "function f(o) { return o.x; }\n"
            "var obj = {x: 5};\n"
            /* Warm up — IC fills with shape A, prop_arr cached */
            "for (var i = 0; i < 200; i++) f(obj);\n"
            /* Add property — shape transitions to shape B, shape_gen++ */
            /* IC will miss on next call because shape_gen differs */
            "obj.extra = 99;\n"
            /* Call again — IC misses, refills with new shape/prop_arr */
            "var result = 0;\n"
            "for (var i = 0; i < 5; i++) result = f(obj);\n"
            "result;\n"
        );
        int iv = 0;
        JS_ToInt32(ctx, &iv, r);
        JS_FreeValue(ctx, r);
        if (iv != 5) {
            fprintf(stderr, "  B: FAIL — expected 5, got %d\n", iv);
            all_pass = 0;
        } else {
            printf("  B: PASS — f(obj) == 5 after shape change (prop_arr refreshed)\n");
        }

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* ------------------------------------------------------------------ */
    /* C: Two objects with same shape layout — bimorphic IC works           */
    /* ------------------------------------------------------------------ */
    printf("=== C: two objects, same shape, bimorphic IC correct ===\n");
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        /* objA and objB have the same shape (same property layout) but
         * different instances with different x values. */
        JSValue r = eval_ok(ctx,
            "function f(o) { return o.x; }\n"
            "var objA = {x: 10};\n"
            "var objB = {x: 20};\n"
            /* Both objects share the same shape (same prop layout),
             * so this exercises the monomorphic IC hitting two instances. */
            "var sum = 0;\n"
            "for (var i = 0; i < 200; i++) { sum += f(objA); sum += f(objB); }\n"
            /* Each of 200 iterations: 10 + 20 = 30 */
            "sum;\n"
        );
        int iv = 0;
        JS_ToInt32(ctx, &iv, r);
        JS_FreeValue(ctx, r);
        /* 200 * 30 = 6000 */
        if (iv != 6000) {
            fprintf(stderr, "  C: FAIL — expected 6000, got %d\n", iv);
            all_pass = 0;
        } else {
            printf("  C: PASS — sum over 200 iters of objA(10)+objB(20) == 6000\n");
        }

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    if (all_pass) {
        printf("ALL P39.2 TESTS PASSED\n");
        return 0;
    } else {
        printf("SOME P39.2 TESTS FAILED\n");
        return 1;
    }
}
