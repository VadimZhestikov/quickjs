/* P34.5 test: hybrid .so installs JIT functions at tier 2
 *
 * Steps:
 *   1. Write a 3-function JS module to /tmp
 *   2. qjsc --jit-hybrid  → /tmp/p34_5_mod.c
 *   3. gcc -shared -fPIC -DCONFIG_JIT → /tmp/p34_5_mod.so
 *   4. dlopen .so (RTLD_NOW | RTLD_GLOBAL)
 *   5. call js_init_module(ctx, name) → JSModuleDef*
 *   6. walk all bytecodes inside the module
 *   7. module body (root) must have tier == 0
 *      inner functions (add, mul, fact) must have tier == 2
 *
 * Build (from quickjs/):
 *   gcc -g -O0 -DCONFIG_JIT -rdynamic \
 *       -DQJS_DIR='"."' -DJIT_INCLUDE_DIR='"."' \
 *       -o /tmp/test_p34_5 jit_tests/P34/test_p34_5.c \
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

#define JS_PATH "/tmp/p34_5_mod.js"
#define C_PATH  "/tmp/p34_5_mod.c"
#define SO_PATH "/tmp/p34_5_mod.so"

/* ------------------------------------------------------------------ */
/* Walker callback: checks tier for each bytecode                      */
/* ------------------------------------------------------------------ */
typedef struct {
    JSFunctionBytecode *root_bc;
    int tier2_count;   /* inner functions at tier 2 */
    int errors;
} TierState;

static void tier_cb(JSFunctionBytecode *b, void *opaque)
{
    TierState *s = (TierState *)opaque;
    uint8_t tier = js_jit_fb_get_tier(b);

    if (b == s->root_bc) {
        /* module body must NOT be JIT-compiled */
        if (tier != 0) {
            fprintf(stderr, "FAIL: module body has tier=%d (expected 0)\n", tier);
            s->errors++;
        }
    } else {
        /* every inner function must be at tier 2 */
        if (tier == 2) {
            s->tier2_count++;
        } else {
            fprintf(stderr, "FAIL: inner function has tier=%d (expected 2)\n", tier);
            s->errors++;
        }
    }
}

int main(void)
{
    int rc;
    char cmd[1024];

    /* ---- 1. Write test module ---- */
    FILE *fjs = fopen(JS_PATH, "w");
    if (!fjs) { perror("fopen " JS_PATH); return 1; }
    fputs("export function add(a, b)  { return a + b; }\n", fjs);
    fputs("export function mul(a, b)  { return a * b; }\n", fjs);
    fputs("export function fact(n)    { return n <= 1 ? 1 : n * fact(n - 1); }\n", fjs);
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

    /* ---- 4. Create runtime/context ---- */
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);

    /* ---- 5. dlopen .so (RTLD_GLOBAL so quickjs symbols resolve) ---- */
    void *dl = dlopen(SO_PATH, RTLD_NOW | RTLD_GLOBAL);
    if (!dl) {
        fprintf(stderr, "FAIL: dlopen: %s\n", dlerror());
        JS_FreeContext(ctx); JS_FreeRuntime(rt);
        unlink(SO_PATH);
        return 1;
    }
    printf("dlopen: OK\n");

    /* ---- 6. Call js_init_module ---- */
    typedef JSModuleDef *(*InitModFn)(JSContext *, const char *);
    InitModFn init_mod = (InitModFn)(uintptr_t)dlsym(dl, "js_init_module");
    if (!init_mod) {
        fprintf(stderr, "FAIL: dlsym js_init_module: %s\n", dlerror());
        dlclose(dl); JS_FreeContext(ctx); JS_FreeRuntime(rt);
        unlink(SO_PATH);
        return 1;
    }

    JSModuleDef *m = init_mod(ctx, SO_PATH);
    if (!m) {
        fprintf(stderr, "FAIL: js_init_module returned NULL\n");
        dlclose(dl); JS_FreeContext(ctx); JS_FreeRuntime(rt);
        unlink(SO_PATH);
        return 1;
    }
    printf("js_init_module: OK\n");

    /* ---- 7. Reconstruct JSValue and get root bytecode ---- */
    JSValue mod_val = JS_MKPTR(JS_TAG_MODULE, m);
    JSFunctionBytecode *root_bc = js_jit_module_get_bc(mod_val);
    if (!root_bc) {
        fprintf(stderr, "FAIL: js_jit_module_get_bc returned NULL\n");
        dlclose(dl); JS_FreeContext(ctx); JS_FreeRuntime(rt);
        unlink(SO_PATH);
        return 1;
    }

    /* ---- 8. Walk and check tiers ---- */
    TierState ts = { root_bc, 0, 0 };
    js_jit_walk_bytecodes(root_bc, tier_cb, &ts);

    printf("module body tier==0: %s\n", ts.errors == 0 ? "OK" : "FAIL");
    printf("inner functions at tier 2: %d\n", ts.tier2_count);

    dlclose(dl);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    unlink(SO_PATH);

    if (ts.errors > 0) {
        fprintf(stderr, "FAIL: %d tier error(s)\n", ts.errors);
        return 1;
    }
    if (ts.tier2_count < 1) {
        fprintf(stderr, "FAIL: no inner functions found at tier 2\n");
        return 1;
    }

    printf("tier check: PASS (%d function(s) at tier 2)\n", ts.tier2_count);
    printf("PASS\n");
    return 0;
}
