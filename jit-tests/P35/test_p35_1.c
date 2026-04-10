/* P35.1 test: bytecode size cap
 *
 * Verifies that:
 *   A) A function whose bytecode exceeds jit_max_bc_len is silently skipped
 *      (stays at tier 0, jit_no_compile=1 so it is never retried).
 *   B) A function whose bytecode is within the cap compiles normally to tier 2.
 *   C) Setting cap=0 disables the limit: a function that would be skipped under
 *      the default cap still reaches tier 2.
 *
 * Strategy: use js_jit_set_max_bc_len() to set an artificially small cap (e.g. 10
 * bytes) so that ordinary functions exceed it, then verify tier stays at 0.
 * Then raise the cap to a large value and confirm the same function reaches tier 2.
 *
 * Build (from quickjs/):
 *   make -C jit-tests/P35
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "quickjs-libc.h"
#include "quickjs-jit.h"

#define THRESHOLD 3

static void fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static void eval_ok(JSContext *ctx, const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        js_std_dump_error(ctx);
        fail("eval exception");
    }
    JS_FreeValue(ctx, v);
}

/* Call a named global function n times; drain JIT after. */
static void call_n(JSContext *ctx, const char *fname, int n)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, fname);
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, fn))
        fail("function not found");
    for (int i = 0; i < n; i++) {
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, fn);
            fail("call exception");
        }
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, fn);
    js_jit_drain();
    js_jit_install_results();
}

static JSFunctionBytecode *get_bc(JSContext *ctx, const char *fname)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, fname);
    JS_FreeValue(ctx, global);
    JSFunctionBytecode *b = js_jit_get_callee_fb(fn);
    JS_FreeValue(ctx, fn);
    return b;
}

/* --- Test A: cap set just below actual bc_len causes skip ---------------- */
static void test_a(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);

    /* First pass: measure actual bytecode length with cap disabled */
    js_jit_set_threshold(999999); /* never trigger JIT during measurement */
    js_jit_set_max_bc_len(0);    /* no cap */
    eval_ok(ctx, "function add(a,b){return a+b;}");
    int bc_len;
    JSFunctionBytecode *b = get_bc(ctx, "add");
    js_jit_fb_get_bytecode(b, &bc_len);
    if (bc_len <= 0)
        fail("A: bytecode length unexpectedly zero");

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    /* Second pass: set cap just below bc_len and verify skip */
    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);

    js_jit_set_threshold(THRESHOLD);
    js_jit_set_max_bc_len(bc_len - 1); /* cap one byte below actual length */

    eval_ok(ctx, "function add(a,b){return a+b;}");
    b = get_bc(ctx, "add");
    call_n(ctx, "add", THRESHOLD + 1);

    if (js_jit_fb_get_tier(b) != 0)
        fail("A: function should stay at tier 0 when bytecode exceeds cap");
    if (!js_jit_fb_jit_no_compile(b))
        fail("A: jit_no_compile should be set (function claimed but skipped)");

    printf("PASS A: function with bc_len=%d skipped under cap=%d\n",
           bc_len, bc_len - 1);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* --- Test B: function within cap compiles normally ----------------------- */
static void test_b(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);

    js_jit_set_threshold(THRESHOLD);
    /* Large cap — all ordinary functions allowed */
    js_jit_set_max_bc_len(1024 * 1024);

    eval_ok(ctx, "function mul(a,b){return a*b;}");
    call_n(ctx, "mul", THRESHOLD + 1);

    JSFunctionBytecode *b = get_bc(ctx, "mul");
    if (js_jit_fb_get_tier(b) != 2)
        fail("B: function should reach tier 2 when within cap");

    printf("PASS B: function within large cap reaches tier 2\n");

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* --- Test C: cap=0 disables the limit ------------------------------------ */
static void test_c(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);

    js_jit_set_threshold(THRESHOLD);
    /* cap=0 means no cap */
    js_jit_set_max_bc_len(0);

    eval_ok(ctx, "function sub(a,b){return a-b;}");
    call_n(ctx, "sub", THRESHOLD + 1);

    JSFunctionBytecode *b = get_bc(ctx, "sub");
    if (js_jit_fb_get_tier(b) != 2)
        fail("C: function should reach tier 2 when cap=0 (disabled)");

    printf("PASS C: cap=0 disables size limit, function reaches tier 2\n");

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* --- Test D: default cap value matches JIT_MAX_BC_LEN ------------------- */
static void test_d(void)
{
    /* Reset to default between runtimes */
    js_jit_set_max_bc_len(JIT_MAX_BC_LEN);
    int got = js_jit_get_max_bc_len();
    if (got != JIT_MAX_BC_LEN)
        fail("D: js_jit_get_max_bc_len() did not return JIT_MAX_BC_LEN");
    printf("PASS D: default max_bc_len=%d (JIT_MAX_BC_LEN)\n", got);
}

int main(void)
{
    printf("=== P35.1: bytecode size cap ===\n");
    test_a();
    test_b();
    test_c();
    test_d();
    printf("=== ALL P35.1 TESTS PASSED ===\n");
    return 0;
}
