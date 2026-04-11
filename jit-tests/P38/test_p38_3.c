/* P38.3 test: inline array.length fast path
 *
 * Verifies that the inlined array.length optimization (direct count field
 * read for dense arrays) returns correct results and falls back correctly
 * for non-array objects.
 *
 * A: Dense array .length returns correct count
 * B: After push, .length returns updated count
 * C: Plain object {length:42} .length returns 42 (slow path fallback)
 * D: Empty array [] .length returns 0
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
    printf("=== P38.3: inline array.length ===\n");

    /* ------------------------------------------------------------------ */
    /* A: dense array .length returns correct count                         */
    /* ------------------------------------------------------------------ */
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        JSValue r = eval_ok(ctx,
            "function f(arr) { return arr.length; }\n"
            "var a = [1, 2, 3, 4, 5];\n"
            "for (var k = 0; k < 200; k++) f(a);\n"
            "f(a);\n"
        );
        int iv = 0;
        JS_ToInt32(ctx, &iv, r);
        JS_FreeValue(ctx, r);
        if (iv != 5) {
            fprintf(stderr, "FAIL A: expected 5, got %d\n", iv);
            exit(1);
        }
        printf("PASS A: [1..5].length == %d\n", iv);

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* ------------------------------------------------------------------ */
    /* B: .length after push returns updated count                          */
    /* ------------------------------------------------------------------ */
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        JSValue r = eval_ok(ctx,
            "function g(arr) { arr.push(99); return arr.length; }\n"
            "var a = [];\n"
            "for (var k = 0; k < 200; k++) g(a);\n"
            "g(a);\n"
        );
        int iv = 0;
        JS_ToInt32(ctx, &iv, r);
        JS_FreeValue(ctx, r);
        /* 201 calls total (200 in loop + 1 final), so length = 201 */
        if (iv != 201) {
            fprintf(stderr, "FAIL B: expected 201, got %d\n", iv);
            exit(1);
        }
        printf("PASS B: arr.length after 201 pushes == %d\n", iv);

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* ------------------------------------------------------------------ */
    /* C: plain object {length:42} falls back to get_prop correctly         */
    /* ------------------------------------------------------------------ */
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        JSValue r = eval_ok(ctx,
            "function h(o) { return o.length; }\n"
            "var o = { length: 42 };\n"
            "for (var k = 0; k < 200; k++) h(o);\n"
            "h(o);\n"
        );
        int iv = 0;
        JS_ToInt32(ctx, &iv, r);
        JS_FreeValue(ctx, r);
        if (iv != 42) {
            fprintf(stderr, "FAIL C: expected 42, got %d\n", iv);
            exit(1);
        }
        printf("PASS C: {length:42}.length == %d (slow path fallback)\n", iv);

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* ------------------------------------------------------------------ */
    /* D: empty array .length returns 0                                     */
    /* ------------------------------------------------------------------ */
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        JSValue r = eval_ok(ctx,
            "function z(arr) { return arr.length; }\n"
            "var a = [];\n"
            "for (var k = 0; k < 200; k++) z(a);\n"
            "z(a);\n"
        );
        int iv = 0;
        JS_ToInt32(ctx, &iv, r);
        JS_FreeValue(ctx, r);
        if (iv != 0) {
            fprintf(stderr, "FAIL D: expected 0, got %d\n", iv);
            exit(1);
        }
        printf("PASS D: [].length == %d\n", iv);

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    printf("ALL P38.3 TESTS PASSED\n");
    return 0;
}
