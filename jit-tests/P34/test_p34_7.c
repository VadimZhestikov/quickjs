/* P34.7 test: unsupported functions (eval) fall back to interpreter silently
 *
 * Module has:
 *   add(a,b)   — JIT-compilable              → tier 2 after load
 *   mul(a,b)   — JIT-compilable              → tier 2 after load
 *   evalf(s)   — uses eval, unsupported      → tier 0 after load
 *
 * Checks:
 *   A. Generated C dispatch table has exactly 2 entries (add + mul only)
 *   B. After dlopen/js_init_module:  2 inner functions at tier 2,
 *                                    1 inner function at tier 0
 *   C. js_jit_call_fb on the tier-0 function returns JS_EXCEPTION
 *      (proves it is not JIT-compiled, i.e. interpreter path is active)
 *   D. Functional: qjs -m runner imports the .so and calls add, mul, evalf
 *      — verifies interpreter fallback produces correct results
 *
 * Build (from quickjs/):
 *   gcc -g -O0 -DCONFIG_JIT -rdynamic \
 *       -DQJS_DIR='"."' -DJIT_INCLUDE_DIR='"."' \
 *       -o /tmp/test_p34_7 jit_tests/P34/test_p34_7.c \
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

#define JS_PATH     "/tmp/p34_7_mod.js"
#define C_PATH      "/tmp/p34_7_mod.c"
#define SO_PATH     "/tmp/p34_7_mod.so"
#define RUNNER_PATH "/tmp/p34_7_run.js"

/* ------------------------------------------------------------------ */
/* Walker: counts tiers and collects tier-0 inner bytecodes            */
/* ------------------------------------------------------------------ */
typedef struct {
    JSFunctionBytecode *root_bc;
    int tier2_count;
    int tier0_count;
    /* store tier-0 bytecodes to call via js_jit_call_fb later */
    JSFunctionBytecode *tier0_bcs[8];
} TierWalkState;

static void tier_walk_cb(JSFunctionBytecode *b, void *opaque)
{
    TierWalkState *s = (TierWalkState *)opaque;
    if (b == s->root_bc) return;

    uint8_t tier = js_jit_fb_get_tier(b);
    if (tier == 2) {
        s->tier2_count++;
    } else {
        if (s->tier0_count < 8)
            s->tier0_bcs[s->tier0_count] = b;
        s->tier0_count++;
    }
}

int main(void)
{
    int rc, failures = 0;
    char cmd[1024];

    /* ---- Write test module ---- */
    FILE *fjs = fopen(JS_PATH, "w");
    if (!fjs) { perror("fopen " JS_PATH); return 1; }
    fputs("export function add(a, b)  { return a + b; }\n", fjs);
    fputs("export function mul(a, b)  { return a * b; }\n", fjs);
    fputs("export function evalf(s)   { return eval(s); }\n", fjs);
    fclose(fjs);

    /* ---- A: Generate hybrid C and check dispatch table count ---- */
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

    /* Count JIT table entries in the generated C */
    snprintf(cmd, sizeof(cmd),
             "grep -c 'ULL, __jit_f_' %s 2>/dev/null", C_PATH);
    FILE *gp = popen(cmd, "r");
    int table_count = -1;
    if (gp) { if (fscanf(gp, "%d", &table_count) != 1) table_count = 0; pclose(gp); }

    printf("dispatch table entries: %d (expected 2)\n", table_count);
    if (table_count != 2) {
        fprintf(stderr, "FAIL A: expected 2 dispatch entries, got %d\n", table_count);
        failures++;
    } else {
        printf("PASS A: dispatch table excludes evalf\n");
    }

    /* ---- Compile .so ---- */
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

    /* ---- B & C: Load and check tiers ---- */
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);

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

    JSModuleDef *m = init_mod(ctx, SO_PATH);
    if (!m) {
        fprintf(stderr, "FAIL: js_init_module returned NULL\n");
        dlclose(dl); JS_FreeContext(ctx); JS_FreeRuntime(rt);
        unlink(SO_PATH);
        return 1;
    }
    printf("js_init_module: OK\n");

    JSValue mod_val = JS_MKPTR(JS_TAG_MODULE, m);
    JSFunctionBytecode *root_bc = js_jit_module_get_bc(mod_val);
    if (!root_bc) {
        fprintf(stderr, "FAIL: js_jit_module_get_bc returned NULL\n");
        dlclose(dl); JS_FreeContext(ctx); JS_FreeRuntime(rt);
        unlink(SO_PATH);
        return 1;
    }

    TierWalkState tws = { root_bc, 0, 0, {NULL} };
    js_jit_walk_bytecodes(root_bc, tier_walk_cb, &tws);

    printf("inner functions at tier 2: %d (expected 2)\n", tws.tier2_count);
    printf("inner functions at tier 0: %d (expected 1)\n", tws.tier0_count);

    if (tws.tier2_count != 2) {
        fprintf(stderr, "FAIL B: expected 2 functions at tier 2, got %d\n",
                tws.tier2_count);
        failures++;
    }
    if (tws.tier0_count != 1) {
        fprintf(stderr, "FAIL B: expected 1 function at tier 0, got %d\n",
                tws.tier0_count);
        failures++;
    }
    if (tws.tier2_count == 2 && tws.tier0_count == 1)
        printf("PASS B: tier assignments correct\n");

    /* Check C: js_jit_call_fb on tier-0 function must return exception */
    for (int i = 0; i < tws.tier0_count && i < 8; i++) {
        JSValue argv[1] = { JS_NewString(ctx, "1+1") };
        JSValue ret = js_jit_call_fb(ctx, tws.tier0_bcs[i],
                                     JS_UNDEFINED, 1, argv);
        JS_FreeValue(ctx, argv[0]);
        int is_ex = JS_IsException(ret);
        if (is_ex) {
            JS_FreeValue(ctx, JS_GetException(ctx)); /* clear and free the exception */
            printf("PASS C: tier-0 function correctly rejected by js_jit_call_fb\n");
        } else {
            fprintf(stderr, "FAIL C: js_jit_call_fb on tier-0 function did not throw\n");
            failures++;
            JS_FreeValue(ctx, ret);
        }
    }

    dlclose(dl);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    /* ---- D: Functional test via qjs -m ---- */
    FILE *fr = fopen(RUNNER_PATH, "w");
    if (!fr) { perror("fopen runner"); unlink(SO_PATH); return 1; }
    fprintf(fr, "import { add, mul, evalf } from '%s';\n", SO_PATH);
    fputs("var ok = true;\n", fr);
    fputs("if (add(3, 4) !== 7)    { print('FAIL D: add');   ok = false; }\n", fr);
    fputs("if (mul(3, 4) !== 12)   { print('FAIL D: mul');   ok = false; }\n", fr);
    fputs("if (evalf('2+3') !== 5) { print('FAIL D: evalf'); ok = false; }\n", fr);
    fputs("if (ok) print('PASS D: interpreter fallback for evalf works');\n", fr);
    fclose(fr);

    snprintf(cmd, sizeof(cmd), "%s/qjs -m %s 2>&1", QJS_DIR, RUNNER_PATH);
    FILE *rp = popen(cmd, "r");
    int pass_d = 0;
    if (rp) {
        char line[256];
        while (fgets(line, sizeof(line), rp)) {
            printf("  qjs: %s", line);
            if (strstr(line, "PASS D:")) pass_d = 1;
            if (strstr(line, "FAIL D:")) failures++;
        }
        pclose(rp);
    }
    unlink(RUNNER_PATH);
    unlink(SO_PATH);

    if (!pass_d) {
        fprintf(stderr, "FAIL D: qjs runner did not print PASS D\n");
        failures++;
    }

    if (failures > 0) {
        fprintf(stderr, "FAIL: %d check(s) failed\n", failures);
        return 1;
    }

    printf("PASS\n");
    return 0;
}
