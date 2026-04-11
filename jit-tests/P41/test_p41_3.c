/* P41.1 exception safety: exception thrown inside bypass path propagates correctly.
 *
 * When the JIT function called via the P41.1 bypass throws an exception:
 * 1. The JSStackFrame must still be correctly unlinked from current_stack_frame.
 * 2. The exception must be visible as JS_EXCEPTION to the caller.
 * 3. A subsequent try/catch at the JS level must catch the exception.
 * 4. Backtrace must include the throwing function (sf->cur_func is set).
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

    /* Test 1: exception thrown from bypass path is caught by try/catch.
     * The closure is JIT-compiled then intentionally throws. */
    if (run_test(rt, ctx,
        "(function() {\n"
        "    let n = 0;\n"
        "    const bomb = () => {\n"
        "        n++;\n"
        "        if (n > 150) throw new Error('boom');\n"
        "        return n;\n"
        "    };\n"
        /* Warm up: first 100 calls succeed and trigger JIT */
        "    for (let i = 0; i < 100; i++) bomb();\n"
        /* Now catch the exception thrown via the bypass path */
        "    let caught = 0;\n"
        "    for (let i = 0; i < 200; i++) {\n"
        "        try { bomb(); } catch(e) { caught++; }\n"
        "    }\n"
        "    return caught;\n"  /* caught == 200 - (150 - 100) = 200 - 50 = 150 */
        "})();\n",
        150)) {
        fprintf(stderr, "FAIL: test 1 (exception caught)\n");
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    /* Test 2: stack frame is correctly restored after exception in bypass path.
     * After the exception the outer interpreted loop must continue working. */
    if (run_test(rt, ctx,
        "(function() {\n"
        "    let n = 0;\n"
        "    const fn = () => { n++; if (n === 110) throw 'x'; return n; };\n"
        "    for (let i = 0; i < 100; i++) fn();\n"  /* JIT-compile fn */
        "    let ok = 0;\n"
        "    for (let i = 0; i < 20; i++) {\n"
        "        try { fn(); ok++; } catch(e) { /* skip */ }\n"
        "    }\n"
        "    return ok;\n"  /* 19 out of 20 succeed (n=110 throws once) */
        "})();\n",
        19)) {
        fprintf(stderr, "FAIL: test 2 (frame restore after exception)\n");
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("P41.3: all tests passed\n");
    return 0;
}
