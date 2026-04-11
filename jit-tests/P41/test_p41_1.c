/* P41.1 correctness: early JIT bypass in JS_CallInternal for interpreted callers.
 *
 * The bypass fires when:
 *   jit_func != NULL && var_ref_count == 0 && has_simple_parameter_list &&
 *   argc >= arg_count && func_kind == JS_FUNC_NORMAL && !(flags & COPY_ARGV)
 *
 * Verifies:
 * 1. Counter closure still returns correct incrementing values after the bypass.
 * 2. Closure over a reference-counted object (string) is refcounted correctly
 *    (no double-free or leak) through the bypass path.
 * 3. Closure with one argument still receives and uses that argument correctly.
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

    /* Test 1: integer counter via interpreted caller loop.
     * After JIT compilation the fast bypass fires on every call; the counter must
     * accumulate correctly through 200+ iterations. */
    if (run_test(rt, ctx,
        "function make_counter() {\n"
        "    let n = 0;\n"
        "    return () => ++n;\n"
        "}\n"
        "const counter = make_counter();\n"
        "for (let i = 0; i < 200; i++) counter();\n"
        "counter();\n",   /* 201st call */
        201)) {
        fprintf(stderr, "FAIL: test 1 (integer counter)\n");
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    /* Test 2: reference-counted object closure.
     * The closure captures a string array and pops elements.  Ensures that
     * the bypass path does not corrupt refcounts (no UAF, no leak). */
    if (run_test(rt, ctx,
        "(function() {\n"
        "    let arr = ['a','b','c','d','e'];\n"
        "    let pop = () => { arr.pop(); return arr.length; };\n"
        "    for (let i = 0; i < 200; i++) pop();\n"  /* JIT-compile pop */
        "    return pop();\n"                            /* length after GC-safe pops: 0 */
        "})();\n",
        0)) {
        fprintf(stderr, "FAIL: test 2 (object refcount)\n");
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    /* Test 3: one-argument closure called from interpreted loop.
     * Verifies that the bypass correctly passes argv[0] to the JIT function. */
    if (run_test(rt, ctx,
        "(function() {\n"
        "    let base = 0;\n"
        "    let add = (x) => { base += x; return base; };\n"
        "    let s = 0;\n"
        "    for (let i = 0; i < 200; i++) s = add(i);\n"
        "    return s;\n"  /* sum 0+1+...+199 = 19900 */
        "})();\n",
        19900)) {
        fprintf(stderr, "FAIL: test 3 (one-arg closure)\n");
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("P41.1: all tests passed\n");
    return 0;
}
