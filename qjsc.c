/*
 * QuickJS command line compiler
 *
 * Copyright (c) 2018-2021 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "cutils.h"
#include "quickjs-libc.h"
#ifdef CONFIG_JIT
#include "quickjs-jit.h"
#endif

typedef struct {
    char *name;
    char *short_name;
    int flags;
} namelist_entry_t;

typedef struct namelist_t {
    namelist_entry_t *array;
    int count;
    int size;
} namelist_t;

typedef struct {
    const char *option_name;
    const char *init_name;
} FeatureEntry;

static namelist_t cname_list;
static namelist_t cmodule_list;
static namelist_t init_module_list;
static uint64_t feature_bitmap;
static FILE *outfile;
static BOOL byte_swap;
static BOOL dynamic_export;
static const char *c_ident_prefix = "qjsc_";

#ifdef CONFIG_JIT
/* P35.4 — --standalone: hook called by jsc_module_loader for each recursively
 * loaded JS module, so JIT bodies are collected for the full import graph.
 * Set before compilation, cleared afterwards. */
static void (*g_jit_module_hook)(JSFunctionBytecode *root_bc, void *opaque) = NULL;
static void *g_jit_module_hook_opaque = NULL;
#endif

#define FE_ALL (-1)

static const FeatureEntry feature_list[] = {
    { "date", "Date" },
    { "eval", "Eval" },
    { "string-normalize", "StringNormalize" },
    { "regexp", "RegExp" },
    { "json", "JSON" },
    { "proxy", "Proxy" },
    { "map", "MapSet" },
    { "typedarray", "TypedArrays" },
    { "promise", "Promise" },
#define FE_MODULE_LOADER 9
    { "module-loader", NULL },
    { "weakref", "WeakRef" },
};

void namelist_add(namelist_t *lp, const char *name, const char *short_name,
                  int flags)
{
    namelist_entry_t *e;
    if (lp->count == lp->size) {
        size_t newsize = lp->size + (lp->size >> 1) + 4;
        namelist_entry_t *a =
            realloc(lp->array, sizeof(lp->array[0]) * newsize);
        /* XXX: check for realloc failure */
        lp->array = a;
        lp->size = newsize;
    }
    e =  &lp->array[lp->count++];
    e->name = strdup(name);
    if (short_name)
        e->short_name = strdup(short_name);
    else
        e->short_name = NULL;
    e->flags = flags;
}

void namelist_free(namelist_t *lp)
{
    while (lp->count > 0) {
        namelist_entry_t *e = &lp->array[--lp->count];
        free(e->name);
        free(e->short_name);
    }
    free(lp->array);
    lp->array = NULL;
    lp->size = 0;
}

namelist_entry_t *namelist_find(namelist_t *lp, const char *name)
{
    int i;
    for(i = 0; i < lp->count; i++) {
        namelist_entry_t *e = &lp->array[i];
        if (!strcmp(e->name, name))
            return e;
    }
    return NULL;
}

static void get_c_name(char *buf, size_t buf_size, const char *file)
{
    const char *p, *r;
    size_t len, i;
    int c;
    char *q;

    p = strrchr(file, '/');
    if (!p)
        p = file;
    else
        p++;
    r = strrchr(p, '.');
    if (!r)
        len = strlen(p);
    else
        len = r - p;
    pstrcpy(buf, buf_size, c_ident_prefix);
    q = buf + strlen(buf);
    for(i = 0; i < len; i++) {
        c = p[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z'))) {
            c = '_';
        }
        if ((q - buf) < buf_size - 1)
            *q++ = c;
    }
    *q = '\0';
}

static void dump_hex(FILE *f, const uint8_t *buf, size_t len)
{
    size_t i, col;
    col = 0;
    for(i = 0; i < len; i++) {
        fprintf(f, " 0x%02x,", buf[i]);
        if (++col == 8) {
            fprintf(f, "\n");
            col = 0;
        }
    }
    if (col != 0)
        fprintf(f, "\n");
}

typedef enum {
    CNAME_TYPE_SCRIPT,
    CNAME_TYPE_MODULE,
    CNAME_TYPE_JSON_MODULE,
} CNameTypeEnum;

static void output_object_code(JSContext *ctx,
                               FILE *fo, JSValueConst obj, const char *c_name,
                               CNameTypeEnum c_name_type)
{
    uint8_t *out_buf;
    size_t out_buf_len;
    int flags;

    if (c_name_type == CNAME_TYPE_JSON_MODULE)
        flags = 0;
    else
        flags = JS_WRITE_OBJ_BYTECODE;
    if (byte_swap)
        flags |= JS_WRITE_OBJ_BSWAP;
    out_buf = JS_WriteObject(ctx, &out_buf_len, obj, flags);
    if (!out_buf) {
        js_std_dump_error(ctx);
        exit(1);
    }

    namelist_add(&cname_list, c_name, NULL, c_name_type);

    fprintf(fo, "const uint32_t %s_size = %u;\n\n",
            c_name, (unsigned int)out_buf_len);
    fprintf(fo, "const uint8_t %s[%u] = {\n",
            c_name, (unsigned int)out_buf_len);
    dump_hex(fo, out_buf, out_buf_len);
    fprintf(fo, "};\n\n");

    js_free(ctx, out_buf);
}

static int js_module_dummy_init(JSContext *ctx, JSModuleDef *m)
{
    /* should never be called when compiling JS code */
    abort();
}

static void find_unique_cname(char *cname, size_t cname_size)
{
    char cname1[1024];
    int suffix_num;
    size_t len, max_len;
    assert(cname_size >= 32);
    /* find a C name not matching an existing module C name by
       adding a numeric suffix */
    len = strlen(cname);
    max_len = cname_size - 16;
    if (len > max_len)
        cname[max_len] = '\0';
    suffix_num = 1;
    for(;;) {
        snprintf(cname1, sizeof(cname1), "%s_%d", cname, suffix_num);
        if (!namelist_find(&cname_list, cname1))
            break;
        suffix_num++;
    }
    pstrcpy(cname, cname_size, cname1);
}

JSModuleDef *jsc_module_loader(JSContext *ctx,
                               const char *module_name, void *opaque,
                               JSValueConst attributes)
{
    JSModuleDef *m;
    namelist_entry_t *e;

    /* check if it is a declared C or system module */
    e = namelist_find(&cmodule_list, module_name);
    if (e) {
        /* add in the static init module list */
        namelist_add(&init_module_list, e->name, e->short_name, 0);
        /* create a dummy module */
        m = JS_NewCModule(ctx, module_name, js_module_dummy_init);
    } else if (has_suffix(module_name, ".so")) {
        fprintf(stderr, "Warning: binary module '%s' will be dynamically loaded\n", module_name);
        /* create a dummy module */
        m = JS_NewCModule(ctx, module_name, js_module_dummy_init);
        /* the resulting executable will export its symbols for the
           dynamic library */
        dynamic_export = TRUE;
    } else {
        size_t buf_len;
        uint8_t *buf;
        char cname[1024];
        int res;
        
        buf = js_load_file(ctx, &buf_len, module_name);
        if (!buf) {
            JS_ThrowReferenceError(ctx, "could not load module filename '%s'",
                                   module_name);
            return NULL;
        }

        res = js_module_test_json(ctx, attributes);
        if (has_suffix(module_name, ".json") || res > 0) {
            /* compile as JSON or JSON5 depending on "type" */
            JSValue val;
            int flags;

            if (res == 2)
                flags = JS_PARSE_JSON_EXT;
            else
                flags = 0;
            val = JS_ParseJSON2(ctx, (char *)buf, buf_len, module_name, flags);
            js_free(ctx, buf);
            if (JS_IsException(val))
                return NULL;
            /* create a dummy module */
            m = JS_NewCModule(ctx, module_name, js_module_dummy_init);
            if (!m) {
                JS_FreeValue(ctx, val);
                return NULL;
            }

            get_c_name(cname, sizeof(cname), module_name);
            if (namelist_find(&cname_list, cname)) {
                find_unique_cname(cname, sizeof(cname));
            }

            /* output the module name */
            fprintf(outfile, "static const uint8_t %s_module_name[] = {\n",
                    cname);
            dump_hex(outfile, (const uint8_t *)module_name, strlen(module_name) + 1);
            fprintf(outfile, "};\n\n");

            output_object_code(ctx, outfile, val, cname, CNAME_TYPE_JSON_MODULE);
            JS_FreeValue(ctx, val);
        } else {
            JSValue func_val;

            /* compile the module */
            func_val = JS_Eval(ctx, (char *)buf, buf_len, module_name,
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
            js_free(ctx, buf);
            if (JS_IsException(func_val))
                return NULL;
            get_c_name(cname, sizeof(cname), module_name);
            if (namelist_find(&cname_list, cname)) {
                find_unique_cname(cname, sizeof(cname));
            }
            output_object_code(ctx, outfile, func_val, cname, CNAME_TYPE_MODULE);

#ifdef CONFIG_JIT
            /* P35.4: collect JIT bodies from this dependency module */
            if (g_jit_module_hook) {
                JSFunctionBytecode *root_bc = js_jit_module_get_bc(func_val);
                if (root_bc)
                    g_jit_module_hook(root_bc, g_jit_module_hook_opaque);
            }
#endif
            /* the module is already referenced, so we must free it */
            m = JS_VALUE_GET_PTR(func_val);
            JS_FreeValue(ctx, func_val);
        }
    }
    return m;
}

static void compile_file(JSContext *ctx, FILE *fo,
                         const char *filename,
                         const char *c_name1,
                         int module)
{
    uint8_t *buf;
    char c_name[1024];
    int eval_flags;
    JSValue obj;
    size_t buf_len;

    buf = js_load_file(ctx, &buf_len, filename);
    if (!buf) {
        fprintf(stderr, "Could not load '%s'\n", filename);
        exit(1);
    }
    eval_flags = JS_EVAL_FLAG_COMPILE_ONLY;
    if (module < 0) {
        module = (has_suffix(filename, ".mjs") ||
                  JS_DetectModule((const char *)buf, buf_len));
    }
    if (module)
        eval_flags |= JS_EVAL_TYPE_MODULE;
    else
        eval_flags |= JS_EVAL_TYPE_GLOBAL;
    obj = JS_Eval(ctx, (const char *)buf, buf_len, filename, eval_flags);
    if (JS_IsException(obj)) {
        js_std_dump_error(ctx);
        exit(1);
    }
    js_free(ctx, buf);
    if (c_name1) {
        pstrcpy(c_name, sizeof(c_name), c_name1);
    } else {
        get_c_name(c_name, sizeof(c_name), filename);
        if (namelist_find(&cname_list, c_name)) {
            find_unique_cname(c_name, sizeof(c_name));
        }
    }
    output_object_code(ctx, fo, obj, c_name, CNAME_TYPE_SCRIPT);
    JS_FreeValue(ctx, obj);
}

static const char main_c_template1[] =
    "int main(int argc, char **argv)\n"
    "{\n"
    "  JSRuntime *rt;\n"
    "  JSContext *ctx;\n"
    "  rt = JS_NewRuntime();\n"
    "  js_std_set_worker_new_context_func(JS_NewCustomContext);\n"
    "  js_std_init_handlers(rt);\n"
    ;

static const char main_c_template2[] =
    "  js_std_loop(ctx);\n"
    "  js_std_free_handlers(rt);\n"
    "  JS_FreeContext(ctx);\n"
    "  JS_FreeRuntime(rt);\n"
    "  return 0;\n"
    "}\n";

#define PROG_NAME "qjsc"

void help(void)
{
    printf("QuickJS Compiler version " CONFIG_VERSION "\n"
           "usage: " PROG_NAME " [options] [files]\n"
           "\n"
           "options are:\n"
           "-c          only output bytecode to a C file\n"
           "-e          output main() and bytecode to a C file (default = executable output)\n"
           "--jit-hybrid     output bytecode + JIT function bodies + js_init_module to a C file\n"
           "--jit-hybrid-app output JIT bodies for all input modules + js_init_app to a C file\n"
           "--standalone     (P35.4) produce a self-contained binary with embedded bytecodes\n"
           "                 and AOT-compiled JIT functions; no .js files needed at runtime\n"
           "--jit-max-bc=N   skip JIT for functions with bytecode > N bytes (default=32768, 0=no cap)\n"
           "--jit-pgo=<file> (P35.5) load call-count profile; skip absent functions and emit\n"
           "                 per-function #pragma GCC optimize levels (O0/O1/O2/O3)\n"
           "-o output   set the output filename\n"
           "-N cname    set the C name of the generated data\n"
           "-m          compile as Javascript module (default=autodetect)\n"
           "-D module_name         compile a dynamically loaded module or worker\n"
           "-M module_name[,cname] add initialization code for an external C module\n"
           "-x          byte swapped output\n"
           "-p prefix   set the prefix of the generated C names\n"
           "-S n        set the maximum stack size to 'n' bytes (default=%d)\n"
           "-s            strip all the debug info\n"
           "--keep-source keep the source code\n",
           JS_DEFAULT_STACK_SIZE);
#ifdef CONFIG_LTO
    {
        int i;
        printf("-flto       use link time optimization\n");
        printf("-fno-[");
        for(i = 0; i < countof(feature_list); i++) {
            if (i != 0)
                printf("|");
            printf("%s", feature_list[i].option_name);
        }
        printf("]\n"
               "            disable selected language features (smaller code size)\n");
    }
#endif
    exit(1);
}

#if defined(CONFIG_CC) && !defined(_WIN32)

int exec_cmd(char **argv)
{
    int pid, status, ret;

    pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        exit(1);
    }

    for(;;) {
        ret = waitpid(pid, &status, 0);
        if (ret == pid && WIFEXITED(status))
            break;
    }
    return WEXITSTATUS(status);
}

static int output_executable(const char *out_filename, const char *cfilename,
                             BOOL use_lto, BOOL verbose, const char *exename,
                             const char **extra_cflags)
{
    const char *argv[64];
    const char **arg, *bn_suffix, *lto_suffix;
    char libjsname[1024];
    char exe_dir[1024], inc_dir[1024], lib_dir[1024], buf[1024], *p;
    int ret;

    /* get the directory of the executable */
    pstrcpy(exe_dir, sizeof(exe_dir), exename);
    p = strrchr(exe_dir, '/');
    if (p) {
        *p = '\0';
    } else {
        pstrcpy(exe_dir, sizeof(exe_dir), ".");
    }

    /* if 'quickjs.h' is present at the same path as the executable, we
       use it as include and lib directory */
    snprintf(buf, sizeof(buf), "%s/quickjs.h", exe_dir);
    if (access(buf, R_OK) == 0) {
        pstrcpy(inc_dir, sizeof(inc_dir), exe_dir);
        pstrcpy(lib_dir, sizeof(lib_dir), exe_dir);
    } else {
        snprintf(inc_dir, sizeof(inc_dir), "%s/include/quickjs", CONFIG_PREFIX);
        snprintf(lib_dir, sizeof(lib_dir), "%s/lib/quickjs", CONFIG_PREFIX);
    }

    lto_suffix = "";
    bn_suffix = "";

    arg = argv;
    *arg++ = CONFIG_CC;
    *arg++ = "-O2";
#ifdef CONFIG_LTO
    if (use_lto) {
        *arg++ = "-flto";
        lto_suffix = ".lto";
    }
#endif
    /* XXX: use the executable path to find the includes files and
       libraries */
    *arg++ = "-D";
    *arg++ = "_GNU_SOURCE";
    if (extra_cflags) {
        const char **ep;
        for (ep = extra_cflags; *ep; ep++)
            *arg++ = *ep;
    }
    *arg++ = "-I";
    *arg++ = inc_dir;
    *arg++ = "-o";
    *arg++ = out_filename;
    if (dynamic_export)
        *arg++ = "-rdynamic";
    *arg++ = cfilename;
    snprintf(libjsname, sizeof(libjsname), "%s/libquickjs%s%s.a",
             lib_dir, bn_suffix, lto_suffix);
    *arg++ = libjsname;
    *arg++ = "-lm";
    *arg++ = "-ldl";
    *arg++ = "-lpthread";
    *arg = NULL;

    if (verbose) {
        for(arg = argv; *arg != NULL; arg++)
            printf("%s ", *arg);
        printf("\n");
    }

    ret = exec_cmd((char **)argv);
    unlink(cfilename);
    return ret;
}
#else
static int output_executable(const char *out_filename, const char *cfilename,
                             BOOL use_lto, BOOL verbose, const char *exename,
                             const char **extra_cflags)
{
    (void)extra_cflags;
    fprintf(stderr, "Executable output is not supported for this target\n");
    exit(1);
    return 0;
}
#endif

#ifdef CONFIG_JIT
/* P35.5-C: PGO profile table -----------------------------------------------
 *
 * Loaded from --jit-pgo=<file> before code generation starts.
 * Maps bc_hash → call_count so walker callbacks can:
 *   - skip functions absent from the profile or with calls == 0
 *   - emit #pragma GCC optimize at the appropriate level
 *
 * Threshold table:
 *   calls >= 10000  → O3
 *   calls >=  1000  → O2
 *   calls >=   100  → O1
 *   calls >=     1  → O0
 *   absent / 0      → skip (no C emitted)
 * --------------------------------------------------------------------------- */
typedef struct {
    uint64_t hash;
    int      calls;
    int      time_ms;  /* P36.5: 0 if not present in profile */
} PGOEntry;

static PGOEntry *g_pgo_entries = NULL;
static int       g_pgo_count   = 0;
static int       g_pgo_active  = 0; /* 1 when --jit-pgo was given */

/* Minimal JSON parser: scan for {"hash":"<16hex>","calls":<N>[,"time_ms":<M>]}
 * objects.  "time_ms" is optional (P36.5 timed profiles only). */
static void pgo_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "qjsc: --jit-pgo: cannot open '%s'\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); perror("malloc"); exit(1); }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    int cap = 64;
    g_pgo_entries = malloc(cap * sizeof(*g_pgo_entries));
    if (!g_pgo_entries) { perror("malloc"); exit(1); }
    g_pgo_count = 0;

    const char *p = buf;
    while ((p = strstr(p, "\"hash\":\""))) {
        p += 8; /* skip "hash":" */
        if (strlen(p) < 16) break;
        /* parse 16-hex hash */
        uint64_t hash = 0;
        for (int i = 0; i < 16; i++) {
            char c = p[i];
            int nibble;
            if (c >= '0' && c <= '9') nibble = c - '0';
            else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
            else { nibble = 0; }
            hash = (hash << 4) | nibble;
        }
        p += 16;

        /* Find the next "hash": to bound searches within this JSON object. */
        const char *next_hash = strstr(p, "\"hash\":\"");
        if (!next_hash) next_hash = buf + sz; /* end of file */

        /* find "calls": */
        const char *cp = strstr(p, "\"calls\":");
        if (!cp || cp >= next_hash) { p = next_hash; continue; }
        cp += 8;
        int calls = atoi(cp);

        /* P36.5: find optional "time_ms": within same object */
        int time_ms = 0;
        const char *tp = strstr(p, "\"time_ms\":");
        if (tp && tp < next_hash)
            time_ms = atoi(tp + 10);

        p = cp;

        if (g_pgo_count >= cap) {
            cap *= 2;
            g_pgo_entries = realloc(g_pgo_entries,
                                    cap * sizeof(*g_pgo_entries));
            if (!g_pgo_entries) { perror("realloc"); exit(1); }
        }
        g_pgo_entries[g_pgo_count].hash    = hash;
        g_pgo_entries[g_pgo_count].calls   = calls;
        g_pgo_entries[g_pgo_count].time_ms = time_ms;
        g_pgo_count++;
    }
    free(buf);
    g_pgo_active = 1;
}

/* Returns the PGOEntry for hash, or NULL if not in profile. */
static const PGOEntry *pgo_lookup(uint64_t hash)
{
    for (int i = 0; i < g_pgo_count; i++)
        if (g_pgo_entries[i].hash == hash)
            return &g_pgo_entries[i];
    return NULL;
}

/* P36.5: unified hotness metric.
 * Prefers time_ms (true CPU time) when available; falls back to calls.
 * Returns a synthetic "calls" value compatible with pgo_opt_level(). */
static int pgo_hotness(const PGOEntry *e)
{
    if (e->time_ms > 0) {
        if (e->time_ms >= 500) return 10000; /* → O3 */
        if (e->time_ms >=  50) return  1000; /* → O2 */
        if (e->time_ms >=   5) return   100; /* → O1 */
        return 1;                             /* → O0 */
    }
    return e->calls;
}

/* Returns "O3" / "O2" / "O1" / "O0" based on hotness value. */
static const char *pgo_opt_level(int hotness)
{
    if (hotness >= 10000) return "O3";
    if (hotness >=  1000) return "O2";
    if (hotness >=   100) return "O1";
    return "O0";
}

/* P34.4 — --jit-hybrid code generation
 *
 * Walker callback state: collects (hash, fname, c_src) for each JIT-eligible
 * bytecode while walking the compiled module's function tree.
 *
 * NOTE: the module body (root_bc itself) is excluded from JIT compilation.
 * Module body functions use module-specific opcodes (OP_define_export, etc.)
 * that the JIT does not handle.  Only inner functions (actual exported/private
 * functions) are compiled and added to the dispatch table. */
typedef struct {
    JSContext          *ctx;
    FILE               *fo;
    int                 count;    /* number of JIT functions emitted */
    int                 error;
    JSFunctionBytecode *root_bc;  /* module body — skip from JIT */
} HybridWalkState;

static void hybrid_walk_cb(JSFunctionBytecode *b, void *opaque)
{
    HybridWalkState *st = (HybridWalkState *)opaque;
    if (st->error) return;
    if (b == st->root_bc) return; /* skip module body */

    uint64_t hash = js_jit_hash_bytecode_pub(b);

    /* P35.5-C: PGO filter — P36.5: use pgo_hotness() for time_ms support */
    if (g_pgo_active) {
        const PGOEntry *pe = pgo_lookup(hash);
        if (!pe || pgo_hotness(pe) <= 0) return; /* absent or cold — skip */
    }

    char fname[64];
    int unsupported = 0;

    char *csrc = js_jit_gen_c_str(st->ctx, b, hash, fname, sizeof(fname),
                                   &unsupported);
    if (!csrc) {
        /* unsupported or OOM — skip silently; interpreter will handle it */
        return;
    }

    /* P35.5-D: per-function optimization pragma — P36.5: driven by hotness */
    if (g_pgo_active) {
        const PGOEntry *pe = pgo_lookup(hash);
        fprintf(st->fo, "#pragma GCC optimize(\"%s\")\n",
                pgo_opt_level(pgo_hotness(pe)));
    }

    /* emit the JIT function body */
    fprintf(st->fo, "%s\n", csrc);
    free(csrc);

    /* Reset to default after function */
    if (g_pgo_active)
        fprintf(st->fo, "#pragma GCC optimize(\"O2\")\n\n");

    st->count++;
}

/* Second walk to emit the dispatch table entries */
typedef struct {
    JSContext          *ctx;
    FILE               *fo;
    int                 first;
    JSFunctionBytecode *root_bc;  /* module body — skip */
} HybridTableState;

static void hybrid_table_cb(JSFunctionBytecode *b, void *opaque)
{
    HybridTableState *st = (HybridTableState *)opaque;
    if (b == st->root_bc) return; /* skip module body */

    uint64_t hash = js_jit_hash_bytecode_pub(b);
    char fname[64];
    int unsupported = 0;

    /* Re-generate just to get fname; the source was already emitted.
     * We only need the symbol name — free the source immediately. */
    char *csrc = js_jit_gen_c_str(st->ctx, b, hash, fname, sizeof(fname),
                                   &unsupported);
    if (!csrc) return; /* unsupported — not in table */
    free(csrc);

    if (!st->first) fprintf(st->fo, ",\n");
    fprintf(st->fo, "    { 0x%016llxULL, %s }",
            (unsigned long long)hash, fname);
    st->first = 0;
}

/* Generate a --jit-hybrid C file for a compiled module value.
 * fo: output file; module_val: JS_TAG_MODULE result from JS_Eval COMPILE_ONLY.
 * Stripping is already applied at compile time via JS_SetStripInfo(). */
static void output_hybrid_module(JSContext *ctx, FILE *fo, JSValue module_val)
{
    if (JS_VALUE_GET_TAG(module_val) != JS_TAG_MODULE) {
        fprintf(stderr, "qjsc: --jit-hybrid requires a JS module (-m flag)\n");
        exit(1);
    }

    /* 1. Serialize bytecode blob (stripping already applied by JS_SetStripInfo) */
    size_t bc_len;
    uint8_t *bc_buf = JS_WriteObject(ctx, &bc_len, module_val, JS_WRITE_OBJ_BYTECODE);
    if (!bc_buf) {
        js_std_dump_error(ctx);
        exit(1);
    }

    /* 2. File header */
    fprintf(fo,
        "/* Generated by qjsc --jit-hybrid.  Do not edit. */\n"
        "#include <stdint.h>\n"
        "#include \"quickjs.h\"\n"
        "#include \"quickjs-libc.h\"\n"
        "#ifdef CONFIG_JIT\n"
        "#include \"quickjs-jit.h\"\n"
        "#endif\n"
        "\n"
        "/* Serialized module bytecode */\n"
        "static const uint8_t _bc[] = {\n");
    dump_hex(fo, bc_buf, bc_len);
    fprintf(fo, "};\n");
    fprintf(fo, "static const uint32_t _bc_size = %u;\n\n",
            (unsigned int)bc_len);
    js_free(ctx, bc_buf);

    /* 3. Get the module body bytecode for walking */
    JSFunctionBytecode *root_bc = js_jit_module_get_bc(module_val);
    if (!root_bc) {
        /* Module has no JS body (e.g. empty module) — emit minimal js_init_module */
        fprintf(fo,
            "JSModuleDef *js_init_module(JSContext *ctx, const char *name)\n"
            "{\n"
            "    JSValue obj = JS_ReadObject(ctx, _bc, _bc_size,\n"
            "                               JS_READ_OBJ_BYTECODE);\n"
            "    if (JS_IsException(obj)) return NULL;\n"
            "    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(obj);\n"
            "    JS_FreeValue(ctx, obj);\n"
            "    return m;\n"
            "}\n");
        return;
    }

    /* 4. Walk and emit JIT function bodies (skip module body root_bc) */
    fprintf(fo, "#ifdef CONFIG_JIT\n\n");
    HybridWalkState wst = { ctx, fo, 0, 0, root_bc };
    js_jit_walk_bytecodes(root_bc, hybrid_walk_cb, &wst);

    if (wst.count == 0) {
        /* No eligible inner functions — emit stub without JIT table */
        fprintf(fo, "#endif /* CONFIG_JIT */\n\n");
        fprintf(fo,
            "JSModuleDef *js_init_module(JSContext *ctx, const char *name)\n"
            "{\n"
            "    JSValue obj = JS_ReadObject(ctx, _bc, _bc_size,\n"
            "                               JS_READ_OBJ_BYTECODE);\n"
            "    if (JS_IsException(obj)) return NULL;\n"
            "    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(obj);\n"
            "    JS_FreeValue(ctx, obj);\n"
            "    return m;\n"
            "}\n");
        return;
    }

    /* 5. Dispatch table */
    fprintf(fo, "static const struct {\n"
                "    uint64_t   hash;\n"
                "    JSJITFunc  func;\n"
                "} _jit_table[] = {\n");
    HybridTableState tst = { ctx, fo, 1, root_bc };
    js_jit_walk_bytecodes(root_bc, hybrid_table_cb, &tst);
    fprintf(fo, "\n};\n");
    fprintf(fo, "#define _JIT_TABLE_COUNT "
                "((int)(sizeof(_jit_table)/sizeof(_jit_table[0])))\n\n");

    /* 6. Walker callback for runtime installation */
    fprintf(fo,
        "static void _install_cb(JSFunctionBytecode *b, void *opaque)\n"
        "{\n"
        "    uint64_t h = js_jit_hash_bytecode_pub(b);\n"
        "    int i;\n"
        "    for (i = 0; i < _JIT_TABLE_COUNT; i++) {\n"
        "        if (_jit_table[i].hash == h) {\n"
        "            js_jit_fb_set_func(b, _jit_table[i].func, NULL, 2);\n"
        "            return;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "#endif /* CONFIG_JIT */\n\n");

    /* 7. js_init_module entry point */
    fprintf(fo,
        "JSModuleDef *js_init_module(JSContext *ctx, const char *name)\n"
        "{\n"
        "    JSValue obj = JS_ReadObject(ctx, _bc, _bc_size,\n"
        "                               JS_READ_OBJ_BYTECODE);\n"
        "    if (JS_IsException(obj)) return NULL;\n"
        "#ifdef CONFIG_JIT\n"
        "    {\n"
        "        JSFunctionBytecode *b = js_jit_module_get_bc(obj);\n"
        "        if (b) js_jit_walk_bytecodes(b, _install_cb, NULL);\n"
        "    }\n"
        "#endif\n"
        "    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(obj);\n"
        "    JS_FreeValue(ctx, obj);\n"
        "    return m;\n"
        "}\n");
}

/* P35.2 — --jit-hybrid-app: whole-application AOT compilation.
 *
 * Accepts one or more compiled module JSValues (from JS_EVAL_FLAG_COMPILE_ONLY).
 * Emits a single .c file containing:
 *   - JIT function bodies for all eligible inner functions across all modules
 *     (deduplicated by bc_hash; module bodies excluded; P35.1 size cap applied)
 *   - A flat dispatch table mapping bc_hash → JSJITFunc
 *   - js_init_app(ctx): calls js_jit_walk_all_modules to install all functions
 *
 * Build the resulting .c as a shared library:
 *   gcc -O3 -flto -shared -fPIC -DCONFIG_JIT -I<quickjs> -o app.so app.c
 *
 * Usage at runtime (embed in app runner or qjs --jit-aot):
 *   void *h = dlopen("./app.so", RTLD_NOW);
 *   void (*init)(JSContext *) = dlsym(h, "js_init_app");
 *   // after all modules are loaded:
 *   init(ctx);
 */

/* Per-function entry in the dispatch table (build-time collection) */
typedef struct { uint64_t hash; char fname[64]; } AppJITEntry;

typedef struct {
    JSContext   *ctx;
    FILE        *fo;
    AppJITEntry *entries;
    int          count;
    int          cap;
    /* module body bytecodes to skip (module bodies use unsupported opcodes) */
    JSFunctionBytecode **roots;
    int                  roots_count;
    int                  roots_cap;
} AppWalkState;

static void app_walk_add_root(AppWalkState *st, JSFunctionBytecode *b)
{
    if (st->roots_count == st->roots_cap) {
        int nc = st->roots_cap ? st->roots_cap * 2 : 8;
        st->roots = realloc(st->roots, (size_t)nc * sizeof(*st->roots));
        st->roots_cap = nc;
    }
    st->roots[st->roots_count++] = b;
}

static int app_walk_is_root(AppWalkState *st, JSFunctionBytecode *b)
{
    for (int i = 0; i < st->roots_count; i++)
        if (st->roots[i] == b) return 1;
    return 0;
}

static int app_walk_hash_seen(AppWalkState *st, uint64_t hash)
{
    for (int i = 0; i < st->count; i++)
        if (st->entries[i].hash == hash) return 1;
    return 0;
}

static void app_walk_bodies_cb(JSFunctionBytecode *b, void *opaque)
{
    AppWalkState *st = (AppWalkState *)opaque;
    if (app_walk_is_root(st, b)) return;

    /* P35.1: size cap */
    if (js_jit_get_max_bc_len() > 0) {
        int bc_len;
        js_jit_fb_get_bytecode(b, &bc_len);
        if (bc_len > js_jit_get_max_bc_len()) return;
    }

    uint64_t hash = js_jit_hash_bytecode_pub(b);
    if (app_walk_hash_seen(st, hash)) return; /* dedup across modules */

    /* P35.5-C: PGO filter — P36.5: use pgo_hotness() for time_ms support */
    if (g_pgo_active) {
        const PGOEntry *pe = pgo_lookup(hash);
        if (!pe || pgo_hotness(pe) <= 0) return; /* absent or cold — skip */
    }

    char fname[64];
    int unsupported = 0;
    char *csrc = js_jit_gen_c_str(st->ctx, b, hash, fname, sizeof(fname),
                                   &unsupported);
    if (!csrc) return; /* unsupported or OOM */

    /* P35.5-D: per-function optimization pragma — P36.5: driven by hotness */
    if (g_pgo_active) {
        const PGOEntry *pe = pgo_lookup(hash);
        fprintf(st->fo, "#pragma GCC optimize(\"%s\")\n",
                pgo_opt_level(pgo_hotness(pe)));
    }

    fprintf(st->fo, "%s\n", csrc);
    free(csrc);

    /* Reset to default after function */
    if (g_pgo_active)
        fprintf(st->fo, "#pragma GCC optimize(\"O2\")\n\n");

    /* Record entry for dispatch table */
    if (st->count == st->cap) {
        int nc = st->cap ? st->cap * 2 : 64;
        st->entries = realloc(st->entries, (size_t)nc * sizeof(*st->entries));
        st->cap = nc;
    }
    st->entries[st->count].hash = hash;
    snprintf(st->entries[st->count].fname, 64, "%s", fname);
    st->count++;
}

static void output_hybrid_app(JSContext *ctx, FILE *fo,
                               JSValue *mod_vals, int mod_count)
{
    fprintf(fo,
        "/* Generated by qjsc --jit-hybrid-app.  Do not edit. */\n"
        "#include <stdint.h>\n"
        "#include \"quickjs.h\"\n"
        "#include \"quickjs-libc.h\"\n"
        "#ifdef CONFIG_JIT\n"
        "#include \"quickjs-jit.h\"\n"
        "#endif\n\n");

    AppWalkState st = {ctx, fo, NULL, 0, 0, NULL, 0, 0};

    /* Collect root_bc pointers (module bodies — excluded from JIT) */
    for (int i = 0; i < mod_count; i++) {
        JSFunctionBytecode *root_bc = js_jit_module_get_bc(mod_vals[i]);
        if (root_bc)
            app_walk_add_root(&st, root_bc);
    }

    /* Walk all modules, emit JIT function bodies */
    fprintf(fo, "#ifdef CONFIG_JIT\n\n");
    for (int i = 0; i < mod_count; i++) {
        JSFunctionBytecode *root_bc = js_jit_module_get_bc(mod_vals[i]);
        if (root_bc)
            js_jit_walk_bytecodes(root_bc, app_walk_bodies_cb, &st);
    }

    if (st.count == 0) {
        fprintf(fo, "#endif /* CONFIG_JIT */\n\n");
    } else {
        /* Dispatch table */
        fprintf(fo,
            "static const struct {\n"
            "    uint64_t   hash;\n"
            "    JSJITFunc  func;\n"
            "} _jit_app_table[] = {\n");
        for (int i = 0; i < st.count; i++)
            fprintf(fo, "    { 0x%016llxULL, %s },\n",
                    (unsigned long long)st.entries[i].hash,
                    st.entries[i].fname);
        fprintf(fo, "};\n");
        fprintf(fo, "#define _JIT_APP_TABLE_COUNT "
                    "((int)(sizeof(_jit_app_table)/sizeof(_jit_app_table[0])))\n\n");

        /* Install callback */
        fprintf(fo,
            "static void _install_app_cb(JSFunctionBytecode *b, void *opaque)\n"
            "{\n"
            "    uint64_t h = js_jit_hash_bytecode_pub(b);\n"
            "    int i;\n"
            "    for (i = 0; i < _JIT_APP_TABLE_COUNT; i++) {\n"
            "        if (_jit_app_table[i].hash == h) {\n"
            "            js_jit_fb_set_func(b, _jit_app_table[i].func, NULL, 2);\n"
            "            return;\n"
            "        }\n"
            "    }\n"
            "}\n"
            "#endif /* CONFIG_JIT */\n\n");
    }

    /* js_init_app entry point — walks all loaded modules at runtime */
    fprintf(fo,
        "/*\n"
        " * js_init_app(ctx) — install pre-compiled JIT functions.\n"
        " * Call once after all modules are loaded, before first function calls.\n"
        " * Works by walking ctx->loaded_modules and matching bytecode hashes\n"
        " * against the dispatch table compiled at build time.\n"
        " */\n"
        "void js_init_app(JSContext *ctx)\n"
        "{\n"
        "#ifdef CONFIG_JIT\n");
    if (st.count > 0)
        fprintf(fo, "    js_jit_walk_all_modules(ctx, _install_app_cb, NULL);\n");
    else
        fprintf(fo, "    (void)ctx; /* no JIT functions compiled */\n");
    fprintf(fo,
        "#else\n"
        "    (void)ctx;\n"
        "#endif\n"
        "}\n");

    free(st.entries);
    free(st.roots);
}

/* P35.4 — standalone: walker callback installed as g_jit_module_hook so that
 * dependency modules recursively loaded by jsc_module_loader also contribute
 * JIT function bodies to the AppWalkState. */
static void standalone_module_walk(JSFunctionBytecode *root_bc, void *opaque)
{
    AppWalkState *st = (AppWalkState *)opaque;
    app_walk_add_root(st, root_bc);
    js_jit_walk_bytecodes(root_bc, app_walk_bodies_cb, st);
}
#endif /* CONFIG_JIT */

static size_t get_suffixed_size(const char *str)
{
    char *p;
    size_t v;
    v = (size_t)strtod(str, &p);
    switch(*p) {
    case 'G':
        v <<= 30;
        break;
    case 'M':
        v <<= 20;
        break;
    case 'k':
    case 'K':
        v <<= 10;
        break;
    default:
        if (*p != '\0') {
            fprintf(stderr, "qjs: invalid suffix: %s\n", p);
            exit(1);
        }
        break;
    }
    return v;
}

typedef enum {
    OUTPUT_C,
    OUTPUT_C_MAIN,
    OUTPUT_EXECUTABLE,
    OUTPUT_C_HYBRID,      /* P34.4: bytecode + JIT bodies + js_init_module */
    OUTPUT_C_HYBRID_APP,  /* P35.2: all-modules JIT AOT; js_init_app entry point */
    OUTPUT_STANDALONE,    /* P35.4: self-contained binary (bytecodes + JIT + main) */
} OutputTypeEnum;

static const char *get_short_optarg(int *poptind, int opt,
                                    const char *arg, int argc, char **argv)
{
    const char *optarg;
    if (*arg) {
        optarg = arg;
    } else if (*poptind < argc) {
        optarg = argv[(*poptind)++];
    } else {
        fprintf(stderr, "qjsc: expecting parameter for -%c\n", opt);
        exit(1);
    }
    return optarg;
}

int main(int argc, char **argv)
{
    int i, verbose, strip_flags;
    const char *out_filename, *cname;
    char cfilename[1024];
    FILE *fo;
    JSRuntime *rt;
    JSContext *ctx;
    BOOL use_lto;
    int module;
    OutputTypeEnum output_type;
    size_t stack_size;
    namelist_t dynamic_module_list;

    out_filename = NULL;
    output_type = OUTPUT_EXECUTABLE;
    cname = NULL;
    feature_bitmap = FE_ALL;
    module = -1;
    byte_swap = FALSE;
    verbose = 0;
    strip_flags = JS_STRIP_SOURCE;
    use_lto = FALSE;
    stack_size = 0;
    memset(&dynamic_module_list, 0, sizeof(dynamic_module_list));

    /* add system modules */
    namelist_add(&cmodule_list, "std", "std", 0);
    namelist_add(&cmodule_list, "os", "os", 0);

    optind = 1;
    while (optind < argc && *argv[optind] == '-') {
        char *arg = argv[optind] + 1;
        const char *longopt = "";
        const char *optarg;
        /* a single - is not an option, it also stops argument scanning */
        if (!*arg)
            break;
        optind++;
        if (*arg == '-') {
            longopt = arg + 1;
            arg += strlen(arg);
            /* -- stops argument scanning */
            if (!*longopt)
                break;
        }
        for (; *arg || *longopt; longopt = "") {
            char opt = *arg;
            if (opt)
                arg++;
            if (opt == 'h' || opt == '?' || !strcmp(longopt, "help")) {
                help();
                continue;
            }
            if (opt == 'o') {
                out_filename = get_short_optarg(&optind, opt, arg, argc, argv);
                break;
            }
            if (opt == 'c') {
                output_type = OUTPUT_C;
                continue;
            }
            if (opt == 'e') {
                output_type = OUTPUT_C_MAIN;
                continue;
            }
            if (!strcmp(longopt, "jit-hybrid")) {
#ifdef CONFIG_JIT
                output_type = OUTPUT_C_HYBRID;
#else
                fprintf(stderr, "qjsc: --jit-hybrid requires CONFIG_JIT build\n");
                exit(1);
#endif
                continue;
            }
            if (!strcmp(longopt, "jit-hybrid-app")) {
#ifdef CONFIG_JIT
                output_type = OUTPUT_C_HYBRID_APP;
#else
                fprintf(stderr, "qjsc: --jit-hybrid-app requires CONFIG_JIT build\n");
                exit(1);
#endif
                continue;
            }
            if (!strcmp(longopt, "standalone")) {
                output_type = OUTPUT_STANDALONE;
                continue;
            }
            if (!strncmp(longopt, "jit-max-bc=", 11)) {
#ifdef CONFIG_JIT
                js_jit_set_max_bc_len(atoi(longopt + 11));
#endif
                continue;
            }
            if (!strncmp(longopt, "jit-pgo=", 8)) {
#ifdef CONFIG_JIT
                pgo_load(longopt + 8);
#else
                fprintf(stderr, "qjsc: --jit-pgo requires CONFIG_JIT build\n");
                exit(1);
#endif
                continue;
            }
            if (opt == 'N') {
                cname = get_short_optarg(&optind, opt, arg, argc, argv);
                break;
            }
            if (opt == 'f') {
                const char *p;
                optarg = get_short_optarg(&optind, opt, arg, argc, argv);
                p = optarg;
                if (!strcmp(p, "lto")) {
                    use_lto = TRUE;
                } else if (strstart(p, "no-", &p)) {
                    use_lto = TRUE;
                    for(i = 0; i < countof(feature_list); i++) {
                        if (!strcmp(p, feature_list[i].option_name)) {
                            feature_bitmap &= ~((uint64_t)1 << i);
                            break;
                        }
                    }
                    if (i == countof(feature_list))
                        goto bad_feature;
                } else {
                bad_feature:
                    fprintf(stderr, "unsupported feature: %s\n", optarg);
                    exit(1);
                }
                break;
            }
            if (opt == 'm') {
                module = 1;
                continue;
            }
            if (opt == 'M') {
                char *p;
                char path[1024];
                char cname[1024];

                optarg = get_short_optarg(&optind, opt, arg, argc, argv);
                pstrcpy(path, sizeof(path), optarg);
                p = strchr(path, ',');
                if (p) {
                    *p = '\0';
                    pstrcpy(cname, sizeof(cname), p + 1);
                } else {
                    get_c_name(cname, sizeof(cname), path);
                }
                namelist_add(&cmodule_list, path, cname, 0);
                break;
            }
            if (opt == 'D') {
                optarg = get_short_optarg(&optind, opt, arg, argc, argv);
                namelist_add(&dynamic_module_list, optarg, NULL, 0);
                break;
            }
            if (opt == 'x') {
                byte_swap = 1;
                continue;
            }
            if (opt == 'v') {
                verbose++;
                continue;
            }
            if (opt == 'p') {
                c_ident_prefix = get_short_optarg(&optind, opt, arg, argc, argv);
                break;
            }
            if (opt == 'S') {
                optarg = get_short_optarg(&optind, opt, arg, argc, argv);
                stack_size = get_suffixed_size(optarg);
                break;
            }
            if (opt == 's') {
                strip_flags = JS_STRIP_DEBUG;
                continue;
            }
            if (!strcmp(longopt, "keep-source")) {
                strip_flags = 0;
                continue;
            }
            if (opt) {
                fprintf(stderr, "qjsc: unknown option '-%c'\n", opt);
            } else {
                fprintf(stderr, "qjsc: unknown option '--%s'\n", longopt);
            }
            help();
        }
    }

    if (optind >= argc)
        help();

    if (!out_filename) {
        if (output_type == OUTPUT_EXECUTABLE || output_type == OUTPUT_STANDALONE) {
            out_filename = "a.out";
        } else {
            out_filename = "out.c";
        }
    }

    if (output_type == OUTPUT_EXECUTABLE || output_type == OUTPUT_STANDALONE) {
#if defined(_WIN32) || defined(__ANDROID__)
        snprintf(cfilename, sizeof(cfilename), "out%d.c", getpid());
#else
        snprintf(cfilename, sizeof(cfilename), "/tmp/out%d.c", getpid());
#endif
    } else {
        pstrcpy(cfilename, sizeof(cfilename), out_filename);
    }

    fo = fopen(cfilename, "w");
    if (!fo) {
        perror(cfilename);
        exit(1);
    }
    outfile = fo;

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);

    JS_SetStripInfo(rt, strip_flags);

    /* loader for ES6 modules */
    JS_SetModuleLoaderFunc2(rt, NULL, jsc_module_loader, NULL, NULL);

    fprintf(fo, "/* File generated automatically by the QuickJS compiler. */\n"
            "\n"
            );

#ifdef CONFIG_JIT
    if (output_type == OUTPUT_C_HYBRID) {
        /* Hybrid mode: compile one module file, emit bytecode + JIT bodies
         * + js_init_module.  Uses its own includes; skip the normal flow. */
        if (optind + 1 != argc) {
            fprintf(stderr, "qjsc: --jit-hybrid requires exactly one input file\n");
            exit(1);
        }
        const char *filename = argv[optind];
        uint8_t *buf;
        size_t buf_len;
        buf = js_load_file(ctx, &buf_len, filename);
        if (!buf) {
            fprintf(stderr, "qjsc: could not load '%s'\n", filename);
            exit(1);
        }
        /* Force module mode: --jit-hybrid only makes sense for modules */
        int eval_flags = JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY;
        JSValue mod_val = JS_Eval(ctx, (const char *)buf, buf_len,
                                   filename, eval_flags);
        js_free(ctx, buf);
        if (JS_IsException(mod_val)) {
            js_std_dump_error(ctx);
            exit(1);
        }
        output_hybrid_module(ctx, fo, mod_val);
        JS_FreeValue(ctx, mod_val);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        fclose(fo);
        return 0;
    }

    if (output_type == OUTPUT_C_HYBRID_APP) {
        /* Whole-app AOT mode: compile all listed module files, emit JIT bodies
         * for all eligible inner functions + js_init_app(ctx) entry point.
         * Each file is compiled independently (COMPILE_ONLY); no import
         * resolution — list all needed modules explicitly on the command line.
         *
         * Build the output as a shared library:
         *   gcc -O3 -flto -shared -fPIC -DCONFIG_JIT -I<quickjs> -o app.so out.c
         * Then at runtime: dlopen("app.so"); init(ctx) = js_init_app; init(ctx); */
        if (optind >= argc) {
            fprintf(stderr, "qjsc: --jit-hybrid-app requires at least one input file\n");
            exit(1);
        }
        int mod_count = argc - optind;
        JSValue *mod_vals = calloc((size_t)mod_count, sizeof(JSValue));
        for (int i = 0; i < mod_count; i++) {
            const char *fname = argv[optind + i];
            uint8_t *buf;
            size_t buf_len;
            buf = js_load_file(ctx, &buf_len, fname);
            if (!buf) {
                fprintf(stderr, "qjsc: could not load '%s'\n", fname);
                exit(1);
            }
            int eval_flags = JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY;
            JSValue v = JS_Eval(ctx, (const char *)buf, buf_len, fname, eval_flags);
            js_free(ctx, buf);
            if (JS_IsException(v)) {
                js_std_dump_error(ctx);
                exit(1);
            }
            mod_vals[i] = v;
        }
        output_hybrid_app(ctx, fo, mod_vals, mod_count);
        for (int i = 0; i < mod_count; i++)
            JS_FreeValue(ctx, mod_vals[i]);
        free(mod_vals);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        fclose(fo);
        return 0;
    }
#endif /* CONFIG_JIT */

    if (output_type == OUTPUT_STANDALONE) {
        /* P35.4 — standalone binary:
         *   - Compile all listed JS files; jsc_module_loader handles imports.
         *   - Bytecode blobs → cfilename (fo).
         *   - JIT function bodies → jit_tmp (via g_jit_module_hook + top-level walk).
         *   - Emit JIT table, _install_cb, _eval_blob, JS_NewCustomContext, main().
         *   - Compile cfilename → out_filename binary. */
#ifdef CONFIG_JIT
        FILE *jit_tmp = tmpfile();
        if (!jit_tmp) { perror("tmpfile"); exit(1); }
        AppWalkState jit_st = {ctx, jit_tmp, NULL, 0, 0, NULL, 0, 0};

        /* Hook so jsc_module_loader also walks dependency modules for JIT */
        g_jit_module_hook = standalone_module_walk;
        g_jit_module_hook_opaque = &jit_st;
#endif

        /* File header */
        fprintf(fo,
            "/* Generated by qjsc --standalone. Do not edit. */\n"
            "#include \"quickjs-libc.h\"\n"
            "#ifdef CONFIG_JIT\n"
            "#include \"quickjs-jit.h\"\n"
            "#endif\n\n");

        /* Compile each input file:
         *   - bytecode blob written to fo via output_object_code
         *   - JIT bodies written to jit_tmp via jit_st walker */
        for (i = optind; i < argc; i++) {
            const char *filename = argv[i];
            uint8_t *buf;
            size_t buf_len;
            char c_name_buf[1024];
            int is_mod, eval_fl;
            JSValue obj;

            buf = js_load_file(ctx, &buf_len, filename);
            if (!buf) {
                fprintf(stderr, "qjsc: cannot load '%s'\n", filename);
                exit(1);
            }
            is_mod = (module < 0)
                ? (has_suffix(filename, ".mjs") ||
                   JS_DetectModule((const char *)buf, buf_len))
                : module;
            eval_fl = JS_EVAL_FLAG_COMPILE_ONLY
                    | (is_mod ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
            obj = JS_Eval(ctx, (const char *)buf, buf_len, filename, eval_fl);
            js_free(ctx, buf);
            if (JS_IsException(obj)) {
                js_std_dump_error(ctx);
                exit(1);
            }

#ifdef CONFIG_JIT
            /* Walk JIT bytecodes for this top-level file */
            {
                JSFunctionBytecode *root_bc = is_mod
                    ? js_jit_module_get_bc(obj)
                    : ((JS_VALUE_GET_TAG(obj) == JS_TAG_FUNCTION_BYTECODE)
                       ? (JSFunctionBytecode *)JS_VALUE_GET_PTR(obj) : NULL);
                if (root_bc) {
                    app_walk_add_root(&jit_st, root_bc);
                    js_jit_walk_bytecodes(root_bc, app_walk_bodies_cb, &jit_st);
                }
            }
#endif

            /* Serialize bytecode blob to fo (as CNAME_TYPE_SCRIPT → executed in main) */
            if (cname) {
                pstrcpy(c_name_buf, sizeof(c_name_buf), cname);
                cname = NULL;
            } else {
                get_c_name(c_name_buf, sizeof(c_name_buf), filename);
                if (namelist_find(&cname_list, c_name_buf))
                    find_unique_cname(c_name_buf, sizeof(c_name_buf));
            }
            output_object_code(ctx, fo, obj, c_name_buf, CNAME_TYPE_SCRIPT);
            JS_FreeValue(ctx, obj);
        }

#ifdef CONFIG_JIT
        g_jit_module_hook = NULL;
        g_jit_module_hook_opaque = NULL;

        /* Emit JIT section: function bodies from jit_tmp, then dispatch table */
        fprintf(fo, "#ifdef CONFIG_JIT\n\n");
        {
            char cpbuf[4096];
            size_t nn;
            rewind(jit_tmp);
            while ((nn = fread(cpbuf, 1, sizeof(cpbuf), jit_tmp)) > 0)
                fwrite(cpbuf, 1, nn, fo);
        }
        fclose(jit_tmp);

        /* Dispatch table (empty array is valid C99) */
        fprintf(fo,
            "static const struct {\n"
            "    uint64_t   hash;\n"
            "    JSJITFunc  func;\n"
            "} _jit_table[] = {\n");
        for (int j = 0; j < jit_st.count; j++)
            fprintf(fo, "    { 0x%016llxULL, %s },\n",
                    (unsigned long long)jit_st.entries[j].hash,
                    jit_st.entries[j].fname);
        fprintf(fo, "};\n");
        fprintf(fo,
            "#define _JIT_TABLE_COUNT "
            "((int)(sizeof(_jit_table)/sizeof(_jit_table[0])))\n\n");

        /* Install callback */
        fprintf(fo,
            "static void _install_cb(JSFunctionBytecode *b, void *opaque)\n"
            "{\n"
            "    uint64_t h = js_jit_hash_bytecode_pub(b);\n"
            "    int i;\n"
            "    for (i = 0; i < _JIT_TABLE_COUNT; i++) {\n"
            "        if (_jit_table[i].hash == h) {\n"
            "            js_jit_fb_set_func(b, _jit_table[i].func, NULL, 2);\n"
            "            return;\n"
            "        }\n"
            "    }\n"
            "}\n");
        fprintf(fo, "#endif /* CONFIG_JIT */\n\n");

        free(jit_st.entries);
        free(jit_st.roots);
#endif /* CONFIG_JIT */

        /* _eval_blob: ReadObject + JIT install + EvalFunction.
         * Mirrors js_std_eval_binary but injects the JIT install step.
         * load_only=1: pre-load module dependency (sets import.meta, no exec).
         * load_only=0: execute script or entry module. */
        fprintf(fo,
            "static void _eval_blob(JSContext *ctx, const uint8_t *buf,\n"
            "                       uint32_t len, int load_only)\n"
            "{\n"
            "    JSValue obj = JS_ReadObject(ctx, buf, len, JS_READ_OBJ_BYTECODE);\n"
            "    if (JS_IsException(obj)) { js_std_dump_error(ctx); exit(1); }\n"
            "#ifdef CONFIG_JIT\n"
            "    {\n"
            "        JSFunctionBytecode *b =\n"
            "            (JS_VALUE_GET_TAG(obj) == JS_TAG_FUNCTION_BYTECODE)\n"
            "            ? (JSFunctionBytecode *)JS_VALUE_GET_PTR(obj)\n"
            "            : js_jit_module_get_bc(obj);\n"
            "        if (b) js_jit_walk_bytecodes(b, _install_cb, NULL);\n"
            "    }\n"
            "#endif\n"
            "    if (load_only) {\n"
            "        if (JS_VALUE_GET_TAG(obj) == JS_TAG_MODULE)\n"
            "            js_module_set_import_meta(ctx, obj, 0, 0);\n"
            "        JS_FreeValue(ctx, obj);\n"
            "    } else {\n"
            "        if (JS_VALUE_GET_TAG(obj) == JS_TAG_MODULE) {\n"
            "            if (JS_ResolveModule(ctx, obj) < 0) {\n"
            "                JS_FreeValue(ctx, obj);\n"
            "                js_std_dump_error(ctx); exit(1);\n"
            "            }\n"
            "            js_module_set_import_meta(ctx, obj, 0, 1);\n"
            "            obj = JS_EvalFunction(ctx, obj);\n"
            "            if (!JS_IsException(obj)) obj = js_std_await(ctx, obj);\n"
            "        } else {\n"
            "            obj = JS_EvalFunction(ctx, obj);\n"
            "        }\n"
            "        if (JS_IsException(obj)) { js_std_dump_error(ctx); exit(1); }\n"
            "        JS_FreeValue(ctx, obj);\n"
            "    }\n"
            "}\n\n");

        /* JS_NewCustomContext: context + intrinsics + pre-load module deps */
        fprintf(fo,
            "static JSContext *JS_NewCustomContext(JSRuntime *rt)\n"
            "{\n"
            "  JSContext *ctx = JS_NewContextRaw(rt);\n"
            "  if (!ctx)\n"
            "    return NULL;\n");
        fprintf(fo, "  JS_AddIntrinsicBaseObjects(ctx);\n");
        for (i = 0; i < countof(feature_list); i++) {
            if ((feature_bitmap & ((uint64_t)1 << i)) && feature_list[i].init_name)
                fprintf(fo, "  JS_AddIntrinsic%s(ctx);\n", feature_list[i].init_name);
        }
        /* Pre-load module dependencies (CNAME_TYPE_MODULE = from jsc_module_loader) */
        for (i = 0; i < cname_list.count; i++) {
            namelist_entry_t *e = &cname_list.array[i];
            if (e->flags == CNAME_TYPE_MODULE)
                fprintf(fo, "  _eval_blob(ctx, %s, %s_size, 1);\n",
                        e->name, e->name);
        }
        fprintf(fo, "  return ctx;\n}\n\n");

        /* main() */
        fputs(main_c_template1, fo);
        if (stack_size != 0)
            fprintf(fo, "  JS_SetMaxStackSize(rt, %u);\n", (unsigned int)stack_size);
        /* Module loader for dynamic import() at runtime */
        fprintf(fo,
            "  JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader,\n"
            "                         js_module_check_attributes, NULL);\n");
        fprintf(fo,
            "  ctx = JS_NewCustomContext(rt);\n"
            "  js_std_add_helpers(ctx, argc, argv);\n");
        /* Execute entry scripts/modules */
        for (i = 0; i < cname_list.count; i++) {
            namelist_entry_t *e = &cname_list.array[i];
            if (e->flags == CNAME_TYPE_SCRIPT)
                fprintf(fo, "  _eval_blob(ctx, %s, %s_size, 0);\n",
                        e->name, e->name);
        }
        fputs(main_c_template2, fo);

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        namelist_free(&cname_list);
        namelist_free(&cmodule_list);
        namelist_free(&init_module_list);
        fclose(fo);

        /* Compile to binary with -DCONFIG_JIT so JIT blocks are active */
        {
#ifdef CONFIG_JIT
            const char *sa_cflags[] = { "-DCONFIG_JIT", NULL };
#else
            const char *sa_cflags[] = { NULL };
#endif
            return output_executable(out_filename, cfilename, use_lto, verbose,
                                     argv[0], sa_cflags);
        }
    }

    if (output_type != OUTPUT_C) {
        fprintf(fo, "#include \"quickjs-libc.h\"\n"
                "\n"
                );
    } else {
        fprintf(fo, "#include <inttypes.h>\n"
                "\n"
                );
    }

    for(i = optind; i < argc; i++) {
        const char *filename = argv[i];
        compile_file(ctx, fo, filename, cname, module);
        cname = NULL;
    }

    for(i = 0; i < dynamic_module_list.count; i++) {
        if (!jsc_module_loader(ctx, dynamic_module_list.array[i].name, NULL, JS_UNDEFINED)) {
            fprintf(stderr, "Could not load dynamic module '%s'\n",
                    dynamic_module_list.array[i].name);
            exit(1);
        }
    }

    if (output_type != OUTPUT_C) {
        fprintf(fo,
                "static JSContext *JS_NewCustomContext(JSRuntime *rt)\n"
                "{\n"
                "  JSContext *ctx = JS_NewContextRaw(rt);\n"
                "  if (!ctx)\n"
                "    return NULL;\n");
        /* add the basic objects */
        fprintf(fo, "  JS_AddIntrinsicBaseObjects(ctx);\n");
        for(i = 0; i < countof(feature_list); i++) {
            if ((feature_bitmap & ((uint64_t)1 << i)) &&
                feature_list[i].init_name) {
                fprintf(fo, "  JS_AddIntrinsic%s(ctx);\n",
                        feature_list[i].init_name);
            }
        }
        /* add the precompiled modules (XXX: could modify the module
           loader instead) */
        for(i = 0; i < init_module_list.count; i++) {
            namelist_entry_t *e = &init_module_list.array[i];
            /* initialize the static C modules */

            fprintf(fo,
                    "  {\n"
                    "    extern JSModuleDef *js_init_module_%s(JSContext *ctx, const char *name);\n"
                    "    js_init_module_%s(ctx, \"%s\");\n"
                    "  }\n",
                    e->short_name, e->short_name, e->name);
        }
        for(i = 0; i < cname_list.count; i++) {
            namelist_entry_t *e = &cname_list.array[i];
            if (e->flags == CNAME_TYPE_MODULE) {
                fprintf(fo, "  js_std_eval_binary(ctx, %s, %s_size, 1);\n",
                        e->name, e->name);
            } else if (e->flags == CNAME_TYPE_JSON_MODULE) {
                fprintf(fo, "  js_std_eval_binary_json_module(ctx, %s, %s_size, (const char *)%s_module_name);\n",
                        e->name, e->name, e->name);
            }
        }
        fprintf(fo,
                "  return ctx;\n"
                "}\n\n");

        fputs(main_c_template1, fo);

        if (stack_size != 0) {
            fprintf(fo, "  JS_SetMaxStackSize(rt, %u);\n",
                    (unsigned int)stack_size);
        }

        /* add the module loader if necessary */
        if (feature_bitmap & (1 << FE_MODULE_LOADER)) {
            fprintf(fo, "  JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);\n");
        }

        fprintf(fo,
                "  ctx = JS_NewCustomContext(rt);\n"
                "  js_std_add_helpers(ctx, argc, argv);\n");

        for(i = 0; i < cname_list.count; i++) {
            namelist_entry_t *e = &cname_list.array[i];
            if (e->flags == CNAME_TYPE_SCRIPT) {
                fprintf(fo, "  js_std_eval_binary(ctx, %s, %s_size, 0);\n",
                        e->name, e->name);
            }
        }
        fputs(main_c_template2, fo);
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    fclose(fo);

    if (output_type == OUTPUT_EXECUTABLE) {
        return output_executable(out_filename, cfilename, use_lto, verbose,
                                 argv[0], NULL);
    }
    namelist_free(&cname_list);
    namelist_free(&cmodule_list);
    namelist_free(&init_module_list);
    return 0;
}
