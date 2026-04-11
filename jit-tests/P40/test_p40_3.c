/* P40.3 correctness: put_var_ref with int-typed source.
 *
 * When the source slot is JIT_T_INT, the generated code should use
 *   JS_MKVAL(JS_TAG_INT, ...) + refcount check
 * instead of boxing via _P94_ENSURE first.
 *
 * More importantly: it must correctly free the old value (if heap-allocated)
 * before overwriting. This test verifies refcount integrity by:
 * 1. Storing a string into a closure var (refcount object)
 * 2. Then overwriting it with an int via put_var_ref (triggers int fast path
 *    if the argument is JIT_T_INT)
 * 3. Verifying no crash / corruption under the C allocator
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "quickjs-libc.h"

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    /* Test 1: overwrite a string closure var with an int.
     * If refcount handling is wrong, this produces a double-free or leak. */
    const char *src =
        "(function() {\n"
        "    let x = 'hello';\n"
        /* Define inner function that sets x to its argument */
        "    const setX = (v) => { x = v; };\n"
        "    const getX = () => x;\n"
        /* Warm up to trigger JIT for setX */
        "    for (let i = 0; i < 200; i++) setX(i);\n"
        /* Now set to string to allocate refcounted object */
        "    setX('world');\n"
        /* Overwrite with int — if P40.3 fires, must free the string */
        "    setX(99);\n"
        "    return getX();\n"
        "})();\n";

    JSValue v = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        js_std_dump_error(ctx);
        JS_FreeValue(ctx, v);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }
    int32_t val = 0;
    if (JS_ToInt32(ctx, &val, v)) {
        fprintf(stderr, "ToInt32 failed\n");
        JS_FreeValue(ctx, v);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }
    JS_FreeValue(ctx, v);
    if (val != 99) {
        fprintf(stderr, "Test 1: expected 99, got %d\n", val);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    /* Test 2: verifies that int-typed ++ result via set_var_ref is consistent.
     * The counter pattern uses set_var_ref (not put_var_ref), keeping the
     * value on stack and writing to the var_ref. */
    const char *src2 =
        "(function() {\n"
        "    let n = 0;\n"
        "    const tick = () => ++n;\n"
        "    for (let i = 0; i < 200; i++) tick();\n"
        "    tick(); tick();\n"
        "    return tick();\n"  /* should be 203 */
        "})();\n";

    JSValue v2 = JS_Eval(ctx, src2, strlen(src2), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v2)) {
        js_std_dump_error(ctx);
        JS_FreeValue(ctx, v2);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }
    int32_t val2 = 0;
    JS_ToInt32(ctx, &val2, v2);
    JS_FreeValue(ctx, v2);
    if (val2 != 203) {
        fprintf(stderr, "Test 2: expected 203, got %d\n", val2);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
