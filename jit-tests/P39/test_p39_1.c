/* P39.1 test: IC check correctness without prop_count condition
 *
 * Verifies that removing the `prop_count > slot` condition from JIT_IC_CHECK
 * and JIT_IC_CHECK_FAST does not break IC correctness.  The shape_gen match
 * already guarantees slot < prop_count at IC check time.
 *
 * A: f(o) returns correct value after JIT warm-up (IC hits correctly)
 * B: f() with object of different shape causes IC miss/refill, still correct
 * C: Megamorphic after 3 shapes — all calls still return correct values
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

    printf("=== P39.1: IC check correctness without prop_count condition ===\n");

    /* ------------------------------------------------------------------ */
    /* A: IC hits correctly — f({x:42}) returns 42 after warm-up           */
    /* ------------------------------------------------------------------ */
    printf("=== A: IC hit on same shape after warm-up ===\n");
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        /* Warm up enough to trigger JIT compilation */
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
            printf("  A: PASS — f({x:42}) == 42 after warm-up\n");
        }

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* ------------------------------------------------------------------ */
    /* B: IC refills on shape change, still returns correct value           */
    /* ------------------------------------------------------------------ */
    printf("=== B: IC miss + refill on shape change ===\n");
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        /* Warm up with shape A {x:N}, then call with shape B {x:N, y:0} */
        JSValue r = eval_ok(ctx,
            "function f(o) { return o.x; }\n"
            "var objA = {x: 5};\n"
            "var objB = {x: 99, y: 0};\n"
            /* Warm up with objA (shape A) */
            "for (var i = 0; i < 200; i++) f(objA);\n"
            /* Now call with objB (different shape — IC miss, refill) */
            "var result = 0;\n"
            "for (var i = 0; i < 5; i++) result = f(objB);\n"
            "result;\n"
        );
        int iv = 0;
        JS_ToInt32(ctx, &iv, r);
        JS_FreeValue(ctx, r);
        if (iv != 99) {
            fprintf(stderr, "  B: FAIL — expected 99, got %d\n", iv);
            all_pass = 0;
        } else {
            printf("  B: PASS — f({x:99, y:0}) == 99 after IC refill\n");
        }

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* ------------------------------------------------------------------ */
    /* C: Megamorphic after 3 shapes — all return correct values            */
    /* ------------------------------------------------------------------ */
    printf("=== C: megamorphic demotion — all shapes still correct ===\n");
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        /* Three distinct shapes; after third, IC goes megamorphic (slow path) */
        JSValue r = eval_ok(ctx,
            "function f(o) { return o.x; }\n"
            "var o1 = {x: 1};\n"
            "var o2 = {x: 2, y: 0};\n"
            "var o3 = {x: 3, y: 0, z: 0};\n"
            /* Warm up: cycle through all three shapes */
            "for (var i = 0; i < 200; i++) { f(o1); f(o2); f(o3); }\n"
            /* Verify correctness after megamorphic demotion */
            "var ok = (f(o1) === 1 && f(o2) === 2 && f(o3) === 3) ? 1 : 0;\n"
            "ok;\n"
        );
        int iv = 0;
        JS_ToInt32(ctx, &iv, r);
        JS_FreeValue(ctx, r);
        if (iv != 1) {
            fprintf(stderr, "  C: FAIL — megamorphic path returned wrong values\n");
            all_pass = 0;
        } else {
            printf("  C: PASS — megamorphic: f(o1)==1, f(o2)==2, f(o3)==3\n");
        }

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    if (all_pass) {
        printf("ALL P39.1 TESTS PASSED\n");
        return 0;
    } else {
        printf("SOME P39.1 TESTS FAILED\n");
        return 1;
    }
}
