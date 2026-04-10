/* P34.8 test: source stripping — Function.prototype.toString() behaviour
 *
 * Three scenarios for a hybrid .so with add(a,b){return a+b}:
 *
 *   A. Default (qjsc --jit-hybrid, strip_flags=JS_STRIP_SOURCE by default)
 *      toString(add)  must contain "[native code]"
 *      toString(add)  must NOT contain "return a + b"
 *
 *   B. --keep-source (strip_flags=0)
 *      toString(add)  must contain "return a + b"   (original source preserved)
 *      toString(add)  must NOT contain "[native code]"
 *
 *   C. -s (strip_flags=JS_STRIP_DEBUG, strips everything incl. line info)
 *      toString(add)  must contain "[native code]"
 *      toString(add)  must NOT contain "return a + b"
 *
 * Each scenario:
 *   1. Writes the JS module to /tmp
 *   2. Runs qjsc with the appropriate flags to produce a hybrid C file
 *   3. Compiles that C to a CONFIG_JIT .so
 *   4. Runs `qjs -m` with a tiny inline runner that prints add.toString()
 *   5. Parses the output and checks for the expected string
 *
 * Build (from quickjs/):
 *   gcc -g -O0 -DCONFIG_JIT \
 *       -DQJS_DIR='"."' -DJIT_INCLUDE_DIR='"."' \
 *       -o /tmp/test_p34_8 jit_tests/P34/test_p34_8.c \
 *       -I. .obj/quickjs.o .obj/quickjs-jit.o .obj/quickjs-libc.o \
 *       .obj/dtoa.o .obj/libregexp.o .obj/libunicode.o .obj/cutils.o \
 *       -lm -lpthread -ldl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef QJS_DIR
#define QJS_DIR "."
#endif
#ifndef JIT_INCLUDE_DIR
#define JIT_INCLUDE_DIR "."
#endif

#define JS_SRC "export function add(a, b) { return a + b; }\n"

/* Run cmd via popen, collect all output into buf (up to bufsz-1 bytes).
 * Returns pclose() exit code. */
static int run_capture(const char *cmd, char *buf, size_t bufsz)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) { buf[0] = '\0'; return -1; }
    size_t n = fread(buf, 1, bufsz - 1, fp);
    buf[n] = '\0';
    return pclose(fp);
}

/* Generate hybrid .so for the given qjsc extra flags (e.g. "" / "--keep-source" / "-s").
 * Writes .js, runs qjsc --jit-hybrid, compiles .so.
 * Returns 0 on success, -1 on failure (prints reason to stderr). */
static int make_hybrid_so(const char *label,
                           const char *qjsc_flags,
                           const char *js_path,
                           const char *c_path,
                           const char *so_path)
{
    /* Write JS source */
    FILE *f = fopen(js_path, "w");
    if (!f) { perror("fopen"); return -1; }
    fputs(JS_SRC, f);
    fclose(f);

    /* qjsc --jit-hybrid [flags] */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "%s/qjsc %s --jit-hybrid -o %s %s 2>&1",
             QJS_DIR, qjsc_flags, c_path, js_path);
    char buf[4096];
    int rc = run_capture(cmd, buf, sizeof(buf));
    unlink(js_path);
    if (rc != 0) {
        fprintf(stderr, "FAIL [%s]: qjsc returned %d: %s\n", label, rc, buf);
        return -1;
    }

    /* gcc → .so */
    snprintf(cmd, sizeof(cmd),
             "gcc -O2 -shared -fPIC -DCONFIG_JIT -I%s -o %s %s 2>&1",
             JIT_INCLUDE_DIR, so_path, c_path);
    rc = run_capture(cmd, buf, sizeof(buf));
    unlink(c_path);
    if (rc != 0) {
        fprintf(stderr, "FAIL [%s]: gcc returned %d: %s\n", label, rc, buf);
        return -1;
    }
    return 0;
}

/* Run qjs -m, evaluating `script` with `import { add } from so_path`.
 * Returns the output of add.toString() in out_buf. */
static int get_tostring(const char *so_path, char *out_buf, size_t bufsz)
{
    char cmd[1024];
    /* Use -e with a two-statement dynamic import sequence via -m mode */
    snprintf(cmd, sizeof(cmd),
             "%s/qjs -m -e \""
             "import { add } from '%s';"
             "print(add.toString());"
             "\" 2>&1",
             QJS_DIR, so_path);
    int rc = run_capture(cmd, out_buf, bufsz);
    return rc;
}

typedef struct {
    const char *label;
    const char *qjsc_flags;
    const char *js_path;
    const char *c_path;
    const char *so_path;
    const char *must_contain;       /* substring that must appear in toString() */
    const char *must_not_contain;   /* substring that must NOT appear */
} Scenario;

int main(void)
{
    int failures = 0;

    Scenario scenarios[] = {
        {
            "A-default",
            "",               /* JS_STRIP_SOURCE by default */
            "/tmp/p34_8a.js", "/tmp/p34_8a.c", "/tmp/p34_8a.so",
            "[native code]",  "return a + b"
        },
        {
            "B-keep-source",
            "--keep-source",
            "/tmp/p34_8b.js", "/tmp/p34_8b.c", "/tmp/p34_8b.so",
            "return a + b",   "[native code]"
        },
        {
            "C-strip-debug",
            "-s",             /* JS_STRIP_DEBUG */
            "/tmp/p34_8c.js", "/tmp/p34_8c.c", "/tmp/p34_8c.so",
            "[native code]",  "return a + b"
        },
    };
    int nscenarios = (int)(sizeof(scenarios) / sizeof(scenarios[0]));

    for (int i = 0; i < nscenarios; i++) {
        Scenario *sc = &scenarios[i];
        printf("--- scenario %s ---\n", sc->label);

        if (make_hybrid_so(sc->label, sc->qjsc_flags,
                           sc->js_path, sc->c_path, sc->so_path) < 0) {
            failures++;
            continue;
        }
        printf("  hybrid .so built: OK\n");

        char ts_buf[4096];
        int rc = get_tostring(sc->so_path, ts_buf, sizeof(ts_buf));
        unlink(sc->so_path);

        if (rc != 0) {
            fprintf(stderr, "FAIL [%s]: qjs -m returned %d: %s\n",
                    sc->label, rc, ts_buf);
            failures++;
            continue;
        }

        /* Trim trailing newline for display */
        size_t tlen = strlen(ts_buf);
        while (tlen > 0 && (ts_buf[tlen-1] == '\n' || ts_buf[tlen-1] == '\r'))
            ts_buf[--tlen] = '\0';

        printf("  toString: %s\n", ts_buf);

        int ok = 1;
        if (!strstr(ts_buf, sc->must_contain)) {
            fprintf(stderr, "FAIL [%s]: toString does not contain '%s'\n",
                    sc->label, sc->must_contain);
            ok = 0;
        }
        if (strstr(ts_buf, sc->must_not_contain)) {
            fprintf(stderr, "FAIL [%s]: toString unexpectedly contains '%s'\n",
                    sc->label, sc->must_not_contain);
            ok = 0;
        }
        if (ok)
            printf("  PASS %s\n", sc->label);
        else
            failures++;
    }

    printf("\n");
    if (failures > 0) {
        fprintf(stderr, "FAIL: %d scenario(s) failed\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
