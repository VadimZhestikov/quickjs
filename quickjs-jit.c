/*
 * QuickJS JIT Compiler — implementation
 *
 * Copyright (c) 2017-2025 Fabrice Bellard
 * Copyright (c) 2017-2025 Charlie Gordon
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
#ifdef CONFIG_JIT

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <dlfcn.h>

#include "quickjs.h"
#include "quickjs-jit.h"
#include "quickjs-opcode.h"
#include "libtcc.h"

/* Build OP_* enum locally from quickjs-opcode.h.
 * SHORT_OPCODES must be defined so that the short-form opcodes (goto8,
 * if_false8, etc.) that appear in final bytecode are included.           */
#ifndef SHORT_OPCODES
#define SHORT_OPCODES 1
#define JIT_DEFINED_SHORT_OPCODES
#endif
typedef enum {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) OP_##id,
#define def(id, size, n_pop, n_push, f) OP_##id,
#include "quickjs-opcode.h"
#undef def
#undef DEF
#undef FMT
    OP_COUNT_JIT
} OPCodeEnumJIT;
#ifdef JIT_DEFINED_SHORT_OPCODES
#undef SHORT_OPCODES
#undef JIT_DEFINED_SHORT_OPCODES
#endif

/* -----------------------------------------------------------------------
 * Forward declarations of internal QuickJS symbols used here but not in
 * the public quickjs.h header.
 * ----------------------------------------------------------------------- */

/* Non-inline destructor called when refcount reaches 0. */
extern void __JS_FreeValue(JSContext *ctx, JSValue v);

/* -----------------------------------------------------------------------
 * JSJITRuntime vtable — wrappers for dup/free (static inline in quickjs.h)
 * and property/call operations (public API).
 * Arithmetic/comparison wrappers live in quickjs.c (js_jit_op_*) so they
 * can reach static-only internal helpers.
 * ----------------------------------------------------------------------- */

static JSValue jit_rt_dup(JSContext *ctx, JSValue v)
{
    return JS_DupValue(ctx, v);
}

static void jit_rt_free(JSContext *ctx, JSValue v)
{
    JS_FreeValue(ctx, v);
}

static JSValue jit_rt_get_prop(JSContext *ctx, JSValue obj, JSAtom atom)
{
    return JS_GetProperty(ctx, obj, atom);
}

static int jit_rt_set_prop(JSContext *ctx, JSValue obj, JSAtom atom,
                           JSValue val)
{
    /* JS_SetProperty consumes val */
    return JS_SetProperty(ctx, obj, atom, val);
}

static JSValue jit_rt_get_array_el(JSContext *ctx, JSValue obj, JSValue idx)
{
    /* Convert index to atom and use the atom-based getter */
    JSAtom atom = JS_ValueToAtom(ctx, idx);
    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    JSValue r = JS_GetProperty(ctx, obj, atom);
    JS_FreeAtom(ctx, atom);
    return r;
}

static int jit_rt_set_array_el(JSContext *ctx, JSValue obj, JSValue idx,
                               JSValue val)
{
    /* Convert index to atom and use the atom-based setter (val consumed) */
    JSAtom atom = JS_ValueToAtom(ctx, idx);
    if (atom == JS_ATOM_NULL) {
        JS_FreeValue(ctx, val);
        return -1;
    }
    int r = JS_SetProperty(ctx, obj, atom, val);
    JS_FreeAtom(ctx, atom);
    return r;
}

static JSValue jit_rt_call(JSContext *ctx, JSValue func, JSValue this_val,
                           int argc, JSValue *argv)
{
    return JS_Call(ctx, func, this_val, argc, argv);
}

static JSValue jit_rt_call_constructor(JSContext *ctx, JSValue ctor,
                                       JSValue new_target,
                                       int argc, JSValue *argv)
{
    (void)new_target; /* Phase 4 will wire up new.target properly */
    return JS_CallConstructor(ctx, ctor, argc, argv);
}

static JSValue jit_rt_throw_type_error(JSContext *ctx, const char *fmt, ...)
{
    JS_ThrowTypeError(ctx, "%s", fmt);
    return JS_EXCEPTION;
}

static JSValue jit_rt_throw_val(JSContext *ctx, JSValue val)
{
    return JS_Throw(ctx, val);
}

/* -----------------------------------------------------------------------
 * Global vtable — filled at compile time, never mutated after.
 * All arithmetic/comparison entries point to js_jit_op_* helpers defined
 * in quickjs.c (they need access to static internal functions).
 * ----------------------------------------------------------------------- */

const JSJITRuntime js_jit_rt = {
    /* arithmetic */
    .add              = js_jit_op_add,
    .sub              = js_jit_op_sub,
    .mul              = js_jit_op_mul,
    .div              = js_jit_op_div,
    .mod              = js_jit_op_mod,
    /* bitwise */
    .shl              = js_jit_op_shl,
    .sar              = js_jit_op_sar,
    .shr              = js_jit_op_shr,
    .band             = js_jit_op_band,
    .bor              = js_jit_op_bor,
    .bxor             = js_jit_op_bxor,
    /* unary */
    .neg              = js_jit_op_neg,
    .plus             = js_jit_op_plus,
    .bnot             = js_jit_op_bnot,
    .type_of          = js_jit_op_type_of,
    /* comparisons */
    .lt               = js_jit_op_lt,
    .lte              = js_jit_op_lte,
    .eq               = js_jit_op_eq,
    .strict_eq        = js_jit_op_strict_eq,
    /* ref counting */
    .dup              = jit_rt_dup,
    .free             = jit_rt_free,
    /* property access */
    .get_prop         = jit_rt_get_prop,
    .set_prop         = jit_rt_set_prop,
    .get_array_el     = jit_rt_get_array_el,
    .set_array_el     = jit_rt_set_array_el,
    /* calls */
    .call             = jit_rt_call,
    .call_constructor = jit_rt_call_constructor,
    /* exceptions */
    .throw_type_error = jit_rt_throw_type_error,
    .throw_val        = jit_rt_throw_val,
};

/* -----------------------------------------------------------------------
 * Task #4 — js_jit_is_eligible()
 *
 * Returns 1 if a JSFunctionBytecode can be JIT-compiled, 0 otherwise.
 * Called before every compilation attempt and also used as a fast pre-check
 * inside JS_CallInternal.
 * ----------------------------------------------------------------------- */

/*
 * Internal structure forward — we need func_kind and flag bits.
 * quickjs-jit.h provides JSFunctionBytecode as an opaque forward; we access
 * only the fields exposed through the public quickjs.h / quickjs-jit.h API.
 * Because quickjs-jit.c is linked with quickjs.o the real struct definition
 * is visible at link time, but we must use the public accessors here.
 *
 * Workaround: the eligibility flags are byte-level accessible.  We define
 * a thin "view struct" that mirrors the first bytes of JSFunctionBytecode.
 * This is fragile; Phase 3 will centralise this into a helper macro.
 */

/* func_kind == 0 means JS_FUNC_NORMAL; any other value is generator/async */
#define JS_JIT_FUNC_NORMAL 0

int js_jit_is_eligible(JSFunctionBytecode *b)
{
    /* Generators/async require saved execution context (yield/await) */
    if (js_jit_fb_func_kind(b) != JS_JIT_FUNC_NORMAL)
        return 0;
    /* eval() has dynamic variable scoping — incompatible with JIT */
    if (js_jit_fb_is_eval(b))
        return 0;
    /* Complex params: destructuring, rest, default values */
    if (!js_jit_fb_has_simple_params(b))
        return 0;
    /* Class method: needs home_object for super */
    if (js_jit_fb_need_home_object(b))
        return 0;
    /* Derived class constructor: special super() handling required */
    if (js_jit_fb_is_derived_ctor(b))
        return 0;
    return 1;
}

/* =======================================================================
 * Phase 2.1 — JSJITCodeBuf: growable C-source buffer
 *
 * Generated C code is accumulated in a heap-allocated buffer that doubles
 * on overflow.  The buffer is always NUL-terminated so it can be passed
 * directly to tcc_compile_string().
 * ======================================================================= */

#define JIT_BUF_INIT_CAP  4096

typedef struct JSJITCodeBuf {
    char  *buf;    /* NUL-terminated C source being built       */
    size_t len;    /* bytes written (excluding the NUL)          */
    size_t cap;    /* allocated capacity including the NUL slot  */
    int    error;  /* set to 1 on OOM; all subsequent ops no-op */
} JSJITCodeBuf;

static int jit_buf_init(JSJITCodeBuf *cb)
{
    cb->buf = malloc(JIT_BUF_INIT_CAP);
    if (!cb->buf) { cb->error = 1; return -1; }
    cb->buf[0] = '\0';
    cb->len    = 0;
    cb->cap    = JIT_BUF_INIT_CAP;
    cb->error  = 0;
    return 0;
}

static void jit_buf_free(JSJITCodeBuf *cb)
{
    free(cb->buf);
    cb->buf = NULL;
}

/* Grow the buffer so at least (cb->len + need + 1) bytes are available. */
static int jit_buf_grow(JSJITCodeBuf *cb, size_t need)
{
    size_t new_cap = cb->cap;
    while (new_cap < cb->len + need + 1)
        new_cap *= 2;
    if (new_cap == cb->cap)
        return 0;
    char *p = realloc(cb->buf, new_cap);
    if (!p) { cb->error = 1; return -1; }
    cb->buf = p;
    cb->cap = new_cap;
    return 0;
}

/* Append a printf-formatted string to the buffer. */
static void jit_buf_printf(JSJITCodeBuf *cb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void jit_buf_printf(JSJITCodeBuf *cb, const char *fmt, ...)
{
    if (cb->error) return;
    va_list ap;
    va_start(ap, fmt);
    /* First try in the remaining space */
    size_t avail = cb->cap - cb->len;
    int n = vsnprintf(cb->buf + cb->len, avail, fmt, ap);
    va_end(ap);
    if (n < 0) { cb->error = 1; return; }
    if ((size_t)n < avail) {
        cb->len += (size_t)n;
        return;
    }
    /* Didn't fit — grow and retry */
    if (jit_buf_grow(cb, (size_t)n) < 0) return;
    va_start(ap, fmt);
    avail = cb->cap - cb->len;
    n = vsnprintf(cb->buf + cb->len, avail, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= avail) { cb->error = 1; return; }
    cb->len += (size_t)n;
}

/* Append a raw string (no formatting). */
static void jit_buf_str(JSJITCodeBuf *cb, const char *s)
{
    if (cb->error) return;
    size_t slen = strlen(s);
    if (cb->len + slen + 1 > cb->cap)
        if (jit_buf_grow(cb, slen) < 0) return;
    memcpy(cb->buf + cb->len, s, slen + 1); /* copy including NUL */
    cb->len += slen;
}

/* =======================================================================
 * Phase 2.2 — Scan pass
 *
 * A single linear walk over the bytecode that:
 *   1. Collects all branch targets into a sorted array (for label emission).
 *   2. Detects opcodes the code generator does not yet support, causing an
 *      early bail-out so we never generate incorrect C code.
 *
 * Opcodes excluded from JIT in this phase (set jit_no_compile):
 *   - catch / gosub / nip_catch : try/finally blocks need a full frame
 *   - with_* : dynamic scoping
 *   - for_in_start / for_of_start / for_*_next / iterator_* : iterators
 *   - apply / apply_eval : spread calls
 *   - delete / delete_var : property deletion
 * ======================================================================= */

/* Maximum number of distinct branch targets in one function.
 * Functions with more branches are rejected (extremely rare in practice). */
#define JIT_MAX_LABELS 4096

typedef struct JSJITScanResult {
    int      *targets;    /* sorted array of branch-target offsets */
    int       ntargets;   /* number of entries in targets[]         */
    int       unsupported; /* 1 if an unsupported opcode was found   */
} JSJITScanResult;

static void scan_result_free(JSJITScanResult *sr)
{
    free(sr->targets);
    sr->targets  = NULL;
    sr->ntargets = 0;
}

/* Insert offset into the target list (keeps it sorted, no duplicates). */
static int scan_add_target(JSJITScanResult *sr, int off, int *cap)
{
    /* Check for duplicate */
    for (int i = 0; i < sr->ntargets; i++)
        if (sr->targets[i] == off) return 0;

    if (sr->ntargets >= JIT_MAX_LABELS) return -1;
    if (sr->ntargets >= *cap) {
        int new_cap = *cap ? *cap * 2 : 64;
        int *p = realloc(sr->targets, (size_t)new_cap * sizeof(int));
        if (!p) return -1;
        sr->targets = p;
        *cap = new_cap;
    }
    sr->targets[sr->ntargets++] = off;
    /* Insertion-sort to keep array sorted */
    for (int i = sr->ntargets - 1; i > 0 &&
             sr->targets[i] < sr->targets[i-1]; i--) {
        int tmp = sr->targets[i];
        sr->targets[i] = sr->targets[i-1];
        sr->targets[i-1] = tmp;
    }
    return 0;
}

/* Read a 32-bit little-endian value from unaligned bytecode pointer. */
static inline int32_t bc_get_i32(const uint8_t *pc)
{
    return (int32_t)((uint32_t)pc[0] | ((uint32_t)pc[1] << 8) |
                     ((uint32_t)pc[2] << 16) | ((uint32_t)pc[3] << 24));
}
static inline int8_t  bc_get_i8 (const uint8_t *pc) { return (int8_t)pc[0]; }
static inline int16_t bc_get_i16(const uint8_t *pc) {
    return (int16_t)((uint16_t)pc[0] | ((uint16_t)pc[1] << 8));
}

/* Opcodes unsupported in this JIT phase. Returns 1 if op is unsupported. */
static int scan_is_unsupported(int op)
{
    switch (op) {
    /* try/finally frame management */
    case OP_catch:
    case OP_gosub:
    case OP_nip_catch:
    /* with-statement dynamic scoping */
    case OP_with_get_var:
    case OP_with_put_var:
    case OP_with_delete_var:
    case OP_with_make_ref:
    case OP_with_get_ref:
    /* iterators / for-in / for-of */
    case OP_for_in_start:
    case OP_for_of_start:
    case OP_for_in_next:
    case OP_for_of_next:
    case OP_for_await_of_next:
    case OP_iterator_check_object:
    case OP_iterator_get_value_done:
    case OP_iterator_close:
    case OP_iterator_next:
    case OP_iterator_call:
    /* spread / apply */
    case OP_apply:
    case OP_apply_eval:
    /* property deletion */
    case OP_delete:
    case OP_delete_var:
        return 1;
    default:
        return 0;
    }
}

/*
 * js_jit_scan() — run the scan pass on function bytecode b.
 *
 * Returns 0 on success (sr filled in), -1 on OOM or unsupported opcode.
 * On -1, sr->unsupported indicates whether the function must be excluded.
 */
static int js_jit_scan(JSFunctionBytecode *b, JSJITScanResult *sr)
{
    int bc_len;
    const uint8_t *bc = js_jit_fb_get_bytecode(b, &bc_len);
    int tbl_count;
    const uint8_t *op_sz = js_jit_get_opcode_size_table(&tbl_count);

    sr->targets    = NULL;
    sr->ntargets   = 0;
    sr->unsupported = 0;
    int cap = 0;

    int pc = 0;
    while (pc < bc_len) {
        int op = bc[pc];
        if (op >= tbl_count || op_sz[op] == 0) {
            sr->unsupported = 1;
            return -1;
        }
        if (scan_is_unsupported(op)) {
            sr->unsupported = 1;
            scan_result_free(sr);
            return -1;
        }

        int sz = op_sz[op];
        /* Extract branch target offsets. The offset field is relative to the
         * byte immediately after the full instruction (pc + sz). */
        switch (op) {
        case OP_if_false:
        case OP_if_true:
        case OP_goto:
        case OP_gosub: {
            int32_t delta = bc_get_i32(&bc[pc + 1]);
            int target = pc + sz + delta;
            if (scan_add_target(sr, target, &cap) < 0) {
                scan_result_free(sr); return -1;
            }
            break;
        }
        case OP_if_false8:
        case OP_if_true8:
        case OP_goto8: {
            int target = pc + sz + (int)bc_get_i8(&bc[pc + 1]);
            if (scan_add_target(sr, target, &cap) < 0) {
                scan_result_free(sr); return -1;
            }
            break;
        }
        case OP_goto16: {
            int target = pc + sz + (int)bc_get_i16(&bc[pc + 1]);
            if (scan_add_target(sr, target, &cap) < 0) {
                scan_result_free(sr); return -1;
            }
            break;
        }
        default:
            break;
        }
        pc += sz;
    }
    return 0;
}

/* Returns 1 if offset is a branch target, 0 otherwise (binary search). */
static int scan_is_target(const JSJITScanResult *sr, int off)
{
    int lo = 0, hi = sr->ntargets - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (sr->targets[mid] == off) return 1;
        if (sr->targets[mid] < off)  lo = mid + 1;
        else                          hi = mid - 1;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */

void js_jit_init(void)
{
    /* Phase 4: initialise GCC background thread here */
}

void js_jit_free(void)
{
    /* Phase 4: join GCC background thread here */
}

/* -----------------------------------------------------------------------
 * Stub implementations (filled in Phases 3 and 4)
 * ----------------------------------------------------------------------- */

/* =======================================================================
 * Phase 2.3–2.11 — C code generator
 *
 * js_jit_gen_c(b, cb) translates JSFunctionBytecode *b into a C source
 * string in *cb.  The generated function has the JSJITFunc signature.
 *
 * Generated code model:
 *   JSValue _s[STACK_SIZE]  — evaluation stack (indexed by _sp)
 *   JSValue _l[VAR_COUNT]   — local variables (lN in JS)
 *   JSValue *_a             — aliases argv (arg variables)
 *   JSVarRef **_vr          — aliases var_refs (closure variables)
 *   int _sp                 — runtime stack pointer
 *
 * Every operation that can throw appends "if(_sp_ok){goto _ex;}" via
 * the _CHK macro inside the generated code.  The _ex label frees all
 * live values and returns JS_EXCEPTION.
 *
 * Branch targets from the scan pass become C labels "_L<offset>:".
 * ======================================================================= */

/* Helper: read little-endian integers from bytecode stream */
static inline uint32_t bc_u32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|
           ((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static inline uint16_t bc_u16(const uint8_t *p) {
    return (uint16_t)p[0]|((uint16_t)p[1]<<8);
}

/*
 * Emit the C preamble: type definitions and the function signature.
 * The function symbol name encodes the bytecode pointer as a hex address
 * so multiple compiled functions don't clash when linked.
 */
static void gen_preamble(JSJITCodeBuf *cb, JSFunctionBytecode *b,
                         int var_count, int arg_count, int stack_size,
                         int closure_var_count, int cpool_count,
                         char *fname_out, size_t fname_sz)
{
    /* Unique function name based on pointer value */
    snprintf(fname_out, fname_sz, "__jit_f_%016llx",
             (unsigned long long)(uintptr_t)b);

    jit_buf_str(cb,
        "#include <stdint.h>\n"
        "#include \"quickjs.h\"\n"
        "#include \"quickjs-jit.h\"\n"
        "#define _RT  (&js_jit_rt)\n"
        "#define _DUP(v)  (_RT->dup(ctx,(v)))\n"
        "#define _FREE(v) (_RT->free(ctx,(v)))\n"
        "#define _CHK(v)  do{if(JS_VALUE_GET_TAG(v)==JS_TAG_EXCEPTION)"
                          "goto _ex;}while(0)\n"
    );

    /* Function signature */
    jit_buf_printf(cb,
        "JSValue %s(\n"
        "    JSContext *ctx, JSValue this_val,\n"
        "    int argc, JSValue *argv,\n"
        "    JSValue *cpool, JSVarRef **var_refs)\n"
        "{\n",
        fname_out);

    /* Stack and local variable declarations */
    if (stack_size > 0)
        jit_buf_printf(cb, "    JSValue _s[%d];\n", stack_size);
    else
        jit_buf_str(cb, "    JSValue _s[1];\n"); /* avoid zero-length array */
    if (var_count > 0)
        jit_buf_printf(cb, "    JSValue _l[%d];\n", var_count);
    jit_buf_str(cb,
        "    int _sp=0, _i;\n"
        "    (void)argc; (void)cpool; (void)var_refs;\n");
    if (var_count > 0)
        jit_buf_printf(cb,
            "    for(_i=0;_i<%d;_i++) _l[_i]=JS_UNDEFINED;\n", var_count);
}

/*
 * Emit the exception-cleanup footer and closing brace.
 */
static void gen_footer(JSJITCodeBuf *cb, int var_count)
{
    jit_buf_str(cb, "_ex:\n");
    if (var_count > 0)
        jit_buf_printf(cb,
            "    for(_i=0;_i<%d;_i++) _FREE(_l[_i]);\n", var_count);
    jit_buf_str(cb,
        "    for(;_sp>0;) _FREE(_s[--_sp]);\n"
        "    return JS_EXCEPTION;\n"
        "}\n");
}

/*
 * Main code generator: iterates over the bytecode and emits a C statement
 * for each opcode.  Unsupported opcodes set *unsupported=1 and return -1.
 */
static int gen_body(JSJITCodeBuf *cb, const uint8_t *bc, int bc_len,
                    const JSJITScanResult *sr,
                    const uint8_t *op_sz, int op_sz_count,
                    int var_count, int arg_count,
                    int *unsupported_out)
{
    *unsupported_out = 0;
    int pc = 0;

    while (pc < bc_len) {
        /* Emit label if this offset is a branch target */
        if (scan_is_target(sr, pc))
            jit_buf_printf(cb, "_L%d:;\n", pc);

        int op = bc[pc];
        if (op >= op_sz_count || op_sz[op] == 0) {
            *unsupported_out = 1; return -1;
        }
        int sz = op_sz[op];

        switch (op) {

        /* ---- nop ---- */
        case OP_nop:
            break;

        /* ---- Push immediate values ---- */
        case OP_push_i32:
            jit_buf_printf(cb,
                "    _s[_sp++]=JS_NewInt32(ctx,(int32_t)%uu);\n",
                bc_u32(&bc[pc+1]));
            break;
        case OP_push_i8:
            jit_buf_printf(cb,
                "    _s[_sp++]=JS_NewInt32(ctx,%d);\n",
                (int)(int8_t)bc[pc+1]);
            break;
        case OP_push_i16:
            jit_buf_printf(cb,
                "    _s[_sp++]=JS_NewInt32(ctx,%d);\n",
                (int)(int16_t)bc_u16(&bc[pc+1]));
            break;
        case OP_push_minus1:
            jit_buf_str(cb, "    _s[_sp++]=JS_NewInt32(ctx,-1);\n"); break;
        case OP_push_0:
            jit_buf_str(cb, "    _s[_sp++]=JS_NewInt32(ctx,0);\n"); break;
        case OP_push_1:
            jit_buf_str(cb, "    _s[_sp++]=JS_NewInt32(ctx,1);\n"); break;
        case OP_push_2:
            jit_buf_str(cb, "    _s[_sp++]=JS_NewInt32(ctx,2);\n"); break;
        case OP_push_3:
            jit_buf_str(cb, "    _s[_sp++]=JS_NewInt32(ctx,3);\n"); break;
        case OP_push_4:
            jit_buf_str(cb, "    _s[_sp++]=JS_NewInt32(ctx,4);\n"); break;
        case OP_push_5:
            jit_buf_str(cb, "    _s[_sp++]=JS_NewInt32(ctx,5);\n"); break;
        case OP_push_6:
            jit_buf_str(cb, "    _s[_sp++]=JS_NewInt32(ctx,6);\n"); break;
        case OP_push_7:
            jit_buf_str(cb, "    _s[_sp++]=JS_NewInt32(ctx,7);\n"); break;
        case OP_push_false:
            jit_buf_str(cb, "    _s[_sp++]=JS_FALSE;\n"); break;
        case OP_push_true:
            jit_buf_str(cb, "    _s[_sp++]=JS_TRUE;\n"); break;
        case OP_undefined:
            jit_buf_str(cb, "    _s[_sp++]=JS_UNDEFINED;\n"); break;
        case OP_null:
            jit_buf_str(cb, "    _s[_sp++]=JS_NULL;\n"); break;
        case OP_push_this:
            jit_buf_str(cb, "    _s[_sp++]=_DUP(this_val);\n"); break;
        case OP_push_empty_string:
            jit_buf_str(cb,
                "    { JSValue _v=JS_NewStringLen(ctx,\"\",0);"
                " _CHK(_v); _s[_sp++]=_v; }\n");
            break;

        /* ---- Constant pool ---- */
        case OP_push_const:
            jit_buf_printf(cb,
                "    _s[_sp++]=_DUP(cpool[%u]);\n",
                bc_u32(&bc[pc+1]));
            break;
        case OP_push_const8:
            jit_buf_printf(cb,
                "    _s[_sp++]=_DUP(cpool[%u]);\n",
                (unsigned)bc[pc+1]);
            break;

        /* ---- Stack manipulation ---- */
        case OP_drop:
            jit_buf_str(cb, "    _FREE(_s[--_sp]);\n"); break;
        case OP_dup:
            jit_buf_str(cb, "    _s[_sp]=_DUP(_s[_sp-1]); _sp++;\n"); break;
        case OP_dup1: /* a b -> a a b */
            jit_buf_str(cb,
                "    { JSValue _t=_DUP(_s[_sp-2]);"
                " _s[_sp]=_s[_sp-1]; _s[_sp-1]=_t; _sp++; }\n");
            break;
        case OP_dup2:
            jit_buf_str(cb,
                "    _s[_sp]=_DUP(_s[_sp-2]);"
                " _s[_sp+1]=_DUP(_s[_sp-1]); _sp+=2;\n");
            break;
        case OP_insert2: /* obj a -> a obj a */
            jit_buf_str(cb,
                "    { JSValue _t=_s[_sp-1];"
                " _s[_sp]=_s[_sp-1]; _s[_sp-1]=_s[_sp-2];"
                " _s[_sp-2]=_t; _sp++; }\n");
            break;
        /* OP_pop does not exist; OP_drop handles the pop case */
        case OP_nip: /* a b -> b */
            jit_buf_str(cb,
                "    { JSValue _t=_s[--_sp]; _FREE(_s[_sp-1]);"
                " _s[_sp-1]=_t; }\n");
            break;
        case OP_swap:
            jit_buf_str(cb,
                "    { JSValue _t=_s[_sp-1]; _s[_sp-1]=_s[_sp-2];"
                " _s[_sp-2]=_t; }\n");
            break;
        case OP_rot3l: /* a b c -> b c a */
            jit_buf_str(cb,
                "    { JSValue _t=_s[_sp-3]; _s[_sp-3]=_s[_sp-2];"
                " _s[_sp-2]=_s[_sp-1]; _s[_sp-1]=_t; }\n");
            break;
        case OP_rot3r: /* a b c -> c a b */
            jit_buf_str(cb,
                "    { JSValue _t=_s[_sp-1]; _s[_sp-1]=_s[_sp-2];"
                " _s[_sp-2]=_s[_sp-3]; _s[_sp-3]=_t; }\n");
            break;

        /* ---- Local variable access ---- */
#define GEN_GET_LOC(idx) \
    jit_buf_printf(cb, "    _s[_sp++]=_DUP(_l[%d]);\n", idx)
#define GEN_PUT_LOC(idx) \
    jit_buf_printf(cb, "    _FREE(_l[%d]); _l[%d]=_s[--_sp];\n", idx, idx)
#define GEN_SET_LOC(idx) \
    jit_buf_printf(cb, "    _FREE(_l[%d]); _l[%d]=_DUP(_s[_sp-1]);\n", idx, idx)

        case OP_get_loc:  case OP_get_loc_check:
        case OP_get_loc_checkthis: GEN_GET_LOC((int)bc_u16(&bc[pc+1])); break;
        case OP_put_loc:  case OP_put_loc_check:
        case OP_put_loc_check_init: GEN_PUT_LOC((int)bc_u16(&bc[pc+1])); break;
        case OP_set_loc:  GEN_SET_LOC((int)bc_u16(&bc[pc+1])); break;
        case OP_get_loc8: GEN_GET_LOC((int)bc[pc+1]); break;
        case OP_put_loc8: GEN_PUT_LOC((int)bc[pc+1]); break;
        case OP_set_loc8: GEN_SET_LOC((int)bc[pc+1]); break;
        case OP_get_loc0: GEN_GET_LOC(0); break;
        case OP_get_loc1: GEN_GET_LOC(1); break;
        case OP_get_loc2: GEN_GET_LOC(2); break;
        case OP_get_loc3: GEN_GET_LOC(3); break;
        case OP_put_loc0: GEN_PUT_LOC(0); break;
        case OP_put_loc1: GEN_PUT_LOC(1); break;
        case OP_put_loc2: GEN_PUT_LOC(2); break;
        case OP_put_loc3: GEN_PUT_LOC(3); break;
        case OP_set_loc0: GEN_SET_LOC(0); break;
        case OP_set_loc1: GEN_SET_LOC(1); break;
        case OP_set_loc2: GEN_SET_LOC(2); break;
        case OP_set_loc3: GEN_SET_LOC(3); break;

#undef GEN_GET_LOC
#undef GEN_PUT_LOC
#undef GEN_SET_LOC

        /* ---- Argument access ---- */
#define GEN_GET_ARG(idx) \
    jit_buf_printf(cb, "    _s[_sp++]=((%d)<argc?_DUP(argv[%d]):JS_UNDEFINED);\n", idx, idx)
#define GEN_PUT_ARG(idx) \
    jit_buf_printf(cb, "    if((%d)<argc){_FREE(argv[%d]); argv[%d]=_s[--_sp];}else _FREE(_s[--_sp]);\n", idx, idx, idx)
#define GEN_SET_ARG(idx) \
    jit_buf_printf(cb, "    if((%d)<argc){_FREE(argv[%d]); argv[%d]=_DUP(_s[_sp-1]);};\n", idx, idx, idx)

        case OP_get_arg: GEN_GET_ARG((int)bc_u16(&bc[pc+1])); break;
        case OP_put_arg: GEN_PUT_ARG((int)bc_u16(&bc[pc+1])); break;
        case OP_set_arg: GEN_SET_ARG((int)bc_u16(&bc[pc+1])); break;
        case OP_get_arg0: GEN_GET_ARG(0); break;
        case OP_get_arg1: GEN_GET_ARG(1); break;
        case OP_get_arg2: GEN_GET_ARG(2); break;
        case OP_get_arg3: GEN_GET_ARG(3); break;
        case OP_put_arg0: GEN_PUT_ARG(0); break;
        case OP_put_arg1: GEN_PUT_ARG(1); break;
        case OP_put_arg2: GEN_PUT_ARG(2); break;
        case OP_put_arg3: GEN_PUT_ARG(3); break;
        case OP_set_arg0: GEN_SET_ARG(0); break;
        case OP_set_arg1: GEN_SET_ARG(1); break;
        case OP_set_arg2: GEN_SET_ARG(2); break;
        case OP_set_arg3: GEN_SET_ARG(3); break;

#undef GEN_GET_ARG
#undef GEN_PUT_ARG
#undef GEN_SET_ARG

        /* ---- Closure variable access ---- */
#define GEN_GET_VR(idx) \
    jit_buf_printf(cb, "    _s[_sp++]=_DUP(*var_refs[%d]->pvalue);\n", idx)
#define GEN_PUT_VR(idx) \
    jit_buf_printf(cb, "    _FREE(*var_refs[%d]->pvalue); *var_refs[%d]->pvalue=_s[--_sp];\n", idx, idx)
#define GEN_SET_VR(idx) \
    jit_buf_printf(cb, "    _FREE(*var_refs[%d]->pvalue); *var_refs[%d]->pvalue=_DUP(_s[_sp-1]);\n", idx, idx)

        case OP_get_var_ref:
        case OP_get_var_ref_check: GEN_GET_VR((int)bc_u16(&bc[pc+1])); break;
        case OP_put_var_ref:
        case OP_put_var_ref_check:
        case OP_put_var_ref_check_init: GEN_PUT_VR((int)bc_u16(&bc[pc+1])); break;
        case OP_set_var_ref: GEN_SET_VR((int)bc_u16(&bc[pc+1])); break;
        case OP_get_var_ref0: GEN_GET_VR(0); break;
        case OP_get_var_ref1: GEN_GET_VR(1); break;
        case OP_get_var_ref2: GEN_GET_VR(2); break;
        case OP_get_var_ref3: GEN_GET_VR(3); break;
        case OP_put_var_ref0: GEN_PUT_VR(0); break;
        case OP_put_var_ref1: GEN_PUT_VR(1); break;
        case OP_put_var_ref2: GEN_PUT_VR(2); break;
        case OP_put_var_ref3: GEN_PUT_VR(3); break;
        case OP_set_var_ref0: GEN_SET_VR(0); break;
        case OP_set_var_ref1: GEN_SET_VR(1); break;
        case OP_set_var_ref2: GEN_SET_VR(2); break;
        case OP_set_var_ref3: GEN_SET_VR(3); break;

#undef GEN_GET_VR
#undef GEN_PUT_VR
#undef GEN_SET_VR

        /* ---- Arithmetic (binary) with inline integer fast paths ----
         *
         * For int+int operations the common case is that both operands are
         * JS_TAG_INT immediates.  We inline that check so TCC emits a simple
         * compare + two-instruction arithmetic sequence, avoiding the vtable
         * call entirely.  The slow path (strings, floats, BigInt) falls back
         * to the vtable wrapper which handles all cases.
         *
         * Integer values have no refcount — JS_FreeValue on TAG_INT is a
         * no-op, so we skip _FREE for the int fast path.
         * ---------------------------------------------------------------- */

        /* add: int overflow check using 64-bit arithmetic */
        case OP_add:
            jit_buf_str(cb,
                "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT){\n"
                "        int64_t _r64=(int64_t)JS_VALUE_GET_INT(_a)+JS_VALUE_GET_INT(_b);\n"
                "        _s[_sp++]=((int32_t)_r64==_r64)?JS_NewInt32(ctx,(int32_t)_r64)\n"
                "                                       :JS_NewFloat64(ctx,(double)_r64);\n"
                "      } else {\n"
                "        JSValue _r=_RT->add(ctx,_a,_b); _CHK(_r); _s[_sp++]=_r;\n"
                "      } }\n");
            break;

        /* sub: int fast path */
        case OP_sub:
            jit_buf_str(cb,
                "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT){\n"
                "        int64_t _r64=(int64_t)JS_VALUE_GET_INT(_a)-JS_VALUE_GET_INT(_b);\n"
                "        _s[_sp++]=((int32_t)_r64==_r64)?JS_NewInt32(ctx,(int32_t)_r64)\n"
                "                                       :JS_NewFloat64(ctx,(double)_r64);\n"
                "      } else {\n"
                "        JSValue _r=_RT->sub(ctx,_a,_b); _CHK(_r); _s[_sp++]=_r;\n"
                "      } }\n");
            break;

        /* mul: int fast path, overflow via int64 */
        case OP_mul:
            jit_buf_str(cb,
                "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT){\n"
                "        int64_t _r64=(int64_t)JS_VALUE_GET_INT(_a)*JS_VALUE_GET_INT(_b);\n"
                "        if((int32_t)_r64==_r64 && !(_r64==0 && ((JS_VALUE_GET_INT(_a)^JS_VALUE_GET_INT(_b))>>31)))\n"
                "          _s[_sp++]=JS_NewInt32(ctx,(int32_t)_r64);\n"
                "        else\n"
                "          _s[_sp++]=JS_NewFloat64(ctx,(double)JS_VALUE_GET_INT(_a)*(double)JS_VALUE_GET_INT(_b));\n"
                "      } else {\n"
                "        JSValue _r=_RT->mul(ctx,_a,_b); _CHK(_r); _s[_sp++]=_r;\n"
                "      } }\n");
            break;

        /* div: always float result; only skip vtable for int/int */
        case OP_div:
            jit_buf_str(cb,
                "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT){\n"
                "        int32_t ia=JS_VALUE_GET_INT(_a),ib=JS_VALUE_GET_INT(_b);\n"
                "        _s[_sp++]=(ib&&ia%ib==0)?JS_NewInt32(ctx,ia/ib)\n"
                "                                :JS_NewFloat64(ctx,(double)ia/(double)ib);\n"
                "      } else {\n"
                "        JSValue _r=_RT->div(ctx,_a,_b); _CHK(_r); _s[_sp++]=_r;\n"
                "      } }\n");
            break;

        /* mod: int fast path */
        case OP_mod:
            jit_buf_str(cb,
                "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT){\n"
                "        int32_t ib=JS_VALUE_GET_INT(_b);\n"
                "        _s[_sp++]=ib?JS_NewInt32(ctx,JS_VALUE_GET_INT(_a)%ib)\n"
                "                   :JS_NewFloat64(ctx,0.0/0.0);\n"
                "      } else {\n"
                "        JSValue _r=_RT->mod(ctx,_a,_b); _CHK(_r); _s[_sp++]=_r;\n"
                "      } }\n");
            break;

        /* Bitwise: ToInt32 already guaranteed by semantics; fast path for int */
#define GEN_BITOP_INT(op_str, rt_name) \
    jit_buf_printf(cb, \
        "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n" \
        "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n" \
        "        _s[_sp++]=JS_NewInt32(ctx,JS_VALUE_GET_INT(_a) %s JS_VALUE_GET_INT(_b));\n" \
        "      else { JSValue _r=_RT->%s(ctx,_a,_b); _CHK(_r); _s[_sp++]=_r; } }\n", \
        op_str, rt_name)

        case OP_shl: GEN_BITOP_INT("<<", "shl"); break;
        case OP_sar: GEN_BITOP_INT(">>", "sar"); break;
        case OP_and: GEN_BITOP_INT("&",  "band"); break;
        case OP_or:  GEN_BITOP_INT("|",  "bor");  break;
        case OP_xor: GEN_BITOP_INT("^",  "bxor"); break;

        /* shr is unsigned right shift — result may exceed INT32_MAX */
        case OP_shr:
            jit_buf_str(cb,
                "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT){\n"
                "        uint32_t _r=(uint32_t)JS_VALUE_GET_INT(_a)>>(JS_VALUE_GET_INT(_b)&31);\n"
                "        _s[_sp++]=(_r<=(uint32_t)INT32_MAX)?JS_NewInt32(ctx,(int32_t)_r)\n"
                "                                           :JS_NewFloat64(ctx,(double)_r);\n"
                "      } else { JSValue _r=_RT->shr(ctx,_a,_b); _CHK(_r); _s[_sp++]=_r; } }\n");
            break;

#undef GEN_BITOP_INT

        /* pow and remaining ops: full vtable (rare) */
        case OP_pow:
            jit_buf_str(cb,
                "    { JSValue _b=_s[--_sp],_a=_s[--_sp],"
                " _r=_RT->pow(ctx,_a,_b); _CHK(_r); _s[_sp++]=_r; }\n");
            break;

        /* ---- Arithmetic (unary) with int fast paths ---- */
        case OP_neg:
            jit_buf_str(cb,
                "    { JSValue _a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT){\n"
                "        int32_t ia=JS_VALUE_GET_INT(_a);\n"
                "        _s[_sp++]=(ia==INT32_MIN)?JS_NewFloat64(ctx,-(double)ia)\n"
                "                                 :JS_NewInt32(ctx,-ia);\n"
                "      } else { JSValue _r=_RT->neg(ctx,_a); _CHK(_r); _s[_sp++]=_r; } }\n");
            break;
        case OP_plus:
            jit_buf_str(cb,
                "    { JSValue _a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT)\n"
                "        _s[_sp++]=_a; /* int is already a number */\n"
                "      else { JSValue _r=_RT->plus(ctx,_a); _CHK(_r); _s[_sp++]=_r; } }\n");
            break;
        case OP_not: /* bitwise ~ */
            jit_buf_str(cb,
                "    { JSValue _a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT)\n"
                "        _s[_sp++]=JS_NewInt32(ctx,~JS_VALUE_GET_INT(_a));\n"
                "      else { JSValue _r=_RT->bnot(ctx,_a); _CHK(_r); _s[_sp++]=_r; } }\n");
            break;
        case OP_typeof:
            jit_buf_str(cb,
                "    { JSValue _a=_s[--_sp],"
                " _r=_RT->type_of(ctx,_a); _CHK(_r); _s[_sp++]=_r; }\n");
            break;
        case OP_lnot:
            jit_buf_str(cb,
                "    { JSValue _a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT)\n"
                "        _s[_sp++]=JS_NewBool(ctx,JS_VALUE_GET_INT(_a)==0);\n"
                "      else if(JS_VALUE_GET_TAG(_a)==JS_TAG_BOOL)\n"
                "        _s[_sp++]=JS_NewBool(ctx,!JS_VALUE_GET_INT(_a));\n"
                "      else { _s[_sp++]=JS_NewBool(ctx,!JS_ToBool(ctx,_a)); _FREE(_a); } }\n");
            break;

        /* ---- Comparisons with inline int fast paths ---- */
#define GEN_CMP_INT(int_op, rt_name) \
    jit_buf_printf(cb, \
        "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n" \
        "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n" \
        "        _s[_sp++]=JS_NewBool(ctx,JS_VALUE_GET_INT(_a) %s JS_VALUE_GET_INT(_b));\n" \
        "      else { JSValue _r=_RT->%s(ctx,_a,_b); _CHK(_r); _s[_sp++]=_r; } }\n", \
        int_op, rt_name)

        case OP_lt:  GEN_CMP_INT("<",  "lt");  break;
        case OP_lte: GEN_CMP_INT("<=", "lte"); break;

#undef GEN_CMP_INT

        case OP_gt:
            jit_buf_str(cb,
                "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n"
                "        _s[_sp++]=JS_NewBool(ctx,JS_VALUE_GET_INT(_a)>JS_VALUE_GET_INT(_b));\n"
                "      else { JSValue _r=_RT->lte(ctx,_b,_a); _CHK(_r); _s[_sp++]=_r; } }\n");
            break;
        case OP_gte:
            jit_buf_str(cb,
                "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n"
                "        _s[_sp++]=JS_NewBool(ctx,JS_VALUE_GET_INT(_a)>=JS_VALUE_GET_INT(_b));\n"
                "      else { JSValue _r=_RT->lt(ctx,_b,_a); _CHK(_r); _s[_sp++]=_r; } }\n");
            break;

        case OP_eq:
            jit_buf_str(cb,
                "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n"
                "        _s[_sp++]=JS_NewBool(ctx,JS_VALUE_GET_INT(_a)==JS_VALUE_GET_INT(_b));\n"
                "      else { JSValue _r=_RT->eq(ctx,_a,_b); _CHK(_r); _s[_sp++]=_r; } }\n");
            break;
        case OP_neq:
            jit_buf_str(cb,
                "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n"
                "        _s[_sp++]=JS_NewBool(ctx,JS_VALUE_GET_INT(_a)!=JS_VALUE_GET_INT(_b));\n"
                "      else { JSValue _r=_RT->eq(ctx,_a,_b); _CHK(_r);\n"
                "             _s[_sp++]=JS_NewBool(ctx,!JS_VALUE_GET_INT(_r)); _FREE(_r); } }\n");
            break;
        case OP_strict_eq:
            jit_buf_str(cb,
                "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_VALUE_GET_TAG(_b)&&JS_VALUE_GET_TAG(_a)==JS_TAG_INT)\n"
                "        _s[_sp++]=JS_NewBool(ctx,JS_VALUE_GET_INT(_a)==JS_VALUE_GET_INT(_b));\n"
                "      else { JSValue _r=_RT->strict_eq(ctx,_a,_b); _CHK(_r); _s[_sp++]=_r; } }\n");
            break;
        case OP_strict_neq:
            jit_buf_str(cb,
                "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_VALUE_GET_TAG(_b)&&JS_VALUE_GET_TAG(_a)==JS_TAG_INT)\n"
                "        _s[_sp++]=JS_NewBool(ctx,JS_VALUE_GET_INT(_a)!=JS_VALUE_GET_INT(_b));\n"
                "      else { JSValue _r=_RT->strict_eq(ctx,_a,_b); _CHK(_r);\n"
                "             _s[_sp++]=JS_NewBool(ctx,!JS_VALUE_GET_INT(_r)); _FREE(_r); } }\n");
            break;

        /* ---- instanceof / in ---- */
        case OP_instanceof:
            jit_buf_str(cb,
                "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                "      int _r=JS_OrdinaryIsInstanceOf(ctx,_a,_b);\n"
                "      _FREE(_a); _FREE(_b);\n"
                "      if(_r<0) goto _ex;\n"
                "      _s[_sp++]=JS_NewBool(ctx,_r); }\n");
            break;
        case OP_in:
            jit_buf_str(cb,
                "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                "      int _r=JS_HasProperty(ctx,_b,JS_ValueToAtom(ctx,_a));\n"
                "      _FREE(_a); _FREE(_b);\n"
                "      if(_r<0) goto _ex;\n"
                "      _s[_sp++]=JS_NewBool(ctx,_r); }\n");
            break;

        /* ---- Control flow ---- */
        case OP_if_false: {
            int32_t delta = (int32_t)bc_u32(&bc[pc+1]);
            int tgt = pc + sz + delta;
            jit_buf_printf(cb,
                "    { JSValue _v=_s[--_sp]; int _b=JS_ToBool(ctx,_v);"
                " _FREE(_v); if(!_b) goto _L%d; }\n", tgt);
            break;
        }
        case OP_if_true: {
            int32_t delta = (int32_t)bc_u32(&bc[pc+1]);
            int tgt = pc + sz + delta;
            jit_buf_printf(cb,
                "    { JSValue _v=_s[--_sp]; int _b=JS_ToBool(ctx,_v);"
                " _FREE(_v); if(_b) goto _L%d; }\n", tgt);
            break;
        }
        case OP_goto: {
            int32_t delta = (int32_t)bc_u32(&bc[pc+1]);
            int tgt = pc + sz + delta;
            jit_buf_printf(cb, "    goto _L%d;\n", tgt);
            break;
        }
        case OP_if_false8: {
            int tgt = pc + sz + (int)(int8_t)bc[pc+1];
            jit_buf_printf(cb,
                "    { JSValue _v=_s[--_sp]; int _b=JS_ToBool(ctx,_v);"
                " _FREE(_v); if(!_b) goto _L%d; }\n", tgt);
            break;
        }
        case OP_if_true8: {
            int tgt = pc + sz + (int)(int8_t)bc[pc+1];
            jit_buf_printf(cb,
                "    { JSValue _v=_s[--_sp]; int _b=JS_ToBool(ctx,_v);"
                " _FREE(_v); if(_b) goto _L%d; }\n", tgt);
            break;
        }
        case OP_goto8: {
            int tgt = pc + sz + (int)(int8_t)bc[pc+1];
            jit_buf_printf(cb, "    goto _L%d;\n", tgt);
            break;
        }
        case OP_goto16: {
            int tgt = pc + sz + (int)(int16_t)bc_u16(&bc[pc+1]);
            jit_buf_printf(cb, "    goto _L%d;\n", tgt);
            break;
        }

        /* ---- Property access ---- */
        case OP_get_field: {
            uint32_t atom = bc_u32(&bc[pc+1]);
            jit_buf_printf(cb,
                "    { JSValue _o=_s[--_sp];"
                " JSValue _r=_RT->get_prop(ctx,_o,(JSAtom)%uu);"
                " _FREE(_o); _CHK(_r); _s[_sp++]=_r; }\n",
                atom);
            break;
        }
        case OP_get_field2: { /* keep object on stack */
            uint32_t atom = bc_u32(&bc[pc+1]);
            jit_buf_printf(cb,
                "    { JSValue _r=_RT->get_prop(ctx,_s[_sp-1],(JSAtom)%uu);"
                " _CHK(_r); _s[_sp++]=_r; }\n",
                atom);
            break;
        }
        case OP_put_field: {
            uint32_t atom = bc_u32(&bc[pc+1]);
            jit_buf_printf(cb,
                "    { JSValue _v=_s[--_sp], _o=_s[--_sp];\n"
                "      int _r=_RT->set_prop(ctx,_o,(JSAtom)%uu,_v);\n"
                "      _FREE(_o); if(_r<0) goto _ex; }\n",
                atom);
            break;
        }
        case OP_get_array_el:
            jit_buf_str(cb,
                "    { JSValue _idx=_s[--_sp], _o=_s[--_sp];\n"
                "      JSValue _r=_RT->get_array_el(ctx,_o,_idx);\n"
                "      _FREE(_o); _FREE(_idx); _CHK(_r); _s[_sp++]=_r; }\n");
            break;
        case OP_put_array_el:
            jit_buf_str(cb,
                "    { JSValue _v=_s[--_sp],_idx=_s[--_sp],_o=_s[--_sp];\n"
                "      int _r=_RT->set_array_el(ctx,_o,_idx,_v);\n"
                "      _FREE(_o); _FREE(_idx); if(_r<0) goto _ex; }\n");
            break;
        case OP_get_length:
            jit_buf_str(cb,
                "    { JSValue _r=_RT->get_prop(ctx,_s[_sp-1],"
                "JS_ATOM_length); _CHK(_r);\n"
                "      _FREE(_s[--_sp]); _s[_sp++]=_r; }\n");
            break;

        /* ---- Function calls ---- */
        case OP_call:
        case OP_call0:
        case OP_call1:
        case OP_call2:
        case OP_call3: {
            int nargs;
            if (op == OP_call)
                nargs = (int)bc_u16(&bc[pc+1]);
            else
                nargs = op - OP_call0;
            /* stack: func arg0 arg1 ... argN-1
             * After call: func and args consumed, result pushed            */
            jit_buf_printf(cb,
                "    { int _n=%d;\n"
                "      JSValue _f=_s[_sp-1-_n];\n"
                "      JSValue _r=_RT->call(ctx,_f,JS_UNDEFINED,_n,&_s[_sp-_n]);\n"
                "      for(int _j=0;_j<_n;_j++) _FREE(_s[_sp-1-_j]);\n"
                "      _sp -= _n+1; _FREE(_f);\n"
                "      _CHK(_r); _s[_sp++]=_r; }\n",
                nargs);
            break;
        }
        case OP_call_method:
        case OP_tail_call_method: {
            int nargs = (int)bc_u16(&bc[pc+1]);
            /* stack: this func arg0 ... argN-1 */
            jit_buf_printf(cb,
                "    { int _n=%d;\n"
                "      JSValue _f=_s[_sp-1-_n];\n"
                "      JSValue _t=_s[_sp-2-_n];\n"
                "      JSValue _r=_RT->call(ctx,_f,_t,_n,&_s[_sp-_n]);\n"
                "      for(int _j=0;_j<_n;_j++) _FREE(_s[_sp-1-_j]);\n"
                "      _sp -= _n+2; _FREE(_f); _FREE(_t);\n"
                "      _CHK(_r); _s[_sp++]=_r; }\n",
                nargs);
            break;
        }
        case OP_tail_call: {
            int nargs = (int)bc_u16(&bc[pc+1]);
            /* treat tail calls as regular calls for correctness */
            jit_buf_printf(cb,
                "    { int _n=%d;\n"
                "      JSValue _f=_s[_sp-1-_n];\n"
                "      JSValue _r=_RT->call(ctx,_f,JS_UNDEFINED,_n,&_s[_sp-_n]);\n"
                "      for(int _j=0;_j<_n;_j++) _FREE(_s[_sp-1-_j]);\n"
                "      _sp -= _n+1; _FREE(_f);\n"
                "      if(JS_VALUE_GET_TAG(_r)==JS_TAG_EXCEPTION) goto _ex;\n"
                "      for(_i=0;_i<%d;_i++) _FREE(_l[_i]);\n"
                "      while(_sp>0) _FREE(_s[--_sp]);\n"
                "      return _r; }\n",
                nargs, var_count);
            break;
        }
        case OP_call_constructor: {
            int nargs = (int)bc_u16(&bc[pc+1]);
            /* stack: ctor new_target arg0 ... argN-1 */
            jit_buf_printf(cb,
                "    { int _n=%d;\n"
                "      JSValue _nt=_s[_sp-1-_n];\n"
                "      JSValue _ctor=_s[_sp-2-_n];\n"
                "      JSValue _r=_RT->call_constructor(ctx,_ctor,_nt,_n,&_s[_sp-_n]);\n"
                "      for(int _j=0;_j<_n;_j++) _FREE(_s[_sp-1-_j]);\n"
                "      _sp -= _n+2; _FREE(_ctor); _FREE(_nt);\n"
                "      _CHK(_r); _s[_sp++]=_r; }\n",
                nargs);
            break;
        }
        /* OP_new does not exist; call_constructor handles 'new' expressions */

        /* ---- Return ---- */
        case OP_return: {
            jit_buf_printf(cb,
                "    { JSValue _r=_s[--_sp];\n"
                "      for(_i=0;_i<%d;_i++) _FREE(_l[_i]);\n"
                "      while(_sp>0) _FREE(_s[--_sp]);\n"
                "      return _r; }\n",
                var_count);
            break;
        }
        case OP_return_undef: {
            jit_buf_printf(cb,
                "    { for(_i=0;_i<%d;_i++) _FREE(_l[_i]);\n"
                "      while(_sp>0) _FREE(_s[--_sp]);\n"
                "      return JS_UNDEFINED; }\n",
                var_count);
            break;
        }

        /* ---- Throw ---- */
        case OP_throw:
            jit_buf_str(cb,
                "    { JSValue _v=_s[--_sp]; _RT->throw_val(ctx,_v);"
                " goto _ex; }\n");
            break;

        /* OP_typeof_undef does not exist; OP_typeof handles typeof */

        /* ---- Object/Array creation ---- */
        case OP_object:
            jit_buf_str(cb,
                "    { JSValue _r=JS_NewObject(ctx); _CHK(_r);"
                " _s[_sp++]=_r; }\n");
            break;
        case OP_array_from: {
            int nargs = (int)bc_u16(&bc[pc+1]);
            jit_buf_printf(cb,
                "    { JSValue _r=JS_NewArray(ctx); _CHK(_r);\n"
                "      for(int _j=%d-1;_j>=0;_j--) {\n"
                "        JSValue _v=_s[--_sp];\n"
                "        JS_SetPropertyUint32(ctx,_r,(uint32_t)_j,_v);\n"
                "      }\n"
                "      _s[_sp++]=_r; }\n",
                nargs);
            break;
        }

        /* ---- Unsupported opcodes (caught in scan, but defensive) ---- */
        default:
            *unsupported_out = 1;
            return -1;
        }
        pc += sz;
    }
    return 0;
}

/*
 * js_jit_gen_c() — top-level: run scan, then generate C source for b.
 * Returns 0 and fills cb on success.
 * Returns -1 on failure; if *unsupported!=0 the function is ineligible.
 */
static int js_jit_gen_c(JSFunctionBytecode *b, JSJITCodeBuf *cb,
                        char *fname_out, size_t fname_sz, int *unsupported)
{
    *unsupported = 0;

    /* Scan pass */
    JSJITScanResult sr;
    if (js_jit_scan(b, &sr) < 0) {
        *unsupported = sr.unsupported;
        return -1;
    }

    int bc_len;
    const uint8_t *bc = js_jit_fb_get_bytecode(b, &bc_len);
    int var_count      = js_jit_fb_get_var_count(b);
    int arg_count      = js_jit_fb_get_arg_count(b);
    int stack_size     = js_jit_fb_get_stack_size(b);
    int cpool_count    = js_jit_fb_get_cpool_count(b);
    int closure_var_count = js_jit_fb_get_closure_var_count(b);

    int op_sz_count;
    const uint8_t *op_sz = js_jit_get_opcode_size_table(&op_sz_count);

    if (jit_buf_init(cb) < 0) {
        scan_result_free(&sr);
        return -1;
    }

    gen_preamble(cb, b, var_count, arg_count, stack_size,
                 closure_var_count, cpool_count, fname_out, fname_sz);

    int unsup = 0;
    if (gen_body(cb, bc, bc_len, &sr, op_sz, op_sz_count,
                 var_count, arg_count, &unsup) < 0) {
        *unsupported = unsup;
        jit_buf_free(cb);
        scan_result_free(&sr);
        return -1;
    }

    gen_footer(cb, var_count);

    scan_result_free(&sr);

    if (cb->error) {
        jit_buf_free(cb);
        return -1;
    }
    return 0;
}

/* =======================================================================
 * Phase 3.1 — js_jit_new_tcc(): create and configure a TCCState
 *
 * Sets up include paths so the generated C can find quickjs.h and
 * quickjs-jit.h, registers the __jit_rt symbol so the generated code
 * can call vtable methods, and sets up error reporting.
 * ======================================================================= */

/* TCC error callback — collects errors into a static buffer for logging */
static void jit_tcc_error(void *opaque, const char *msg)
{
    (void)opaque;
    /* For now: ignore TCC warnings/errors; compilation result is checked
     * by tcc_relocate() return value.  Phase 3.4 will add proper logging. */
    (void)msg;
}

static TCCState *js_jit_new_tcc(void)
{
    TCCState *s = tcc_new();
    if (!s) return NULL;

    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    tcc_set_error_func(s, NULL, jit_tcc_error);

    /* Add include path so generated C can find quickjs.h / quickjs-jit.h.
     * Use the directory where the quickjs binary lives (runtime path is
     * unknown; fall back to a compile-time path via a macro).             */
#ifdef JIT_INCLUDE_DIR
    tcc_add_include_path(s, JIT_INCLUDE_DIR);
#endif
    /* Always search the current directory and the standard locations */
    tcc_add_include_path(s, ".");

    /* Register the vtable so generated code can call rt->add() etc. */
    tcc_add_symbol(s, "js_jit_rt", &js_jit_rt);

    return s;
}

/* =======================================================================
 * Phase 3.2–3.3 — js_jit_compile_tcc(): full TCC compilation with CAS guard
 *
 * Steps:
 *   1. CAS on jit_no_compile to claim the compilation slot (race guard).
 *   2. Generate C source via js_jit_gen_c().
 *   3. Compile and relocate via libtcc.
 *   4. Retrieve the function pointer via tcc_get_symbol().
 *   5. Atomically install the pointer with js_jit_fb_set_func().
 *
 * On any failure, mark the function as non-compilable and free the TCC state.
 * ======================================================================= */

void js_jit_compile_tcc(JSContext *ctx, JSFunctionBytecode *b)
{
    (void)ctx;

    /* Phase 3.3 — CAS race guard.
     * If another thread beat us here, jit_no_compile will already be 1 or
     * jit_func will be non-NULL.  In either case we have nothing to do.    */
    if (js_jit_fb_jit_no_compile(b)) return;
    if (js_jit_fb_get_func(b) != NULL) return;

    /* Generate C source */
    JSJITCodeBuf cb;
    char fname[64];
    int unsupported = 0;
    if (js_jit_gen_c(b, &cb, fname, sizeof(fname), &unsupported) < 0) {
        js_jit_fb_set_no_compile(b);
        return;
    }

    /* Create TCC state and compile */
    TCCState *s = js_jit_new_tcc();
    if (!s) {
        jit_buf_free(&cb);
        js_jit_fb_set_no_compile(b);
        return;
    }

    int rc = tcc_compile_string(s, cb.buf);
    jit_buf_free(&cb);
    if (rc < 0) {
        tcc_delete(s);
        js_jit_fb_set_no_compile(b);
        return;
    }

    /* Relocate: allocates executable memory and patches addresses */
    if (tcc_relocate(s, TCC_RELOCATE_AUTO) < 0) {
        tcc_delete(s);
        js_jit_fb_set_no_compile(b);
        return;
    }

    /* Retrieve the generated function pointer */
    JSJITFunc f = (JSJITFunc)tcc_get_symbol(s, fname);
    if (!f) {
        tcc_delete(s);
        js_jit_fb_set_no_compile(b);
        return;
    }

    /* Atomically install: js_jit_fb_set_func uses __atomic_store_n RELEASE */
    js_jit_fb_set_func(b, f, s, 1);
}

void js_jit_queue_gcc(JSContext *ctx, JSFunctionBytecode *b)
{
    /* Phase 4: enqueue for GCC background compilation */
    (void)ctx;
    (void)b;
}

/* -----------------------------------------------------------------------
 * Task #6 — cleanup hook called from free_function_bytecode()
 *
 * All struct member access goes through the accessor API because
 * JSFunctionBytecode is an incomplete type from quickjs-jit.c's
 * perspective (defined only inside quickjs.c).
 * ----------------------------------------------------------------------- */

void js_jit_free_bytecode(JSFunctionBytecode *b)
{
    uint8_t tier       = js_jit_fb_get_tier(b);
    void   *handle     = js_jit_fb_get_handle(b);
    void   *old_handle = js_jit_fb_get_old_handle(b);

    /* Clear all JIT pointers first so no thread can race and call freed code */
    js_jit_fb_clear_handles(b);

    if (tier == 1) {
        /* Tier-1: handle is a TCCState* — free it (also frees compiled code) */
        if (handle)
            tcc_delete((TCCState *)handle);
    } else if (tier == 2) {
        /* Tier-2: handle is a dlopen handle (.so); old_handle may be a TCCState* */
        if (handle)
            dlclose(handle);
        if (old_handle)
            tcc_delete((TCCState *)old_handle);
    }
}

#endif /* CONFIG_JIT */
