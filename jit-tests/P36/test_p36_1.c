/* P36.1 test: JIT address range registry
 *
 * Tests the jit_addr_registry: add/remove/lookup and the hooks in
 * js_jit_install_results / js_jit_free_bytecode.
 *
 * The registry internals are file-static; the only public accessor is
 * js_jit_registry_count().  We test the end-to-end behaviour by:
 *   - compiling JS modules with CONFIG_JIT=y and checking that the registry
 *     grows when functions reach tier 2
 *   - checking it shrinks when the runtime is freed
 *
 * Build (from quickjs/):
 *   make -C jit-tests/P36
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "quickjs.h"
#include "quickjs-libc.h"
#include "quickjs-jit.h"

static void fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

/* --- Test A: registry starts empty ---------------------------------------- */
static void test_a(void)
{
    int cnt = js_jit_registry_count();
    if (cnt != 0)
        fail("A: registry not empty at start");
    printf("PASS A: registry starts empty (count=%d)\n", cnt);
}

/* --- Test B: registry grows when JIT functions reach tier 2 --------------- */
static void test_b(void)
{
    /* Use a global script with inline functions + AOT compile-all.
     * COMPILE_ONLY with JS_EVAL_TYPE_GLOBAL gives us a bytecode tree
     * with all inner functions (add, mul, fib) as children of root_b. */
    const char *src =
        "function add(a,b){return a+b;}\n"
        "function mul(a,b){return a*b;}\n"
        "function fib(n){return n<=1?n:fib(n-1)+fib(n-2);}\n";

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);

    js_jit_set_aot_mode(1);
    JSValue top = JS_Eval(ctx, src, strlen(src), "<test_b>",
                          JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(top)) { js_std_dump_error(ctx); fail("B: compile failed"); }

    JSFunctionBytecode *root_b = (JSFunctionBytecode *)JS_VALUE_GET_PTR(top);
    js_jit_compile_all(ctx, root_b);
    js_jit_drain();
    js_jit_install_results();

    int cnt_after = js_jit_registry_count();
    if (cnt_after < 1)
        fail("B: registry empty after AOT compile+install");

    printf("PASS B: registry has %d entries after AOT install\n", cnt_after);

    JS_FreeValue(ctx, top);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* --- Test C: registry shrinks when runtime is freed ----------------------- */
static void test_c(void)
{
    const char *src =
        "function sq(n){return n*n;}\n"
        "function cube(n){return n*n*n;}\n";

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);

    js_jit_set_aot_mode(1);
    JSValue top = JS_Eval(ctx, src, strlen(src), "<test_c>",
                          JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(top)) { js_std_dump_error(ctx); fail("C: compile failed"); }

    js_jit_compile_all(ctx, (JSFunctionBytecode *)JS_VALUE_GET_PTR(top));
    js_jit_drain();
    js_jit_install_results();

    int cnt_after_install = js_jit_registry_count();
    if (cnt_after_install < 1)
        fail("C: registry empty after install");

    JS_FreeValue(ctx, top);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);   /* triggers js_jit_free_bytecode → jit_registry_remove */

    int cnt_after_free = js_jit_registry_count();
    if (cnt_after_free != 0)
        fail("C: registry not empty after FreeRuntime");

    printf("PASS C: registry shrank to 0 after JS_FreeRuntime (was %d)\n",
           cnt_after_install);
}

/* --- Test D: registry is empty after clean start (order matters) ---------- */
static void test_d(void)
{
    /* Run B then C: the test order within main() leaves a clean state,
     * so after test_c the count should be 0 (verified in C already).
     * Here just confirm the count is still 0 after all prior tests. */
    int cnt = js_jit_registry_count();
    if (cnt != 0)
        fail("D: registry not empty between test runs");
    printf("PASS D: registry is 0 between independent test runs\n");
}

int main(void)
{
    printf("=== P36.1: JIT address range registry ===\n");
    test_a();
    test_b();
    test_c();
    test_d();
    printf("=== ALL P36.1 TESTS PASSED ===\n");
    return 0;
}
