/* P50.3 correctness: put_array_el with mixed types and edge cases.
 *
 * Verifies that the warm-INT speculation for put_array_el does not corrupt
 * results when:
 *  - Values transition from INT to float (speculation deoptimises cleanly).
 *  - Values transition from INT to string (non-INT write on warm path).
 *  - The write value is a computed expression that may or may not stay in range.
 *
 * Tests:
 * 1. INT then float transition — warm up with INTs, switch to 1.5;
 *    array must contain 1.5, not a corrupted integer.
 * 2. INT then string transition — after warm recompile, writing a string must
 *    not crash and the array must contain the string.
 * 3. Large INT (still fits int32_t) — write 2^30; read back must equal 2^30.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "quickjs-libc.h"

static JSValue eval_ok(JSContext *ctx, const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { js_std_dump_error(ctx); exit(1); }
    return v;
}

int main(void)
{
    int all_pass = 1;

    /* Test 1: INT → float transition.
     * store(a, v) writes a[0] = v.  Warm up with INT v=1, then write v=1.5.
     * a[0] must be 1.5 (float), not a corrupted integer value. */
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);

        /* Warm up: 400 iterations with integer value */
        eval_ok(ctx,
            "function store(a, v) { a[0] = v; }\n"
            "var _a = [0];\n"
            "for (var i = 0; i < 400; i++) store(_a, 7);\n"
        );
        /* Now write a float */
        JSValue r = eval_ok(ctx, "store(_a, 1.5); _a[0];\n");
        double d = 0.0;
        JS_ToFloat64(ctx, &d, r);
        JS_FreeValue(ctx, r);
        if (d != 1.5) {
            fprintf(stderr, "FAIL: test 1 (INT→float): expected 1.5, got %g\n", d);
            all_pass = 0;
        } else {
            printf("  test 1 (INT→float): pass\n");
        }

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* Test 2: INT → string transition.
     * After warm recompile with INT writes, write a string; must not crash.
     * Array element must be the string "hello". */
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);

        eval_ok(ctx,
            "function store2(a, v) { a[0] = v; }\n"
            "var _b = [0];\n"
            "for (var i = 0; i < 400; i++) store2(_b, i);\n"
        );
        JSValue r = eval_ok(ctx, "store2(_b, 'hello'); _b[0];\n");
        const char *str = JS_ToCString(ctx, r);
        int ok = str && strcmp(str, "hello") == 0;
        JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, r);
        if (!ok) {
            fprintf(stderr, "FAIL: test 2 (INT→string): element not 'hello'\n");
            all_pass = 0;
        } else {
            printf("  test 2 (INT→string): pass\n");
        }

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* Test 3: large INT — 2^30 = 1073741824 fits in int32_t.
     * Write it 400 times via the warm path; read back must be exact. */
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);

        JSValue r = eval_ok(ctx,
            "(function() {\n"
            "    const a = [0];\n"
            "    const BIG = 1073741824;\n"  /* 2^30 */
            "    function storeInt(a, v) { a[0] = v; }\n"
            "    for (let i = 0; i < 400; i++) storeInt(a, BIG);\n"
            "    return a[0];\n"
            "})();\n"
        );
        int32_t val = 0;
        JS_ToInt32(ctx, &val, r);
        JS_FreeValue(ctx, r);
        if (val != 1073741824) {
            fprintf(stderr, "FAIL: test 3 (large INT): expected 1073741824, got %d\n", val);
            all_pass = 0;
        } else {
            printf("  test 3 (large INT 2^30): pass\n");
        }

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    if (!all_pass) {
        fprintf(stderr, "P50.3: FAILED\n");
        return 1;
    }
    printf("P50.3: all tests passed\n");
    return 0;
}
