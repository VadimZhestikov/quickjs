/* P50.2 correctness: put_array_el warm-INT path produces correct results.
 *
 * After >200 warm JIT calls the warm recompile fires; for put_array_el with
 * an INT-typed value the warm code emits JS_MKVAL(JS_TAG_INT,(int32_t)_ti...)
 * directly instead of boxing via _P94_ENSURE.  Results must be identical to
 * the cold path.
 *
 * Tests:
 * 1. Write-then-read roundtrip — fill 10-element array with incrementing INTs,
 *    read back and verify; driven 400 times to guarantee warm .so is active.
 * 2. Accumulator pattern — a[0] accumulates loop counter writes;
 *    final a[0] must equal the last written value.
 * 3. Two-array scatter — fill a[] with i, fill b[] with i*i; compute
 *    dot-product-like sum; 400 iterations to pass warm threshold.
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

    /* Test 1: write-then-read roundtrip after warm recompile.
     * 400 iterations: first ~200 cold, then warm .so activates;
     * the last iteration's array must read back correctly. */
    if (run_test(rt, ctx,
        "(function() {\n"
        "    const a = new Array(10);\n"
        "    function fillCheck(a) {\n"
        "        for (let i = 0; i < 10; i++) a[i] = i + 1;\n"
        "        let s = 0;\n"
        "        for (let i = 0; i < 10; i++) s += a[i];\n"
        "        return s;\n"  /* 1+2+...+10 = 55 */
        "    }\n"
        "    let r = 0;\n"
        "    for (let k = 0; k < 400; k++) r = fillCheck(a);\n"
        "    return r;\n"
        "})();\n",
        55)) {
        fprintf(stderr, "FAIL: test 1 (write-then-read roundtrip)\n");
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    /* Test 2: accumulator — each call writes the outer loop counter into a[0].
     * After 400 calls a[0] must equal 399 (last write wins). */
    if (run_test(rt, ctx,
        "(function() {\n"
        "    const a = [0];\n"
        "    function store(a, v) { a[0] = v; }\n"
        "    for (let i = 0; i < 400; i++) store(a, i);\n"
        "    return a[0];\n"  /* 399 */
        "})();\n",
        399)) {
        fprintf(stderr, "FAIL: test 2 (accumulator)\n");
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    /* Test 3: two-array scatter then verify.
     * fill(a, b) writes a[i]=i, b[i]=i*i for i in 0..4.
     * cross-sum = sum(a[i]*b[i]) = 0+1+4*2+9*3+16*4 = 0+1+8+27+64... wait:
     * a[i]=i, b[i]=i*i → a[i]*b[i]=i*i*i.
     * sum(i^3 for i=0..4) = 0+1+8+27+64 = 100. */
    if (run_test(rt, ctx,
        "(function() {\n"
        "    const a = new Array(5);\n"
        "    const b = new Array(5);\n"
        "    function scatter(a, b) {\n"
        "        for (let i = 0; i < 5; i++) {\n"
        "            a[i] = i;\n"
        "            b[i] = i * i;\n"
        "        }\n"
        "    }\n"
        "    for (let k = 0; k < 400; k++) scatter(a, b);\n"
        "    let s = 0;\n"
        "    for (let i = 0; i < 5; i++) s += a[i] * b[i];\n"
        "    return s;\n"  /* 0^3+1^3+2^3+3^3+4^3 = 100 */
        "})();\n",
        100)) {
        fprintf(stderr, "FAIL: test 3 (two-array scatter)\n");
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("P50.2: all tests passed\n");
    return 0;
}
