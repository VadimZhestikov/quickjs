/* P39.3 test: put_field borrow refcount integrity
 *
 * Verifies that the put_field borrow elision (skipping _FREE(_o) when the
 * object was loaded without DupValue) does not corrupt object refcounts.
 *
 * A: function pw(o, n) { for (var i = 0; i < n; i++) o.x = i; }
 *    Call with n=10000, assert o.x == 9999
 * B: Object is still usable after many JIT calls (refcount not drifted;
 *    we verify by reading o.x and checking the object is accessible)
 * C: Closure capturing object — put_field through captured var works correctly
 *    (captured vars take the _CAP_LOC path, not the borrow path)
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

    printf("=== P39.3: put_field borrow refcount integrity ===\n");

    /* ------------------------------------------------------------------ */
    /* A: pw(o, 10000) — assert o.x == 9999 after call                    */
    /* ------------------------------------------------------------------ */
    printf("=== A: put_field borrow — correct final value ===\n");
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        JSValue r = eval_ok(ctx,
            "function pw(o, n) { for (var i = 0; i < n; i++) o.x = i; }\n"
            "var obj = {x: 0};\n"
            /* Warm up to trigger JIT */
            "for (var k = 0; k < 200; k++) pw(obj, 10);\n"
            /* Now run 10000 iterations */
            "pw(obj, 10000);\n"
            "obj.x;\n"
        );
        int iv = 0;
        JS_ToInt32(ctx, &iv, r);
        JS_FreeValue(ctx, r);
        if (iv != 9999) {
            fprintf(stderr, "  A: FAIL — expected obj.x == 9999, got %d\n", iv);
            all_pass = 0;
        } else {
            printf("  A: PASS — pw(obj, 10000) → obj.x == 9999\n");
        }

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* ------------------------------------------------------------------ */
    /* B: Object still usable after many JIT put_field calls               */
    /* ------------------------------------------------------------------ */
    printf("=== B: object usable after put_field borrow calls ===\n");
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        /* Define and warm up the write function */
        eval_ok(ctx,
            "function pw(o, n) { for (var i = 0; i < n; i++) o.x = i; }\n"
            "var obj = {x: 0};\n"
            "for (var k = 0; k < 200; k++) pw(obj, 10);\n"
        );

        /* Get reference to obj from JS world */
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue obj = JS_GetPropertyStr(ctx, global, "obj");
        if (JS_IsException(obj)) fail("B: failed to get obj");

        /* Run JIT function many times with obj */
        JSValue pw_fn = JS_GetPropertyStr(ctx, global, "pw");
        JSValue n_val = JS_NewInt32(ctx, 1000);
        JSValue args[2] = { obj, n_val };
        JSValue res = JS_Call(ctx, pw_fn, JS_UNDEFINED, 2, args);
        if (JS_IsException(res)) { js_std_dump_error(ctx); fail("B: pw call failed"); }
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, n_val);
        JS_FreeValue(ctx, pw_fn);

        /* Verify obj is still accessible and has the correct value */
        JSValue x_val = JS_GetPropertyStr(ctx, obj, "x");
        if (JS_IsException(x_val)) {
            fprintf(stderr, "  B: FAIL — obj.x inaccessible (possible refcount corruption)\n");
            all_pass = 0;
        } else {
            int iv = 0;
            JS_ToInt32(ctx, &iv, x_val);
            JS_FreeValue(ctx, x_val);
            if (iv != 999) {
                fprintf(stderr, "  B: FAIL — obj.x expected 999, got %d\n", iv);
                all_pass = 0;
            } else {
                printf("  B: PASS — obj.x == 999 (object usable, refcount intact)\n");
            }
        }

        JS_FreeValue(ctx, obj);
        JS_FreeValue(ctx, global);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* ------------------------------------------------------------------ */
    /* C: Closure capturing object — put_field through closure              */
    /* ------------------------------------------------------------------ */
    printf("=== C: closure-captured object, put_field correct ===\n");
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        /* make() captures o; returned function writes o.x in a loop */
        JSValue r = eval_ok(ctx,
            "function make() {\n"
            "  var o = {x: 0};\n"
            "  return function(n) {\n"
            "    for (var i = 0; i < n; i++) o.x = i;\n"
            "    return o.x;\n"
            "  };\n"
            "}\n"
            "var fn = make();\n"
            /* Warm up the inner function */
            "for (var k = 0; k < 200; k++) fn(10);\n"
            /* Final call with n=100: expect 99 */
            "fn(100);\n"
        );
        int iv = 0;
        JS_ToInt32(ctx, &iv, r);
        JS_FreeValue(ctx, r);
        if (iv != 99) {
            fprintf(stderr, "  C: FAIL — expected 99, got %d\n", iv);
            all_pass = 0;
        } else {
            printf("  C: PASS — closure put_field: fn(100) == 99\n");
        }

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    if (all_pass) {
        printf("ALL P39.3 TESTS PASSED\n");
        return 0;
    } else {
        printf("SOME P39.3 TESTS FAILED\n");
        return 1;
    }
}
