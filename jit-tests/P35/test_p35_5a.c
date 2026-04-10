/* P35.5-A test: js_jit_write_profile call-count profile writer
 *
 * Verifies that:
 *   A) js_jit_write_profile writes valid JSON with "hash" + "calls" + "name".
 *   B) Functions called more times have higher call counts in the profile.
 *   C) Functions never called are absent from the profile (calls > 0 filter).
 *   D) js_jit_fb_get_call_count returns the same count as what js_jit_fb_inc_count
 *      accumulated (unit accessor test).
 *
 * Build (from quickjs/):
 *   make -C jit-tests/P35
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

/* Read entire file into a malloc'd buffer (caller frees). Returns NULL on error. */
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

/* --- Test A: profile JSON is written with expected fields ----------------- */
static void test_a(void)
{
    /* A module that exposes add() and mul() */
    const char *mod_src =
        "export function add(a,b){return a+b;}\n"
        "export function mul(a,b){return a*b;}\n";

    const char *call_src2 =
        "import {add,mul} from '/tmp/p35_5a_mymod.js';\n"
        "for(var i=0;i<5;i++) add(i,i);\n"
        "for(var i=0;i<3;i++) mul(i,i);\n";

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, NULL, NULL);

    /* Write module to a temp file */
    FILE *mf = fopen("/tmp/p35_5a_mymod.js", "w");
    if (!mf) fail("A: can't write module file");
    fwrite(mod_src, 1, strlen(mod_src), mf);
    fclose(mf);

    JSValue val = JS_Eval(ctx, call_src2, strlen(call_src2), "<test_a>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
        fail("A: eval failed");
    }
    JS_FreeValue(ctx, val);
    js_std_loop(ctx);

    /* Write profile */
    const char *prof_path = "/tmp/p35_5a_profile.json";
    if (js_jit_write_profile(ctx, prof_path) != 0)
        fail("A: js_jit_write_profile returned error");

    char *json = read_file(prof_path);
    if (!json)
        fail("A: profile file not created");

    /* Check basic JSON structure */
    if (!strstr(json, "\"functions\""))
        fail("A: missing 'functions' key");
    if (!strstr(json, "\"hash\""))
        fail("A: missing 'hash' field");
    if (!strstr(json, "\"calls\""))
        fail("A: missing 'calls' field");
    if (!strstr(json, "\"name\""))
        fail("A: missing 'name' field");

    printf("PASS A: profile JSON written with expected fields\n");

    free(json);
    unlink(prof_path);
    unlink("/tmp/p35_5a_mymod.js");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* --- Test B: call counts reflect actual invocation counts ----------------- */
static void test_b(void)
{
    /* Write module */
    const char *mod_src =
        "export function heavy(n){var s=0;for(var i=0;i<n;i++)s+=i;return s;}\n"
        "export function light(n){return n+1;}\n";

    FILE *mf = fopen("/tmp/p35_5b_mod.js", "w");
    if (!mf) fail("B: can't write module file");
    fwrite(mod_src, 1, strlen(mod_src), mf);
    fclose(mf);

    /* Call heavy 20 times, light 5 times */
    const char *call_src =
        "import {heavy,light} from '/tmp/p35_5b_mod.js';\n"
        "for(var i=0;i<20;i++) heavy(10);\n"
        "for(var i=0;i<5;i++) light(i);\n";

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, NULL, NULL);

    JSValue val = JS_Eval(ctx, call_src, strlen(call_src), "<test_b>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
        fail("B: eval failed");
    }
    JS_FreeValue(ctx, val);
    js_std_loop(ctx);

    const char *prof_path = "/tmp/p35_5b_profile.json";
    if (js_jit_write_profile(ctx, prof_path) != 0)
        fail("B: write_profile failed");

    char *json = read_file(prof_path);
    if (!json) fail("B: profile file missing");

    /* Check that "calls":20 appears (heavy was called 20 times) */
    if (!strstr(json, "\"calls\":20"))
        fail("B: expected calls:20 for heavy()");

    /* Check that "calls":5 appears (light was called 5 times) */
    if (!strstr(json, "\"calls\":5"))
        fail("B: expected calls:5 for light()");

    printf("PASS B: call counts match invocation counts (heavy=20, light=5)\n");

    free(json);
    unlink(prof_path);
    unlink("/tmp/p35_5b_mod.js");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* --- Test C: un-called functions are absent from profile ------------------ */
static void test_c(void)
{
    const char *mod_src =
        "export function called(n){return n;}\n"
        "export function never_called(n){return n*2;}\n";

    FILE *mf = fopen("/tmp/p35_5c_mod.js", "w");
    if (!mf) fail("C: can't write module file");
    fwrite(mod_src, 1, strlen(mod_src), mf);
    fclose(mf);

    const char *call_src =
        "import {called} from '/tmp/p35_5c_mod.js';\n"
        "called(42);\n";  /* never_called is never invoked */

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, NULL, NULL);

    JSValue val = JS_Eval(ctx, call_src, strlen(call_src), "<test_c>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
        fail("C: eval failed");
    }
    JS_FreeValue(ctx, val);
    js_std_loop(ctx);

    const char *prof_path = "/tmp/p35_5c_profile.json";
    if (js_jit_write_profile(ctx, prof_path) != 0)
        fail("C: write_profile failed");

    char *json = read_file(prof_path);
    if (!json) fail("C: profile file missing");

    /* "called" must be present */
    if (!strstr(json, "\"name\":\"called\""))
        fail("C: 'called' missing from profile");

    /* "never_called" must be absent (calls=0) */
    if (strstr(json, "never_called"))
        fail("C: 'never_called' must not appear in profile (calls=0)");

    printf("PASS C: un-called functions absent; called functions present\n");

    free(json);
    unlink(prof_path);
    unlink("/tmp/p35_5c_mod.js");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* --- Test D: js_jit_fb_get_call_count unit test --------------------------- */
static void test_d(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);

    const char *src = "function probe(){return 1;}\n";
    JSValue top = JS_Eval(ctx, src, strlen(src), "<test_d>",
                          JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(top)) { js_std_dump_error(ctx); fail("D: compile failed"); }

    JSFunctionBytecode *root_b = (JSFunctionBytecode *)JS_VALUE_GET_PTR(top);

    /* root_b itself is a valid JSFunctionBytecode — use it for the unit test */
    int c0 = js_jit_fb_get_call_count(root_b);
    if (c0 != 0)
        fail("D: initial call count should be 0");

    js_jit_fb_inc_count(root_b);
    js_jit_fb_inc_count(root_b);
    js_jit_fb_inc_count(root_b);
    int c3 = js_jit_fb_get_call_count(root_b);
    if (c3 != 3)
        fail("D: expected call count 3 after 3 increments");

    printf("PASS D: js_jit_fb_get_call_count returns %d (correct)\n", c3);

    JS_FreeValue(ctx, top);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

int main(void)
{
    printf("=== P35.5-A: js_jit_write_profile call-count profile writer ===\n");
    test_a();
    test_b();
    test_c();
    test_d();
    printf("=== ALL P35.5-A TESTS PASSED ===\n");
    return 0;
}
