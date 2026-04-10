/* P34.2 test: js_jit_walk_bytecodes visits each bytecode exactly once
 *
 * Test A: outer function + 3 inner functions → expects 4 visits, 0 duplicates.
 * Test B: 3-level deep nesting (outer→middle→inner) → expects 3 visits, 0 duplicates.
 *
 * Build (from quickjs/):
 *   gcc -g -O0 -DCONFIG_JIT -o /tmp/test_p34_2 jit_tests/P34/test_p34_2.c \
 *       -I. .obj/quickjs.o .obj/quickjs-jit.o .obj/quickjs-libc.o \
 *       .obj/dtoa.o .obj/libregexp.o .obj/libunicode.o .obj/cutils.o \
 *       -lm -lpthread -ldl
 */
#include <stdio.h>
#include <string.h>
#include "quickjs.h"
#include "quickjs-libc.h"
#include "quickjs-jit.h"

#define MAX_VISITED 64

typedef struct {
    JSFunctionBytecode *ptrs[MAX_VISITED];
    int count;
    int duplicates;
} WalkResult;

static void walk_cb(JSFunctionBytecode *b, void *opaque)
{
    WalkResult *r = (WalkResult *)opaque;
    for (int i = 0; i < r->count; i++) {
        if (r->ptrs[i] == b) { r->duplicates++; return; }
    }
    if (r->count < MAX_VISITED)
        r->ptrs[r->count++] = b;
}

static int run_test(JSContext *ctx, const char *src,
                    int expected_count, const char *label)
{
    JSValue fn = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) { js_std_dump_error(ctx); return 1; }

    JSFunctionBytecode *b = js_jit_get_callee_fb(fn);
    JS_FreeValue(ctx, fn);
    if (!b) { fprintf(stderr, "FAIL %s: could not get bytecode\n", label); return 1; }

    WalkResult r = {{0}, 0, 0};
    js_jit_walk_bytecodes(b, walk_cb, &r);

    printf("[%s] visited=%d duplicates=%d\n", label, r.count, r.duplicates);

    int pass = (r.count == expected_count && r.duplicates == 0);
    if (!pass)
        printf("FAIL %s: expected %d visits, got %d (duplicates=%d)\n",
               label, expected_count, r.count, r.duplicates);
    else
        printf("PASS %s\n", label);
    return pass ? 0 : 1;
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);

    int failures = 0;

    /* Test A: outer + 3 siblings → 4 visits */
    failures += run_test(ctx,
        "(function outer() {\n"
        "  function inner1() { return 1; }\n"
        "  function inner2() { return 2; }\n"
        "  function inner3() { return 3; }\n"
        "  return inner1() + inner2() + inner3();\n"
        "})",
        4, "siblings");

    /* Test B: 3-level deep nesting → 3 visits */
    failures += run_test(ctx,
        "(function outer() {\n"
        "  function middle() {\n"
        "    function inner() { return 42; }\n"
        "    return inner;\n"
        "  }\n"
        "  return middle;\n"
        "})",
        3, "deep-nesting");

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return failures ? 1 : 0;
}
