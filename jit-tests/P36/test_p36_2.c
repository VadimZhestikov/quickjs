/* P36.2 test: SIGPROF sampler
 *
 * Verifies that:
 *   A) js_jit_sampler_start / stop install and restore SIGPROF without crash.
 *   B) After running a JIT-compiled tight loop with the sampler active,
 *      the timed profile contains a "time_ms" field with at least one
 *      non-zero entry.
 *   C) Sampler hz() returns 0 after stop.
 *   D) Double-start is a no-op (hz is preserved).
 *
 * Build (from quickjs/):
 *   make -C jit-tests/P36
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "quickjs.h"
#include "quickjs-libc.h"
#include "quickjs-jit.h"

static void fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

/* --- Test A: start/stop does not crash ------------------------------------ */
static void test_a(void)
{
    js_jit_sampler_start(100);
    if (js_jit_sampler_hz() != 100)
        fail("A: hz should be 100 after start");
    js_jit_sampler_stop();
    if (js_jit_sampler_hz() != 0)
        fail("A: hz should be 0 after stop");
    printf("PASS A: sampler start/stop without crash\n");
}

/* --- Test B: samples accumulate in registry during JIT execution ---------- */
static void test_b(void)
{
    /* Use a global script: COMPILE_ONLY gives JS_TAG_FUNCTION_BYTECODE so
     * js_jit_compile_all receives the correct type.
     * After JS_EvalFunction, inner function bytecodes stay alive (referenced
     * by global-scope closures) until JS_FreeRuntime.
     * We verify sampling via js_jit_registry_max_samples() — a direct
     * accessor that doesn't require walking live module bytecodes. */
    const char *src =
        "function fib(n){return n<=1?n:fib(n-1)+fib(n-2);}\n"
        "var r=0; for(var i=0;i<500;i++) r+=fib(20);\n";

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

    if (js_jit_registry_count() < 1)
        fail("B: no functions in registry after AOT install");

    /* Start sampler at 1000 Hz for good coverage of a ~50ms run */
    js_jit_sampler_start(1000);

    /* JS_EvalFunction takes ownership of top */
    JSValue res = JS_EvalFunction(ctx, top);
    if (JS_IsException(res)) { js_std_dump_error(ctx); fail("B: execution failed"); }
    JS_FreeValue(ctx, res);

    js_jit_sampler_stop();

    uint32_t max_samples = js_jit_registry_max_samples();
    if (max_samples == 0)
        fail("B: no samples accumulated in registry — sampler did not fire during JIT execution");

    printf("PASS B: sampler accumulated samples (max=%u in one function)\n",
           max_samples);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* --- Test C: hz() returns 0 after stop ------------------------------------ */
static void test_c(void)
{
    js_jit_sampler_start(500);
    if (js_jit_sampler_hz() != 500)
        fail("C: hz should be 500 while running");
    js_jit_sampler_stop();
    if (js_jit_sampler_hz() != 0)
        fail("C: hz should be 0 after stop");
    printf("PASS C: js_jit_sampler_hz() is 0 after stop\n");
}

/* --- Test D: double-start is a no-op -------------------------------------- */
static void test_d(void)
{
    js_jit_sampler_start(200);
    js_jit_sampler_start(999); /* second call must be a no-op */
    int hz = js_jit_sampler_hz();
    js_jit_sampler_stop();
    if (hz != 200)
        fail("D: double-start changed hz (should be no-op)");
    printf("PASS D: double-start is a no-op (hz=%d preserved)\n", hz);
}

int main(void)
{
    printf("=== P36.2: SIGPROF sampler ===\n");
    test_a();
    test_b();
    test_c();
    test_d();
    printf("=== ALL P36.2 TESTS PASSED ===\n");
    return 0;
}
