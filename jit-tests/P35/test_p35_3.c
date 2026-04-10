/* P35.3 test: --jit-compile-all static function enumeration
 *
 * Verifies that:
 *   A) js_jit_compile_all + js_jit_drain + js_jit_install_results puts all
 *      inner functions at tier 2 BEFORE JS_EvalFunction is called.
 *   B) After installation the functions execute correctly (correct results).
 *   C) --jit-exit equivalent: after compile+install, skipping JS_EvalFunction
 *      still leaves functions at tier 2 (pure build-step path).
 *   D) P35.1 integration: size cap prevents compilation of oversized function;
 *      other functions in the same script still compile normally.
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

static void fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

/* Collect inner bytecodes (all except the root/top-level frame) */
typedef struct {
    JSFunctionBytecode  *root;
    JSFunctionBytecode **funcs;
    int                  count;
    int                  cap;
} CollectState;

static void collect_cb(JSFunctionBytecode *b, void *opaque)
{
    CollectState *st = (CollectState *)opaque;
    if (b == st->root)
        return; /* skip the top-level module wrapper */
    if (st->count >= st->cap) {
        st->cap = st->cap ? st->cap * 2 : 8;
        st->funcs = realloc(st->funcs, st->cap * sizeof(*st->funcs));
        if (!st->funcs) { perror("realloc"); exit(1); }
    }
    st->funcs[st->count++] = b;
}

/* --- Test A: functions are at tier 2 BEFORE JS_EvalFunction -------------- */
static void test_a(void)
{
    /* Three named functions at the top level of a global script */
    const char *src =
        "function add(a,b){return a+b;}\n"
        "function mul(a,b){return a*b;}\n"
        "function fib(n){return n<=1?n:fib(n-1)+fib(n-2);}\n";

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);

    /* Compile only — do NOT execute */
    JSValue top = JS_Eval(ctx, src, strlen(src), "<test_a>",
                          JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(top)) {
        js_std_dump_error(ctx);
        fail("A: compile-only eval failed");
    }

    JSFunctionBytecode *root_b =
        (JSFunctionBytecode *)JS_VALUE_GET_PTR(top);

    /* Enumerate all inner functions */
    CollectState st = { root_b, NULL, 0, 0 };
    js_jit_walk_bytecodes(root_b, collect_cb, &st);

    if (st.count < 3)
        fail("A: expected at least 3 inner bytecodes (add, mul, fib)");

    /* Verify all are at tier 0 before compilation */
    for (int i = 0; i < st.count; i++) {
        if (js_jit_fb_get_tier(st.funcs[i]) != 0)
            fail("A: unexpected tier != 0 before compile");
    }

    /* --- AOT compile step (P35.3 core) --- */
    js_jit_set_aot_mode(1);
    js_jit_compile_all(ctx, root_b);
    js_jit_drain();
    js_jit_install_results();

    /* All inner functions must now be at tier 2 */
    int all_t2 = 1;
    for (int i = 0; i < st.count; i++) {
        if (js_jit_fb_get_tier(st.funcs[i]) != 2) {
            all_t2 = 0;
            break;
        }
    }
    if (!all_t2)
        fail("A: not all inner functions reached tier 2 after install");

    printf("PASS A: %d inner functions at tier 2 before JS_EvalFunction\n",
           st.count);

    free(st.funcs);
    JS_FreeValue(ctx, top);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* --- Test B: functions execute correctly after AOT install ---------------- */
static void test_b(void)
{
    /* fib(10) = 55, add(3,4) = 7 */
    const char *setup =
        "function add(a,b){return a+b;}\n"
        "function fib(n){return n<=1?n:fib(n-1)+fib(n-2);}\n";
    const char *call_add  = "add(3,4)";
    const char *call_fib  = "fib(10)";

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);

    /* Define functions (compile + execute so they exist in the context) */
    js_jit_set_aot_mode(1);
    JSValue top = JS_Eval(ctx, setup, strlen(setup), "<test_b>",
                          JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(top)) { js_std_dump_error(ctx); fail("B: compile failed"); }

    js_jit_compile_all(ctx, (JSFunctionBytecode *)JS_VALUE_GET_PTR(top));
    js_jit_drain();
    js_jit_install_results();
    JS_EvalFunction(ctx, top); /* define add and fib in global scope */

    /* Call add(3,4) */
    JSValue res = JS_Eval(ctx, call_add, strlen(call_add), "<test_b_add>",
                          JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(res)) { js_std_dump_error(ctx); fail("B: add() threw"); }
    int add_result;
    JS_ToInt32(ctx, &add_result, res);
    JS_FreeValue(ctx, res);
    if (add_result != 7)
        fail("B: add(3,4) != 7");

    /* Call fib(10) */
    res = JS_Eval(ctx, call_fib, strlen(call_fib), "<test_b_fib>",
                  JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(res)) { js_std_dump_error(ctx); fail("B: fib() threw"); }
    int fib_result;
    JS_ToInt32(ctx, &fib_result, res);
    JS_FreeValue(ctx, res);
    if (fib_result != 55)
        fail("B: fib(10) != 55");

    printf("PASS B: add(3,4)=%d fib(10)=%d — both correct after AOT install\n",
           add_result, fib_result);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* --- Test C: --jit-exit equivalent: tier 2 without executing -------------- */
static void test_c(void)
{
    /* A function with a deliberate side-effect; in --jit-exit mode it must
     * never be called, yet the function bytecode should be at tier 2. */
    const char *src =
        "var _executed = false;\n"
        "function marker(){_executed=true;}\n"
        "function compute(n){return n*n;}\n"
        "marker();\n"; /* top-level call — must NOT run in exit mode */

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);

    js_jit_set_aot_mode(1);
    JSValue top = JS_Eval(ctx, src, strlen(src), "<test_c>",
                          JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(top)) { js_std_dump_error(ctx); fail("C: compile failed"); }

    JSFunctionBytecode *root_b =
        (JSFunctionBytecode *)JS_VALUE_GET_PTR(top);

    CollectState st = { root_b, NULL, 0, 0 };
    js_jit_walk_bytecodes(root_b, collect_cb, &st);

    js_jit_compile_all(ctx, root_b);
    js_jit_drain();
    js_jit_install_results();

    /* --jit-exit: free without executing */
    JS_FreeValue(ctx, top);

    /* _executed must NOT be set (side effect never ran) */
    JSValue check = JS_Eval(ctx, "typeof _executed", 16, "<test_c_chk>",
                            JS_EVAL_TYPE_GLOBAL);
    /* _executed is not defined because we never ran the script */
    const char *type_str = JS_ToCString(ctx, check);
    int is_undefined = type_str && strcmp(type_str, "undefined") == 0;
    JS_FreeCString(ctx, type_str);
    JS_FreeValue(ctx, check);

    if (!is_undefined)
        fail("C: side-effect ran — --jit-exit path executed the script");

    /* At least marker and compute should have been tier 2 */
    int t2_count = 0;
    for (int i = 0; i < st.count; i++) {
        if (js_jit_fb_get_tier(st.funcs[i]) == 2)
            t2_count++;
    }
    if (t2_count < 2)
        fail("C: expected at least 2 functions at tier 2 (marker + compute)");

    printf("PASS C: %d functions at tier 2; side-effect skipped (--jit-exit)\n",
           t2_count);

    free(st.funcs);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* --- Test D: P35.1 size cap integration ---------------------------------- */
static void test_d(void)
{
    /* tiny() is small enough to compile; big() bytecode is artificially capped
     * by setting a very small limit, so it must stay at tier 0. */
    const char *src =
        "function tiny(){return 1+1;}\n"
        "function big(a,b,c,d,e,f,g,h){"
        "  var x=a+b; var y=c+d; var z=e+f;"
        "  return x+y+z+g+h;"
        "}\n";

    /* First, measure big()'s bytecode length */
    JSRuntime *rt_probe = JS_NewRuntime();
    JSContext *ctx_probe = JS_NewContext(rt_probe);
    js_std_init_handlers(rt_probe);
    JSValue top_probe = JS_Eval(ctx_probe, src, strlen(src), "<probe>",
                                JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(top_probe)) {
        js_std_dump_error(ctx_probe);
        fail("D: probe compile failed");
    }
    JSFunctionBytecode *root_probe =
        (JSFunctionBytecode *)JS_VALUE_GET_PTR(top_probe);

    CollectState probe = { root_probe, NULL, 0, 0 };
    js_jit_walk_bytecodes(root_probe, collect_cb, &probe);

    /* Find max bc_len among inner functions */
    int max_len = 0;
    for (int i = 0; i < probe.count; i++) {
        int len;
        js_jit_fb_get_bytecode(probe.funcs[i], &len);
        if (len > max_len) max_len = len;
    }
    int min_len = max_len;
    for (int i = 0; i < probe.count; i++) {
        int len;
        js_jit_fb_get_bytecode(probe.funcs[i], &len);
        if (len < min_len) min_len = len;
    }
    free(probe.funcs);
    JS_FreeValue(ctx_probe, top_probe);
    JS_FreeContext(ctx_probe);
    JS_FreeRuntime(rt_probe);

    if (min_len == max_len)
        fail("D: both functions have identical bc_len — can't distinguish");

    /* Set cap: allow tiny (min_len) but reject big (max_len) */
    int cap = min_len + (max_len - min_len) / 2;  /* midpoint */
    js_jit_set_max_bc_len(cap);

    /* Real test */
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);
    js_jit_set_aot_mode(1);

    JSValue top = JS_Eval(ctx, src, strlen(src), "<test_d>",
                          JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(top)) { js_std_dump_error(ctx); fail("D: compile failed"); }
    JSFunctionBytecode *root_b =
        (JSFunctionBytecode *)JS_VALUE_GET_PTR(top);

    js_jit_compile_all(ctx, root_b);
    js_jit_drain();
    js_jit_install_results();

    CollectState st = { root_b, NULL, 0, 0 };
    js_jit_walk_bytecodes(root_b, collect_cb, &st);

    int t0_count = 0, t2_count = 0;
    for (int i = 0; i < st.count; i++) {
        int len;
        js_jit_fb_get_bytecode(st.funcs[i], &len);
        int tier = js_jit_fb_get_tier(st.funcs[i]);
        if (len <= cap && tier == 2)
            t2_count++;
        else if (len > cap && tier == 0)
            t0_count++;
    }

    if (t2_count < 1)
        fail("D: small function not at tier 2");
    if (t0_count < 1)
        fail("D: large function should stay at tier 0 due to size cap");

    printf("PASS D: size cap=%d: %d small func(s) at tier 2, "
           "%d large func(s) capped at tier 0\n", cap, t2_count, t0_count);

    free(st.funcs);
    JS_FreeValue(ctx, top);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    /* Restore default cap */
    js_jit_set_max_bc_len(JIT_MAX_BC_LEN);
}

int main(void)
{
    printf("=== P35.3: --jit-compile-all static function enumeration ===\n");
    test_a();
    test_b();
    test_c();
    test_d();
    printf("=== ALL P35.3 TESTS PASSED ===\n");
    return 0;
}
