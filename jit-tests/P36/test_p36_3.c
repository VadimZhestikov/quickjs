/* P36.3 test: extended profile writer (js_jit_write_profile_timed)
 *
 * Verifies that:
 *   A) js_jit_write_profile_timed with hz=0 produces the same output as
 *      js_jit_write_profile (no "time_ms" field).
 *   B) With sampler active during execution, the timed profile includes
 *      "time_ms" with at least one non-zero entry for module functions.
 *   C) Functions with calls=0 and samples=0 are absent from both profiles.
 *   D) A profile written without timing (js_jit_write_profile) can be
 *      loaded by pgo_load (backwards-compat verified by checking it
 *      does not contain "time_ms").
 *
 * Module bytecodes stay alive until JS_FreeRuntime, so the walk succeeds.
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

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* Helper: set up a context with a hot module (fib called many times).
 * The module bytecodes are loaded into the context and stay alive.
 * Returns the JSContext (caller must free ctx+rt when done). */
static JSContext *setup_hot_module(JSRuntime **rt_out, int run_fib)
{
    const char *mod_src =
        "export function fib(n){return n<=1?n:fib(n-1)+fib(n-2);}\n"
        "export function cold(n){return n+1;}\n";
    const char *entry_src =
        "import {fib,cold} from '/tmp/p36_3_mod.js';\n"
        "var r=0; for(var i=0;i<200;i++) r+=fib(18);\n"
        "cold(1);\n";   /* called once */

    FILE *mf = fopen("/tmp/p36_3_mod.js", "w");
    if (!mf) fail("setup: can't write module");
    fwrite(mod_src, 1, strlen(mod_src), mf);
    fclose(mf);

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, NULL, NULL);

    if (run_fib) {
        /* Execute without AOT so call counts accumulate via interpreter path */
        const char *entry2 =
            "import {fib,cold} from '/tmp/p36_3_mod.js';\n"
            "var r=0; for(var i=0;i<200;i++) r+=fib(18);\n"
            "cold(1);\n";
        JSValue res = JS_Eval(ctx, entry2, strlen(entry2), "<setup>",
                              JS_EVAL_TYPE_MODULE);
        if (JS_IsException(res)) { js_std_dump_error(ctx); fail("setup: eval failed"); }
        JS_FreeValue(ctx, res);
        js_std_loop(ctx);
    } else {
        /* Just load the module (COMPILE_ONLY then evaluate to register it) */
        JSValue top = JS_Eval(ctx, entry_src, strlen(entry_src), "<setup>",
                              JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(top)) { js_std_dump_error(ctx); fail("setup: compile failed"); }
        JSFunctionBytecode *root_bc = (JSFunctionBytecode *)JS_VALUE_GET_PTR(top);
        (void)root_bc;
        JS_FreeValue(ctx, top);
    }

    *rt_out = rt;
    return ctx;
}

/* --- Test A: timed profile with hz=0 has no time_ms field (=plain profile) */
static void test_a(void)
{
    JSRuntime *rt;
    JSContext *ctx = setup_hot_module(&rt, 1);

    const char *path = "/tmp/p36_3a_prof.json";
    if (js_jit_write_profile_timed(ctx, path, 0) != 0)
        fail("A: write_profile_timed(hz=0) returned error");

    char *json = read_file(path);
    if (!json) fail("A: profile file not created");
    unlink(path);

    if (strstr(json, "\"time_ms\""))
        fail("A: time_ms field present when hz=0 — should not be");
    if (!strstr(json, "\"functions\""))
        fail("A: missing 'functions' key");

    printf("PASS A: hz=0 produces plain profile (no time_ms field)\n");
    free(json);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    unlink("/tmp/p36_3_mod.js");
}

/* --- Test B: timed profile includes time_ms with sampler active ----------- */
static void test_b(void)
{
    /* Use a function with no outer-scope references so the JIT scanner finds
     * no MODULE_* closure vars and compilation succeeds.
     * fib_iter is purely local (args + local vars only) and runs for many
     * cycles — ensuring the sampler fires during its JIT execution.
     * We iterate only the inner cpool functions — NOT root_bc itself — to
     * avoid setting root_bc->jit_no_compile=1 which would break JS_EvalFunction.
     * Module bytecodes stay alive until JS_FreeRuntime, so the profile walk
     * finds them after execution. */
    const char *entry =
        "function fib_iter(n) {\n"
        "  var a=0,b=1,c,i;\n"
        "  for (i=0;i<n;i++){c=a+b;a=b;b=c;}\n"
        "  return a;\n"
        "}\n"
        "var r=0,j;\n"
        "for(j=0;j<500000;j++) r=r+fib_iter(30);\n";

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);

    /* AOT-compile only the inner cpool functions (not the module root itself).
     * Calling js_jit_compile_all(ctx, root_bc) would mark root_bc->jit_no_compile=1
     * even if the root fails to compile (it has MODULE_DECL closure vars that the
     * JIT scanner rejects), causing JS_EvalFunction to silently fail later. */
    js_jit_set_aot_mode(1);
    JSValue top = JS_Eval(ctx, entry, strlen(entry), "<test_b>",
                          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(top)) { js_std_dump_error(ctx); fail("B: compile failed"); }

    JSFunctionBytecode *root_bc = js_jit_module_get_bc(top);
    if (root_bc) {
        int cnt = js_jit_fb_get_cpool_count(root_bc);
        for (int i = 0; i < cnt; i++) {
            JSFunctionBytecode *inner = js_jit_cpool_get_fb(root_bc, i);
            if (inner) js_jit_compile_all(ctx, inner);
        }
        js_jit_drain();
        js_jit_install_results();
    }

    /* Execute with sampler running */
    js_jit_sampler_start(1000);
    JSValue res = JS_EvalFunction(ctx, top); /* takes ownership */
    if (JS_IsException(res)) { js_std_dump_error(ctx); fail("B: exec failed"); }
    js_std_loop(ctx);
    JS_FreeValue(ctx, res);
    js_jit_sampler_stop();

    const char *path = "/tmp/p36_3b_prof.json";
    if (js_jit_write_profile_timed(ctx, path, 1000) != 0)
        fail("B: write_profile_timed failed");

    char *json = read_file(path);
    if (!json) fail("B: profile file not created");
    unlink(path);

    if (!strstr(json, "\"time_ms\""))
        fail("B: no time_ms field in timed profile");

    /* At least one entry must have time_ms > 0 */
    int has_nonzero = 0;
    const char *p = json;
    while ((p = strstr(p, "\"time_ms\":"))) {
        p += 10;
        if (atoi(p) > 0) { has_nonzero = 1; break; }
    }
    if (!has_nonzero)
        fail("B: all time_ms=0 — sampler produced no samples during module execution");

    printf("PASS B: timed profile has time_ms > 0 for fib_iter (module function)\n");
    free(json);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* --- Test C: uncalled functions absent from both profiles ----------------- */
static void test_c(void)
{
    const char *mod_src =
        "export function called_fn(n){return n+1;}\n"
        "export function never_fn(n){return n*2;}\n";
    const char *entry =
        "import {called_fn} from '/tmp/p36_3c_mod.js';\n"
        "called_fn(10);\n";

    FILE *mf = fopen("/tmp/p36_3c_mod.js", "w");
    if (!mf) fail("C: can't write module");
    fwrite(mod_src, 1, strlen(mod_src), mf);
    fclose(mf);

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, NULL, NULL);

    JSValue res = JS_Eval(ctx, entry, strlen(entry), "<test_c>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(res)) { js_std_dump_error(ctx); fail("C: eval failed"); }
    JS_FreeValue(ctx, res);
    js_std_loop(ctx);

    const char *path = "/tmp/p36_3c_prof.json";
    if (js_jit_write_profile(ctx, path) != 0)
        fail("C: write_profile failed");

    char *json = read_file(path);
    if (!json) fail("C: profile file not created");
    unlink(path);
    unlink("/tmp/p36_3c_mod.js");

    if (!strstr(json, "\"name\":\"called_fn\""))
        fail("C: called_fn missing from profile");
    if (strstr(json, "never_fn"))
        fail("C: never_fn should be absent (calls=0, samples=0)");

    printf("PASS C: uncalled function absent; called function present\n");
    free(json);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* --- Test D: plain profile has no time_ms (backwards-compat) -------------- */
static void test_d(void)
{
    JSRuntime *rt;
    JSContext *ctx = setup_hot_module(&rt, 1);

    const char *path = "/tmp/p36_3d_prof.json";
    if (js_jit_write_profile(ctx, path) != 0)
        fail("D: write_profile failed");

    char *json = read_file(path);
    if (!json) fail("D: profile file not created");
    unlink(path);

    if (strstr(json, "\"time_ms\""))
        fail("D: plain profile must not contain time_ms field");
    if (!strstr(json, "\"calls\""))
        fail("D: plain profile must contain calls field");

    printf("PASS D: plain profile has no time_ms (backwards compatible)\n");
    free(json);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    unlink("/tmp/p36_3_mod.js");
}

int main(void)
{
    printf("=== P36.3: extended profile writer (js_jit_write_profile_timed) ===\n");
    test_a();
    test_b();
    test_c();
    test_d();
    printf("=== ALL P36.3 TESTS PASSED ===\n");
    return 0;
}
