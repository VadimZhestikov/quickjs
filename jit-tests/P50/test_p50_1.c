/* P50.1 correctness: put_array_el cold-path INT writes produce correct results.
 *
 * Verifies that filling an array with integer values via subscript assignment
 * works correctly both before JIT compilation (interpreter) and after JIT
 * compilation but before the warm recompile fires (cold JIT path).
 *
 * Tests:
 * 1. Sequential array fill — a[i]=i for i in 0..N; sum must match N*(N-1)/2.
 * 2. Reverse fill — a[N-1-i]=i; sum must be the same.
 * 3. Index expression — a[i*2]=i; even indices get INT values; sum correct.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "quickjs-libc.h"

static int run_test(JSRuntime *rt, JSContext *ctx, const char *src, int expected)
{
    JSValue result = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        js_std_dump_error(ctx);
        JS_FreeValue(ctx, result);
        return -1;
    }
    int32_t val;
    if (JS_ToInt32(ctx, &val, result)) {
        fprintf(stderr, "ToInt32 failed\n");
        JS_FreeValue(ctx, result);
        return -1;
    }
    JS_FreeValue(ctx, result);
    if (val != expected) {
        fprintf(stderr, "Expected %d, got %d\n", expected, val);
        return -1;
    }
    return 0;
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    /* Test 1: sequential INT fill — a[i]=i for 0..99; sum = 4950.
     * 200 outer iterations trigger JIT; result must be stable. */
    if (run_test(rt, ctx,
        "(function() {\n"
        "    function fillSeq(a, n) {\n"
        "        for (let i = 0; i < n; i++) a[i] = i;\n"
        "    }\n"
        "    const a = new Array(100);\n"
        "    for (let k = 0; k < 200; k++) fillSeq(a, 100);\n"
        "    let s = 0;\n"
        "    for (let i = 0; i < 100; i++) s += a[i];\n"
        "    return s;\n"   /* 0+1+...+99 = 4950 */
        "})();\n",
        4950)) {
        fprintf(stderr, "FAIL: test 1 (sequential fill)\n");
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    /* Test 2: reverse INT fill — a[N-1-i]=i; same elements, different order;
     * sum must still be 4950. */
    if (run_test(rt, ctx,
        "(function() {\n"
        "    function fillRev(a, n) {\n"
        "        for (let i = 0; i < n; i++) a[n - 1 - i] = i;\n"
        "    }\n"
        "    const a = new Array(100);\n"
        "    for (let k = 0; k < 200; k++) fillRev(a, 100);\n"
        "    let s = 0;\n"
        "    for (let i = 0; i < 100; i++) s += a[i];\n"
        "    return s;\n"   /* 0+1+...+99 = 4950 */
        "})();\n",
        4950)) {
        fprintf(stderr, "FAIL: test 2 (reverse fill)\n");
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    /* Test 3: even-index fill — a[i*2]=i for i in 0..49 (50 elements);
     * sum over even indices = 0+1+...+49 = 1225. */
    if (run_test(rt, ctx,
        "(function() {\n"
        "    function fillEven(a) {\n"
        "        for (let i = 0; i < 50; i++) a[i * 2] = i;\n"
        "    }\n"
        "    const a = new Array(100);\n"
        "    for (let k = 0; k < 200; k++) fillEven(a);\n"
        "    let s = 0;\n"
        "    for (let i = 0; i < 50; i++) s += a[i * 2];\n"
        "    return s;\n"   /* 0+1+...+49 = 1225 */
        "})();\n",
        1225)) {
        fprintf(stderr, "FAIL: test 3 (even-index fill)\n");
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("P50.1: all tests passed\n");
    return 0;
}
