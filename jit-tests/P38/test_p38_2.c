/* P38.2 test: typed index fast path for array access
 *
 * When the loop counter is a typed int64_t, the JIT should skip boxing it
 * to JSValue and use the native value directly as the array index.
 *
 * A: Loop with typed int index returns correct sum
 * B: Array indexed by JSValue (non-typed) still works (fallback path)
 * C: Out-of-bounds index → undefined (no crash), fast path misses correctly
 * D: Boundary: index == arr.length → undefined
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
    printf("=== P38.2: typed index fast path ===\n");

    /* ------------------------------------------------------------------ */
    /* A: loop counter is typed int — direct native access                  */
    /* ------------------------------------------------------------------ */
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        JSValue r = eval_ok(ctx,
            "function f(arr) {\n"
            "  var s = 0;\n"
            "  for (var i = 0; i < arr.length; i++) s += arr[i];\n"
            "  return s;\n"
            "}\n"
            "var a = [10, 20, 30];\n"
            "for (var k = 0; k < 200; k++) f(a);\n"
            "f(a);\n"
        );
        int iv = 0;
        JS_ToInt32(ctx, &iv, r);
        JS_FreeValue(ctx, r);
        if (iv != 60) {
            fprintf(stderr, "FAIL A: expected 60, got %d\n", iv);
            exit(1);
        }
        printf("PASS A: f([10,20,30]) == %d (typed index)\n", iv);

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* ------------------------------------------------------------------ */
    /* B: JSValue index (non-typed) still works — fallback path             */
    /* ------------------------------------------------------------------ */
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        JSValue r = eval_ok(ctx,
            "function g(arr, idx) { return arr[idx]; }\n"
            "var a = [7, 8, 9]; var idx = 1;\n"
            "for (var k = 0; k < 200; k++) g(a, idx);\n"
            "g(a, idx);\n"
        );
        int iv = 0;
        JS_ToInt32(ctx, &iv, r);
        JS_FreeValue(ctx, r);
        if (iv != 8) {
            fprintf(stderr, "FAIL B: expected 8, got %d\n", iv);
            exit(1);
        }
        printf("PASS B: g([7,8,9], 1) == %d (JSValue index fallback)\n", iv);

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* ------------------------------------------------------------------ */
    /* C: out-of-bounds index falls through to slow path (no crash)         */
    /* ------------------------------------------------------------------ */
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        JSValue r = eval_ok(ctx,
            "function h(arr) { return arr[999]; }\n"
            "var a = [1, 2, 3];\n"
            "for (var k = 0; k < 200; k++) h(a);\n"
            "h(a);\n"
        );
        if (!JS_IsUndefined(r)) {
            fprintf(stderr, "FAIL C: expected undefined for out-of-bounds, got tag=%d\n",
                    JS_VALUE_GET_TAG(r));
            JS_FreeValue(ctx, r);
            exit(1);
        }
        JS_FreeValue(ctx, r);
        printf("PASS C: arr[999] on 3-element array == undefined (no crash)\n");

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* ------------------------------------------------------------------ */
    /* D: index == arr.length (boundary) → undefined                        */
    /* ------------------------------------------------------------------ */
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        JSValue r = eval_ok(ctx,
            "function m(arr) { return arr[arr.length]; }\n"
            "var a = [5, 6, 7];\n"
            "for (var k = 0; k < 200; k++) m(a);\n"
            "m(a);\n"
        );
        if (!JS_IsUndefined(r)) {
            fprintf(stderr, "FAIL D: expected undefined for index==length, got tag=%d\n",
                    JS_VALUE_GET_TAG(r));
            JS_FreeValue(ctx, r);
            exit(1);
        }
        JS_FreeValue(ctx, r);
        printf("PASS D: arr[arr.length] == undefined (boundary, no crash)\n");

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    printf("ALL P38.2 TESTS PASSED\n");
    return 0;
}
