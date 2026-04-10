/* P34.6 test: JIT functions are actually executed via js_jit_call_fb
 *
 * Uses three simple non-recursive functions (add, mul, sub) in a hybrid
 * .so so that direct bytecode invocation has no closure/global lookups.
 *
 * Steps:
 *   1. Write a 3-function JS module (add/mul/sub) to /tmp
 *   2. qjsc --jit-hybrid  → /tmp/p34_6_mod.c
 *   3. gcc -shared -fPIC -DCONFIG_JIT → /tmp/p34_6_mod.so
 *   4. dlopen .so (RTLD_NOW | RTLD_GLOBAL)
 *   5. js_init_module(ctx, name)  — installs tier-2 JIT pointers
 *   6. walk all inner bytecodes; call each via js_jit_call_fb(ctx, b, ...)
 *   7. verify the result set is exactly { add(10,3)=13, mul(10,3)=30,
 *                                         sub(10,3)=7  }
 *
 * Build (from quickjs/):
 *   gcc -g -O0 -DCONFIG_JIT -rdynamic \
 *       -DQJS_DIR='"."' -DJIT_INCLUDE_DIR='"."' \
 *       -o /tmp/test_p34_6 jit_tests/P34/test_p34_6.c \
 *       -I. .obj/quickjs.o .obj/quickjs-jit.o .obj/quickjs-libc.o \
 *       .obj/dtoa.o .obj/libregexp.o .obj/libunicode.o .obj/cutils.o \
 *       -lm -lpthread -ldl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include "quickjs.h"
#include "quickjs-libc.h"
#include "quickjs-jit.h"

#ifndef QJS_DIR
#define QJS_DIR "."
#endif
#ifndef JIT_INCLUDE_DIR
#define JIT_INCLUDE_DIR "."
#endif

#define JS_PATH "/tmp/p34_6_mod.js"
#define C_PATH  "/tmp/p34_6_mod.c"
#define SO_PATH "/tmp/p34_6_mod.so"

/* Expected results when calling each function with (10, 3) */
#define ARG_A   10
#define ARG_B   3
#define EXPECT_ADD  13   /* 10 + 3 */
#define EXPECT_MUL  30   /* 10 * 3 */
#define EXPECT_SUB   7   /* 10 - 3 */

/* ------------------------------------------------------------------ */
/* Walker: calls each inner function and records the int results       */
/* ------------------------------------------------------------------ */
typedef struct {
    JSContext          *ctx;
    JSFunctionBytecode *root_bc;
    int                 results[16];
    int                 result_count;
    int                 errors;
} CallState;

static void call_cb(JSFunctionBytecode *b, void *opaque)
{
    CallState *s = (CallState *)opaque;
    if (b == s->root_bc)
        return;  /* skip module body */

    JSValue argv[2];
    argv[0] = JS_NewInt32(s->ctx, ARG_A);
    argv[1] = JS_NewInt32(s->ctx, ARG_B);

    JSValue ret = js_jit_call_fb(s->ctx, b, JS_UNDEFINED, 2, argv);

    JS_FreeValue(s->ctx, argv[0]);
    JS_FreeValue(s->ctx, argv[1]);

    if (JS_IsException(ret)) {
        fprintf(stderr, "FAIL: js_jit_call_fb returned exception\n");
        js_std_dump_error(s->ctx);
        s->errors++;
        return;
    }

    int32_t ival;
    if (JS_ToInt32(s->ctx, &ival, ret) != 0) {
        fprintf(stderr, "FAIL: result is not an integer\n");
        s->errors++;
        JS_FreeValue(s->ctx, ret);
        return;
    }
    JS_FreeValue(s->ctx, ret);

    if (s->result_count < 16)
        s->results[s->result_count++] = ival;
}

/* Simple sort for small arrays */
static void isort(int *a, int n) {
    for (int i = 1; i < n; i++) {
        int key = a[i], j = i - 1;
        while (j >= 0 && a[j] > key) { a[j+1] = a[j]; j--; }
        a[j+1] = key;
    }
}

int main(void)
{
    int rc;
    char cmd[1024];

    /* ---- 1. Write test module (no recursion) ---- */
    FILE *fjs = fopen(JS_PATH, "w");
    if (!fjs) { perror("fopen " JS_PATH); return 1; }
    fputs("export function add(a, b) { return a + b; }\n", fjs);
    fputs("export function mul(a, b) { return a * b; }\n", fjs);
    fputs("export function sub(a, b) { return a - b; }\n", fjs);
    fclose(fjs);

    /* ---- 2. Generate hybrid C ---- */
    snprintf(cmd, sizeof(cmd),
             "%s/qjsc --jit-hybrid -o %s %s 2>&1",
             QJS_DIR, C_PATH, JS_PATH);
    rc = system(cmd);
    unlink(JS_PATH);
    if (rc != 0) {
        fprintf(stderr, "FAIL: qjsc --jit-hybrid returned %d\n", rc);
        return 1;
    }
    printf("hybrid C generated: OK\n");

    /* ---- 3. Compile to .so with CONFIG_JIT ---- */
    snprintf(cmd, sizeof(cmd),
             "gcc -O2 -shared -fPIC -DCONFIG_JIT -I%s -o %s %s 2>&1",
             JIT_INCLUDE_DIR, SO_PATH, C_PATH);
    rc = system(cmd);
    unlink(C_PATH);
    if (rc != 0) {
        fprintf(stderr, "FAIL: gcc returned %d\n", rc);
        return 1;
    }
    printf("hybrid .so compiled: OK\n");

    /* ---- 4. Create runtime / context ---- */
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);

    /* ---- 5. dlopen .so ---- */
    void *dl = dlopen(SO_PATH, RTLD_NOW | RTLD_GLOBAL);
    if (!dl) {
        fprintf(stderr, "FAIL: dlopen: %s\n", dlerror());
        JS_FreeContext(ctx); JS_FreeRuntime(rt);
        unlink(SO_PATH);
        return 1;
    }

    typedef JSModuleDef *(*InitModFn)(JSContext *, const char *);
    InitModFn init_mod = (InitModFn)(uintptr_t)dlsym(dl, "js_init_module");
    if (!init_mod) {
        fprintf(stderr, "FAIL: dlsym js_init_module: %s\n", dlerror());
        dlclose(dl); JS_FreeContext(ctx); JS_FreeRuntime(rt);
        unlink(SO_PATH);
        return 1;
    }

    /* ---- 6. Load module — _install_cb sets tier 2 on all inner funcs ---- */
    JSModuleDef *m = init_mod(ctx, SO_PATH);
    if (!m) {
        fprintf(stderr, "FAIL: js_init_module returned NULL\n");
        dlclose(dl); JS_FreeContext(ctx); JS_FreeRuntime(rt);
        unlink(SO_PATH);
        return 1;
    }
    printf("js_init_module: OK\n");

    JSValue mod_val  = JS_MKPTR(JS_TAG_MODULE, m);
    JSFunctionBytecode *root_bc = js_jit_module_get_bc(mod_val);
    if (!root_bc) {
        fprintf(stderr, "FAIL: js_jit_module_get_bc returned NULL\n");
        dlclose(dl); JS_FreeContext(ctx); JS_FreeRuntime(rt);
        unlink(SO_PATH);
        return 1;
    }

    /* ---- 7. Call each inner function via js_jit_call_fb ---- */
    CallState cs = { ctx, root_bc, {0}, 0, 0 };
    js_jit_walk_bytecodes(root_bc, call_cb, &cs);

    if (cs.errors > 0) {
        fprintf(stderr, "FAIL: %d call error(s)\n", cs.errors);
        dlclose(dl); JS_FreeContext(ctx); JS_FreeRuntime(rt);
        unlink(SO_PATH);
        return 1;
    }

    printf("jit_call_fb results: ");
    for (int i = 0; i < cs.result_count; i++)
        printf("%d%s", cs.results[i], i + 1 < cs.result_count ? ", " : "");
    printf("\n");

    /* Sort results and compare to expected set */
    isort(cs.results, cs.result_count);
    int expected[3] = { EXPECT_SUB, EXPECT_ADD, EXPECT_MUL };
    isort(expected, 3);  /* { 7, 13, 30 } */

    int pass = (cs.result_count == 3);
    for (int i = 0; i < 3 && pass; i++)
        pass = (cs.results[i] == expected[i]);

    dlclose(dl);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    unlink(SO_PATH);

    if (!pass) {
        fprintf(stderr, "FAIL: expected {%d,%d,%d} (sorted), got {",
                expected[0], expected[1], expected[2]);
        for (int i = 0; i < cs.result_count; i++)
            fprintf(stderr, "%d%s", cs.results[i],
                    i + 1 < cs.result_count ? "," : "");
        fprintf(stderr, "}\n");
        return 1;
    }

    printf("js_jit_call_fb: PASS (add=%d mul=%d sub=%d)\n",
           EXPECT_ADD, EXPECT_MUL, EXPECT_SUB);
    printf("PASS\n");
    return 0;
}
