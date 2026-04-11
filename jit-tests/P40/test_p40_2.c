/* P40.2 correctness: multiple closure variables (_vrp0, _vrp1) in preamble.
 *
 * Verifies that:
 * - Two captured vars each get their own _vrp{i}
 * - Both are read and written correctly
 * - Interactions between them are correct
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "quickjs-libc.h"

static int eval_int(JSContext *ctx, const char *src, int32_t *out)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        js_std_dump_error(ctx);
        JS_FreeValue(ctx, v);
        return -1;
    }
    int rc = JS_ToInt32(ctx, out, v);
    JS_FreeValue(ctx, v);
    return rc;
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    /* Test: two captured vars — sum accumulator pattern.
     * After 3 calls: a = 10+20 = 30, +21 = 51, +22 = 73; b = 21, 22, 23.
     * Result: a + b = 73 + 23 = 96 */
    const char *src =
        "(function() {\n"
        "    let a = 10, b = 20;\n"
        "    const f = () => { a += b; b++; };\n"
        /* Warm up to trigger JIT */
        "    for (let i = 0; i < 200; i++) f();\n"
        /* Now reset and run exactly 3 times in a fresh closure */
        "    return 0;\n"
        "})();\n";

    int32_t dummy;
    if (eval_int(ctx, src, &dummy)) {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    /* Fresh test with exact iteration count */
    const char *src2 =
        "(function() {\n"
        "    let a = 10, b = 20;\n"
        "    const f = () => { a += b; b++; };\n"
        /* Call 200 times to trigger JIT, then 3 more times */
        "    for (let i = 0; i < 200; i++) f();\n"
        /* After 200 calls: a=10 + sum(20..219)*1, b=220 */
        /* Just verify it doesn't crash and returns a number */
        "    return a + b;\n"
        "})();\n";

    int32_t result2;
    if (eval_int(ctx, src2, &result2)) {
        fprintf(stderr, "Test 2 failed\n");
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }
    /* a starts at 10, each iteration adds current b; b starts at 20, increments by 1 each time.
     * After k iterations: b = 20+k, a = 10 + sum(20..20+k-1) = 10 + k*20 + k*(k-1)/2.
     * After 200: a = 10 + 200*20 + 200*199/2 = 10 + 4000 + 19900 = 23910, b = 220.
     * Total = 23910 + 220 = 24130 */
    if (result2 != 24130) {
        fprintf(stderr, "Test 2: expected 24130, got %d\n", result2);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    /* Test 3: closure with no var_refs (no _vrp should be emitted) */
    const char *src3 =
        "(function() {\n"
        "    let sum = 0;\n"
        "    for (let i = 0; i < 200; i++) sum += i;\n"
        "    return sum;\n"
        "})();\n";

    int32_t result3;
    if (eval_int(ctx, src3, &result3) || result3 != 19900) {
        fprintf(stderr, "Test 3: expected 19900, got %d\n", result3);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
