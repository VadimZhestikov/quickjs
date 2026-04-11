/* P40.1 / P40.2 correctness: closure variable access via _vrp{i} preamble cache.
 *
 * Runs a closure-based counter JIT-compiled, verifying that:
 * - get_var_ref reads the correct value from pvalue
 * - set_var_ref writes and the value is visible on the next read
 * - Multiple calls accumulate correctly
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

    /* Counter: called JIT_THRESHOLD times to trigger JIT compilation */
    const char *src =
        "function make_counter() {\n"
        "    let n = 0;\n"
        "    return () => ++n;\n"
        "}\n"
        "const counter = make_counter();\n"
        /* Call enough times to trigger JIT */
        "for (let i = 0; i < 200; i++) counter();\n"
        /* Now verify result: 201st call returns 201 */
        "counter();\n";

    if (run_test(rt, ctx, src, 201)) {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    /* Second test: counter accumulates correctly after JIT */
    const char *src2 =
        "function make_counter2() {\n"
        "    let n = 0;\n"
        "    return () => ++n;\n"
        "}\n"
        "const c = make_counter2();\n"
        "for (let i = 0; i < 100; i++) c();\n"
        "c(); c(); c();\n"
        "c();\n"; /* 104th call should return 104 */

    if (run_test(rt, ctx, src2, 104)) {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
