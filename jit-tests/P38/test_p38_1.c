/* P38.1 test: refcount elision for array parameter in arr[i] loop
 *
 * Verifies that the 2-opcode look-ahead peephole (get_loc arr skips DupValue
 * when followed by get_loc i and get_array_el) does not corrupt array refcounts.
 *
 * A: arr_sum returns correct value after JIT compilation
 * B: arr still usable after many JIT calls (refcount not corrupted)
 * C: mixed-type array exercises slow path without crash
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
    printf("=== P38.1: array-obj refcount elision ===\n");

    /* ------------------------------------------------------------------ */
    /* A: arr_sum-style function returns correct value after JIT            */
    /* ------------------------------------------------------------------ */
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        /* Define function and build array, warm up to trigger JIT */
        JSValue r = eval_ok(ctx,
            "function arr_sum(arr) {\n"
            "  var s = 0; var n = arr.length;\n"
            "  for (var i = 0; i < n; i++) s += arr[i];\n"
            "  return s;\n"
            "}\n"
            "var _arr = [];\n"
            "for (var k = 0; k < 100; k++) _arr.push(k);\n"
            "for (var k = 0; k < 200; k++) arr_sum(_arr);\n"
            "arr_sum(_arr);\n"   /* return value from final call */
        );
        int iv = 0;
        JS_ToInt32(ctx, &iv, r);
        JS_FreeValue(ctx, r);
        if (iv != 4950) {
            fprintf(stderr, "FAIL A: expected 4950, got %d\n", iv);
            exit(1);
        }
        printf("PASS A: arr_sum([0..99]) == %d (correct)\n", iv);

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* ------------------------------------------------------------------ */
    /* B: arr still usable after many JIT calls (refcount integrity)        */
    /* ------------------------------------------------------------------ */
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        eval_ok(ctx,
            "function arr_sum(arr) {\n"
            "  var s = 0; var n = arr.length;\n"
            "  for (var i = 0; i < n; i++) s += arr[i];\n"
            "  return s;\n"
            "}\n"
            "var _arr = [1, 2, 3];\n"
            "for (var k = 0; k < 200; k++) arr_sum(_arr);\n"
        );

        /* If refcount was corrupted, _arr may be freed — check length */
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue arr = JS_GetPropertyStr(ctx, global, "_arr");
        if (JS_IsException(arr)) fail("B: exception getting _arr");

        JSValue len = JS_GetPropertyStr(ctx, arr, "length");
        int lv = 0;
        JS_ToInt32(ctx, &lv, len);
        JS_FreeValue(ctx, len);
        JS_FreeValue(ctx, arr);
        JS_FreeValue(ctx, global);

        if (lv != 3) {
            fprintf(stderr, "FAIL B: _arr.length expected 3, got %d (possible refcount corruption)\n", lv);
            exit(1);
        }
        printf("PASS B: arr still alive after JIT calls (length==%d)\n", lv);

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* ------------------------------------------------------------------ */
    /* C: mixed-type array exercises slow path without crash                */
    /* ------------------------------------------------------------------ */
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        js_std_add_helpers(ctx, 0, NULL);

        JSValue r = eval_ok(ctx,
            "function num_sum(arr) {\n"
            "  var s = 0;\n"
            "  for (var i = 0; i < arr.length; i++) {\n"
            "    var v = arr[i];\n"
            "    if (typeof v === 'number') s += v;\n"
            "  }\n"
            "  return s;\n"
            "}\n"
            "var _marr = [1, 'hello', 2, null, 3];\n"
            "for (var k = 0; k < 200; k++) num_sum(_marr);\n"
            "num_sum(_marr);\n"
        );
        int iv = 0;
        JS_ToInt32(ctx, &iv, r);
        JS_FreeValue(ctx, r);
        if (iv != 6) {
            fprintf(stderr, "FAIL C: expected 6, got %d\n", iv);
            exit(1);
        }
        printf("PASS C: mixed-type array sum == %d (slow path ok)\n", iv);

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    printf("ALL P38.1 TESTS PASSED\n");
    return 0;
}
