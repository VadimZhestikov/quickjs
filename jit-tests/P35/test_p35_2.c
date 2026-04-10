/* P35.2 test: js_jit_walk_module_graph / js_jit_walk_all_modules
 *
 * Verifies that:
 *   A) js_jit_walk_all_modules finds bytecodes from a loaded module
 *      (module body + all inner functions)
 *   B) js_jit_walk_all_modules visits bytecodes from BOTH entry and imported
 *      module after a two-module load
 *   C) js_jit_walk_module_graph on a compile-only module visits its inner
 *      functions (COMPILE_ONLY gives module JSValue directly; imports unresolved
 *      so only the one module is walked)
 *   D) js_jit_walk_module_graph is a no-op for non-module JSValues
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

#define LIB_PATH    "/tmp/p35_test_lib.js"
#define ENTRY_PATH  "/tmp/p35_test_entry.js"

static void fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static void write_tmp_file(const char *path, const char *src)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(src, f);
    fclose(f);
}

static void count_cb(JSFunctionBytecode *b, void *opaque)
{
    (void)b;
    (*(int *)opaque)++;
}

/* --- Test A: js_jit_walk_all_modules finds module body + inner functions -- */
static void test_a(void)
{
    write_tmp_file(LIB_PATH,
        "export function add(a,b){return a+b;}\n"
        "export function mul(a,b){return a*b;}\n");

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, NULL, NULL);

    /* Evaluate lib module (adds it to ctx->loaded_modules) */
    uint8_t *buf;
    size_t buf_len;
    buf = js_load_file(ctx, &buf_len, LIB_PATH);
    if (!buf) fail("A: could not load lib file");
    JSValue mod = JS_Eval(ctx, (const char *)buf, buf_len, LIB_PATH,
                          JS_EVAL_TYPE_MODULE);
    js_free(ctx, buf);
    if (JS_IsException(mod)) {
        js_std_dump_error(ctx);
        fail("A: module eval failed");
    }
    JS_FreeValue(ctx, mod);
    js_std_loop(ctx);

    int count = 0;
    js_jit_walk_all_modules(ctx, count_cb, &count);

    /* lib.js: 1 module body + 2 inner functions (add, mul) = 3 */
    if (count < 3)
        fail("A: expected at least 3 bytecodes (module body + add + mul)");

    printf("PASS A: js_jit_walk_all_modules found %d bytecodes in 1 module\n",
           count);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* --- Test B: two-module load, both visited -------------------------------- */
static void test_b(void)
{
    write_tmp_file(LIB_PATH,
        "export function add(a,b){return a+b;}\n");

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, NULL, NULL);

    /* Entry module imports from lib; loading entry triggers lib load too */
    const char *src = "import {add} from '" LIB_PATH "';\n"
                      "export {add};\n";
    JSValue mod = JS_Eval(ctx, src, strlen(src), ENTRY_PATH,
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(mod)) {
        js_std_dump_error(ctx);
        fail("B: module eval failed");
    }
    JS_FreeValue(ctx, mod);
    js_std_loop(ctx);

    int count = 0;
    js_jit_walk_all_modules(ctx, count_cb, &count);

    /* entry.js: 1 module body (no inner functions, only re-exports)
     * lib.js:   1 module body + 1 inner function (add)
     * total:    >= 2 (at minimum both module bodies) */
    if (count < 2)
        fail("B: expected bytecodes from both modules");

    printf("PASS B: js_jit_walk_all_modules found %d bytecodes across 2 modules\n",
           count);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* --- Test C: js_jit_walk_module_graph on a compile-only module ------------ */
static void test_c(void)
{
    write_tmp_file(LIB_PATH,
        "export function add(a,b){return a+b;}\n"
        "export function mul(a,b){return a*b;}\n");

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    uint8_t *buf;
    size_t buf_len;
    buf = js_load_file(ctx, &buf_len, LIB_PATH);
    if (!buf) fail("C: could not load lib file");
    /* COMPILE_ONLY: returns JS_TAG_MODULE value directly */
    JSValue mod = JS_Eval(ctx, (const char *)buf, buf_len, LIB_PATH,
                          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    js_free(ctx, buf);
    if (JS_IsException(mod)) {
        js_std_dump_error(ctx);
        fail("C: compile-only eval failed");
    }

    int count = 0;
    js_jit_walk_module_graph(ctx, mod, count_cb, &count);

    /* lib.js (compile-only, no import resolution):
     * 1 module body + 2 inner functions = 3 */
    if (count < 3)
        fail("C: expected at least 3 bytecodes from compile-only module");

    printf("PASS C: js_jit_walk_module_graph found %d bytecodes\n", count);

    JS_FreeValue(ctx, mod);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* --- Test D: js_jit_walk_module_graph is a no-op for non-module input ----- */
static void test_d(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    int count = 0;
    js_jit_walk_module_graph(ctx, JS_UNDEFINED, count_cb, &count);
    js_jit_walk_module_graph(ctx, JS_NewInt32(ctx, 42), count_cb, &count);
    js_jit_walk_module_graph(ctx, JS_NULL, count_cb, &count);

    if (count != 0)
        fail("D: expected 0 bytecodes for non-module input");

    printf("PASS D: js_jit_walk_module_graph ignores non-module values\n");

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

int main(void)
{
    printf("=== P35.2: module graph walkers ===\n");
    test_a();
    test_b();
    test_c();
    test_d();
    printf("=== ALL P35.2 TESTS PASSED ===\n");
    return 0;
}
