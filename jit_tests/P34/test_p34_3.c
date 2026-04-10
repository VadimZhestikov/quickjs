/* P34.3 test: js_jit_gen_c_str generates valid, compilable C
 *
 * Compiles function add(a,b){return a+b}, calls js_jit_gen_c_str(),
 * verifies the symbol name contains the hash, and compiles the generated
 * C source with gcc to confirm it produces a valid .so with the expected symbol.
 *
 * Build (from quickjs/):
 *   gcc -g -O0 -DCONFIG_JIT -o /tmp/test_p34_3 jit_tests/P34/test_p34_3.c \
 *       -I. .obj/quickjs.o .obj/quickjs-jit.o .obj/quickjs-libc.o \
 *       .obj/dtoa.o .obj/libregexp.o .obj/libunicode.o .obj/cutils.o \
 *       -lm -lpthread -ldl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "quickjs.h"
#include "quickjs-libc.h"
#include "quickjs-jit.h"

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);

    const char *src = "function add(a,b){ return a+b; }\nadd(1,2);";
    JSValue val = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) { js_std_dump_error(ctx); return 1; }
    JS_FreeValue(ctx, val);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "add");
    JS_FreeValue(ctx, global);

    JSFunctionBytecode *b = js_jit_get_callee_fb(fn);
    JS_FreeValue(ctx, fn);
    if (!b) { fprintf(stderr, "FAIL: no bytecode\n"); return 1; }

    uint64_t hash = js_jit_hash_bytecode_pub(b);
    char fname[64];
    int unsupported = 0;

    char *csrc = js_jit_gen_c_str(ctx, b, hash, fname, sizeof(fname), &unsupported);
    if (!csrc) {
        fprintf(stderr, "FAIL: gen_c_str returned NULL (unsupported=%d)\n", unsupported);
        return 1;
    }

    printf("fname:      %s\n", fname);
    printf("hash:       %016llx\n", (unsigned long long)hash);
    printf("src length: %zu bytes\n", strlen(csrc));

    /* fname must contain the hash */
    char expected_hash[32];
    snprintf(expected_hash, sizeof(expected_hash), "%016llx", (unsigned long long)hash);
    if (!strstr(fname, expected_hash)) {
        fprintf(stderr, "FAIL: fname '%s' does not contain hash '%s'\n",
                fname, expected_hash);
        free(csrc);
        return 1;
    }
    printf("hash in fname: OK\n");

    /* Write C source to temp file and compile */
    char c_path[] = "/tmp/test_p34_3_XXXXXX.c";
    int fd = mkstemps(c_path, 2);
    if (fd < 0) { perror("mkstemps"); free(csrc); return 1; }
    if (write(fd, csrc, strlen(csrc)) < 0) { perror("write"); free(csrc); return 1; }
    close(fd);
    free(csrc);

    char so_path[256];
    snprintf(so_path, sizeof(so_path), "%s.so", c_path);

    /* JIT_INCLUDE_DIR is set by the Makefile to the quickjs/ source dir */
#ifndef JIT_INCLUDE_DIR
#define JIT_INCLUDE_DIR "."
#endif
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "gcc -O2 -shared -fPIC -DCONFIG_JIT -I%s -o %s %s 2>&1",
             JIT_INCLUDE_DIR, so_path, c_path);
    printf("Compiling: %s\n", c_path);
    int rc = system(cmd);
    unlink(c_path);

    if (rc != 0) {
        fprintf(stderr, "FAIL: gcc returned %d\n", rc);
        return 1;
    }
    printf("gcc compile: OK\n");

    /* Verify expected symbol in .so */
    char nm_cmd[512];
    snprintf(nm_cmd, sizeof(nm_cmd), "nm -D %s | grep -q '%s'", so_path, fname);
    rc = system(nm_cmd);
    unlink(so_path);

    if (rc != 0) {
        fprintf(stderr, "FAIL: symbol '%s' not found in .so\n", fname);
        return 1;
    }
    printf("symbol '%s' in .so: OK\n", fname);
    printf("PASS\n");

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
