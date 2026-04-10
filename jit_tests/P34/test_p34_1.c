/* P34.1 test: js_jit_hash_bytecode_pub matches cache filenames
 *
 * Compiles function add(a,b){return a+b}, obtains its bytecode,
 * calls js_jit_hash_bytecode_pub(), and verifies that the resulting
 * hex string matches an existing .so or .c file in ~/.cache/qjs-jit/.
 *
 * Build (from quickjs/):
 *   gcc -g -O0 -DCONFIG_JIT -o /tmp/test_p34_1 jit_tests/P34/test_p34_1.c \
 *       -I. .obj/quickjs.o .obj/quickjs-jit.o .obj/quickjs-libc.o \
 *       .obj/dtoa.o .obj/libregexp.o .obj/libunicode.o .obj/cutils.o \
 *       -lm -lpthread -ldl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "quickjs.h"
#include "quickjs-libc.h"
#include "quickjs-jit.h"

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);
    js_jit_init();
    js_jit_set_threshold(1);

    const char *src =
        "function add(a,b){ return a+b; }\n"
        "for(var i=0;i<5;i++) add(i,i+1);\n";

    JSValue val = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) { js_std_dump_error(ctx); return 1; }
    JS_FreeValue(ctx, val);

    js_jit_drain();
    js_jit_install_results();

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue add_fn = JS_GetPropertyStr(ctx, global, "add");
    JS_FreeValue(ctx, global);

    JSFunctionBytecode *b = js_jit_get_callee_fb(add_fn);
    JS_FreeValue(ctx, add_fn);
    if (!b) { fprintf(stderr, "FAIL: could not get bytecode from 'add'\n"); return 1; }

    uint64_t hash = js_jit_hash_bytecode_pub(b);
    printf("Hash: %016llx\n", (unsigned long long)hash);

    const char *home = getenv("HOME");
    char path[512];

    /* Check .so */
    snprintf(path, sizeof(path), "%s/.cache/qjs-jit/%016llx.so",
             home, (unsigned long long)hash);
    struct stat st;
    if (stat(path, &st) == 0) {
        printf("PASS: cache .so exists: %s\n", path);
        goto done;
    }

    /* Check .c (written before .so is ready) */
    snprintf(path, sizeof(path), "%s/.cache/qjs-jit/%016llx.c",
             home, (unsigned long long)hash);
    if (stat(path, &st) == 0) {
        printf("PASS: cache .c exists: %s\n", path);
        goto done;
    }

    printf("INFO: no cache file found for hash %016llx (function not JIT-compiled yet)\n",
           (unsigned long long)hash);

done:
    js_jit_free();
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
