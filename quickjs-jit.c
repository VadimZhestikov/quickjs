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
#include <ctype.h>
#include <dlfcn.h>

#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dlfcn.h>
/* P36.2: SIGPROF sampler */
#include <signal.h>
#include <ucontext.h>
#include <sys/time.h>

#include "quickjs.h"
#include "quickjs-jit.h"
#include "quickjs-opcode.h"

#define _STRINGIFY(x) #x
#define STRINGIFY(x)  _STRINGIFY(x)

/* Atom enum — needed to get numeric values of predefined atoms (e.g. JS_ATOM_length)
 * without pulling in the full quickjs.c internal headers.
 * IMPORTANT: must match the runtime enum in quickjs.c exactly.
 * The runtime enum starts with __JS_ATOM_NULL = 0 before the DEF entries,
 * so the first DEF entry (null) gets value 1, not 0.              */
#define DEF(name, str) JS_ATOM_##name,
typedef enum {
    __JIT_ATOM_NULL = 0, /* aligns with JS_ATOM_NULL = 0; DEF entries start at 1 */
#include "quickjs-atom.h"
    JS_ATOM__COUNT
} JSAtomEnumJIT;
#undef DEF

/* Build OP_* enum locally from quickjs-opcode.h, matching quickjs.c exactly.
 *
 * DEF() entries are real opcodes; def() entries are temporary phase-1 opcodes
 * that never appear in final bytecode.  We must keep def() BLANK here so the
 * enum values for SHORT_OPCODES entries (push_0, push_1, if_false8, …) match
 * the byte values that appear in the bytecode the JIT receives.
 *
 * The TEMP opcodes (enter_scope, leave_scope, label, scope_*, line_num) are
 * only present in intermediate bytecode and are removed before the final
 * JSFunctionBytecode is created — the JIT never sees them.
 */
#ifndef SHORT_OPCODES
#define SHORT_OPCODES 1
#define JIT_DEFINED_SHORT_OPCODES
#endif
typedef enum {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) OP_##id,
#define def(id, size, n_pop, n_push, f) /* temp — not in final bytecode */
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
    /* P32: use JS_CallConstructor2 to forward new_target correctly.
     * For plain `new Foo(args)`, new_target == ctor (same as JS_CallConstructor).
     * For `super(args)` in a derived class constructor, new_target is the
     * subclass being instantiated, not the superclass ctor — must be forwarded. */
    return JS_CallConstructor2(ctx, ctor, new_target, argc, argv);
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
    .pow              = js_jit_op_pow,
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
    .typeof_is_undefined = js_jit_op_typeof_is_undefined,
    .typeof_is_function  = js_jit_op_typeof_is_function,
    /* comparisons */
    .lt               = js_jit_op_lt,
    .lte              = js_jit_op_lte,
    .eq               = js_jit_op_eq,
    .strict_eq        = js_jit_op_strict_eq,
    /* ref counting */
    .dup              = jit_rt_dup,
    .free             = jit_rt_free,
    /* var ref accessor */
    .var_ref_value    = js_jit_var_ref_value,
    /* property access */
    .get_prop         = jit_rt_get_prop,
    .set_prop         = jit_rt_set_prop,
    .get_var_slow     = js_jit_op_get_var_slow,
    .put_var_slow     = js_jit_op_put_var_slow,
    .get_array_el     = jit_rt_get_array_el,
    .set_array_el     = jit_rt_set_array_el,
    /* property deletion — P16 */
    .delete_global_var = js_jit_op_delete_global_var,
    /* calls — P8.3: js_jit_call checks jit_func before falling to JS_Call */
    .call             = js_jit_call,
    .call_constructor = jit_rt_call_constructor,
    /* spread/apply — P17 */
    .apply            = js_jit_op_apply,
    .apply_eval       = js_jit_op_apply_eval,
    /* exceptions */
    .throw_type_error = jit_rt_throw_type_error,
    .throw_val        = jit_rt_throw_val,
    /* P8.2: interrupt poll for direct self-recursive calls */
    .poll_interrupts  = js_jit_poll_interrupts,
    /* P19: utility ops */
    .get_var_undef    = js_jit_op_get_var_undef,
    .throw_error      = js_jit_op_throw_error,
    .to_object        = js_jit_op_to_object,
    .to_propkey       = js_jit_op_to_propkey,
    .regexp           = js_jit_op_regexp,
    .set_name_computed = js_jit_op_set_name_computed,
    .set_proto        = js_jit_op_set_proto,
    .set_home_object  = js_jit_op_set_home_object,
    .get_array_el2    = js_jit_op_get_array_el2,
    .define_array_el  = js_jit_op_define_array_el,
    .push_bigint_i32  = js_jit_op_push_bigint_i32,
    .close_loc        = js_jit_op_close_loc,
    /* P20: ref-slot ops */
    .make_ref_pair    = js_jit_op_make_ref_pair,
    .make_var_ref     = js_jit_op_make_var_ref,
    .get_ref_value    = js_jit_op_get_ref_value,
    .put_ref_value    = js_jit_op_put_ref_value,
    /* P21: spread / rest / copy */
    .rest                  = js_jit_op_rest,
    .append                = js_jit_op_append,
    .copy_data_properties  = js_jit_op_copy_data_properties,
    /* P22: private fields */
    .private_symbol        = js_jit_op_private_symbol,
    .get_private_field     = js_jit_op_get_private_field,
    .put_private_field     = js_jit_op_put_private_field,
    .define_private_field  = js_jit_op_define_private_field,
    .private_in            = js_jit_op_private_in,
    /* P23: OOP / class (feasible subset) */
    .check_brand           = js_jit_op_check_brand,
    .add_brand             = js_jit_op_add_brand,
    .get_super_value       = js_jit_op_get_super_value,
    .put_super_value       = js_jit_op_put_super_value,
    .define_method         = js_jit_op_define_method,
    .define_method_computed = js_jit_op_define_method_computed,
    /* P26: constructor / class-definition */
    .check_ctor            = js_jit_op_check_ctor,
    .init_ctor             = js_jit_op_init_ctor,
    .define_class          = js_jit_op_define_class,
    .define_class_computed = js_jit_op_define_class_computed,
    /* P27: dynamic import */
    .import_op             = js_jit_op_import,
    /* P30: async for-of */
    .for_await_of_start    = js_jit_for_await_of_start,
    .for_await_of_next     = js_jit_for_await_of_next,
    /* P30: with_* object-environment lookup */
    .with_has              = js_jit_with_has,
    .with_get_var          = js_jit_with_get_var,
    .with_put_var          = js_jit_with_put_var,
    .with_delete_var       = js_jit_with_delete_var,
    .with_make_ref         = js_jit_with_make_ref,
    .with_get_ref          = js_jit_with_get_ref,
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

/* func_kind values matching JSFunctionKindEnum in quickjs.c */
#define JS_JIT_FUNC_NORMAL          0
#define JS_JIT_FUNC_GENERATOR       1  /* JS_FUNC_GENERATOR */
#define JS_JIT_FUNC_ASYNC           2  /* JS_FUNC_ASYNC — P12.3 */
#define JS_JIT_FUNC_ASYNC_GENERATOR 3  /* JS_FUNC_ASYNC_GENERATOR — P12.4 */

int js_jit_is_eligible(JSFunctionBytecode *b)
{
    uint8_t fk = js_jit_fb_func_kind(b);
    /* Normal, generator, async, and async generator functions are supported. */
    if (fk > JS_JIT_FUNC_ASYNC_GENERATOR)
        return 0;
    /* eval() has dynamic variable scoping — incompatible with JIT */
    if (js_jit_fb_is_eval(b))
        return 0;
    /* P33: complex params (destructuring, rest, default values) — now supported.
     * js_jit_call / js_jit_ic_direct_call pass real argc + all args for these
     * functions so OP_rest can see extras beyond b->arg_count. */
    /* P31: need_home_object — class methods using super.prop/super.method().
     * home_object is read by OP_special_object HOME_OBJECT via js_jit_special_object
     * which reads ctx->rt->current_stack_frame->cur_func->u.func.home_object.
     * No JIT changes needed — already handled. */
    /* P32: is_derived_ctor — derived class constructors.
     * super() calls use OP_call_constructor with correct new_target forwarded via
     * jit_rt_call_constructor → JS_CallConstructor2. No other changes needed. */
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
    /* P13: closure capture analysis (populated by js_jit_scan) */
    int       has_fclosure;        /* 1 if any OP_fclosure/fclosure8 found */
    uint64_t  captured_local_mask; /* bit i = local i captured by ≥1 inner closure */
    uint64_t  captured_arg_mask;   /* bit i = arg i captured by ≥1 inner closure */
    uint64_t  mutated_arg_mask;    /* bit i = arg i written by OP_put_arg/OP_set_arg */
    /* P14: try/catch/finally analysis */
    int       has_try;                    /* 1 if any OP_catch/gosub found */
    int       catch_handler_pcs[32];      /* target PC of each OP_catch handler */
    int       n_catch;                    /* number of OP_catch instructions */
    int       gosub_ret_pcs[32];          /* return PC (=gosub_pc+5) for each OP_gosub */
    int       n_gosub;                    /* number of OP_gosub instructions */
    /* P12: generator/async yield analysis */
    int       has_yield;   /* 1 if function has OP_yield/OP_initial_yield/OP_await */
    int       yield_count; /* number of OP_yield/OP_await sites (excluding initial_yield) */
    int       func_kind;   /* P12.3: JS_JIT_FUNC_{NORMAL,GENERATOR,ASYNC} */
    /* Stack slots live BELOW the yielded/awaited value at each yield site.
     * yield_below[0] = for _Lresume_0 (initial_yield → always 0)
     * yield_below[k] = for _Lresume_k (k=1..yield_count), value = (stack_depth-1) */
    int       yield_below[64]; /* saved stack slot count per resume label */
    int       max_below_yield; /* max of yield_below[] — extra saved_lv slots needed */
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
    /* P28: direct eval needs full scope-chain access — exclude rather than implement */
    case OP_eval:
        return 1;
    default:
        return 0;
    }
}

/* Forward declaration needed by js_jit_scan (full definition appears later). */
static inline uint16_t bc_u16(const uint8_t *p);

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
    sr->has_fclosure        = 0;
    sr->captured_local_mask = 0;
    sr->captured_arg_mask   = 0;
    sr->mutated_arg_mask    = 0;
    sr->has_try   = 0;
    sr->n_catch   = 0;
    sr->n_gosub   = 0;
    sr->has_yield   = 0;
    sr->yield_count = 0;
    sr->func_kind   = (int)js_jit_fb_func_kind(b);
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
        /* Extract branch target offsets.
         * QuickJS stores the offset relative to the FIRST OPERAND BYTE
         * (pc + 1), not the end of the instruction.  The interpreter
         * dispatches with opcode = *pc++ so pc is already at pc+1 when
         * the case body executes, and then does: pc += delta.
         * Therefore: target = (pc + 1) + delta.
         */
        switch (op) {
        case OP_if_false:
        case OP_if_true:
        case OP_goto: {
            int32_t delta = bc_get_i32(&bc[pc + 1]);
            int target = pc + 1 + delta;
            if (scan_add_target(sr, target, &cap) < 0) {
                scan_result_free(sr); return -1;
            }
            break;
        }
        case OP_gosub: {
            int32_t delta = bc_get_i32(&bc[pc + 1]);
            int sub_target = pc + 1 + delta;
            int ret_pc = pc + 5;  /* instruction after gosub (1 opcode + 4 operand bytes) */
            if (scan_add_target(sr, sub_target, &cap) < 0) {
                scan_result_free(sr); return -1;
            }
            /* return address is also a branch target (OP_ret jumps back to it) */
            if (scan_add_target(sr, ret_pc, &cap) < 0) {
                scan_result_free(sr); return -1;
            }
            if (sr->n_gosub < 32) sr->gosub_ret_pcs[sr->n_gosub++] = ret_pc;
            else { sr->unsupported = 1; scan_result_free(sr); return -1; }
            sr->has_try = 1;
            break;
        }
        case OP_catch: {
            int32_t delta = bc_get_i32(&bc[pc + 1]);
            int handler_pc = pc + 1 + delta;
            if (scan_add_target(sr, handler_pc, &cap) < 0) {
                scan_result_free(sr); return -1;
            }
            if (sr->n_catch < 32) sr->catch_handler_pcs[sr->n_catch++] = handler_pc;
            else { sr->unsupported = 1; scan_result_free(sr); return -1; }
            sr->has_try = 1;
            break;
        }
        case OP_if_false8:
        case OP_if_true8:
        case OP_goto8: {
            int target = pc + 1 + (int)bc_get_i8(&bc[pc + 1]);
            if (scan_add_target(sr, target, &cap) < 0) {
                scan_result_free(sr); return -1;
            }
            break;
        }
        case OP_goto16: {
            int target = pc + 1 + (int)bc_get_i16(&bc[pc + 1]);
            if (scan_add_target(sr, target, &cap) < 0) {
                scan_result_free(sr); return -1;
            }
            break;
        }
        default:
            break;
        }

        /* P20: OP_close_loc / OP_make_loc_ref / OP_make_arg_ref also need _sf_vrefs.
         * Set has_fclosure so the preamble declares _sf_vrefs even if no OP_fclosure
         * is present in this function (e.g. destructuring ref-pairs without closures). */
        if (op == OP_close_loc || op == OP_make_loc_ref || op == OP_make_arg_ref)
            sr->has_fclosure = 1;

        /* P13: detect OP_fclosure / OP_fclosure8 and compute captured-var masks. */
        if (op == OP_fclosure || op == OP_fclosure8) {
            /* Read cpool index (inline bc_u32 since bc_u32 is defined later in this file) */
            int cpool_idx = (op == OP_fclosure8) ? (int)bc[pc+1] :
                            (int)((uint32_t)bc[pc+1]|((uint32_t)bc[pc+2]<<8)|
                                  ((uint32_t)bc[pc+3]<<16)|((uint32_t)bc[pc+4]<<24));
            JSFunctionBytecode *b_inner = js_jit_cpool_get_fb(b, cpool_idx);
            if (!b_inner) {
                /* cpool entry is not a bytecode function — mark unsupported */
                sr->unsupported = 1;
                scan_result_free(sr);
                return -1;
            }
            int n_cv = js_jit_fb_get_closure_var_count(b_inner);
            for (int ci = 0; ci < n_cv; ci++) {
                int cv_type = js_jit_fb_get_inner_cv_type(b_inner, ci);
                int cv_vidx = js_jit_fb_get_inner_cv_var_idx(b_inner, ci);
                if (cv_type == JIT_CLOSURE_LOCAL) {
                    if (cv_vidx >= 64) { sr->unsupported = 1; scan_result_free(sr); return -1; }
                    sr->captured_local_mask |= (uint64_t)1 << cv_vidx;
                } else if (cv_type == JIT_CLOSURE_ARG) {
                    if (cv_vidx >= 64) { sr->unsupported = 1; scan_result_free(sr); return -1; }
                    sr->captured_arg_mask |= (uint64_t)1 << cv_vidx;
                } else if (cv_type == JIT_CLOSURE_REF || cv_type == JIT_CLOSURE_GLOBAL_REF) {
                    /* Pass-through: already have a JSVarRef* in var_refs[]. No capture mask needed. */
                } else {
                    /* GLOBAL_DECL, GLOBAL, MODULE_* — eval/module only, mark unsupported */
                    sr->unsupported = 1;
                    scan_result_free(sr);
                    return -1;
                }
            }
            sr->has_fclosure = 1;
        }

        /* P12: track generator yield/resume sites */
        if (op == OP_initial_yield) {
            sr->has_yield = 1;
        } else if (op == OP_yield) {
            sr->has_yield = 1;
            sr->yield_count++;
        } else if (op == OP_await) {
            /* P12.3: OP_await is the async equivalent of OP_yield */
            sr->has_yield = 1;
            sr->yield_count++;
        } else if (op == OP_yield_star || op == OP_async_yield_star) {
            /* P29: yield* / async yield* — same suspend/resume machinery as OP_yield */
            sr->has_yield = 1;
            sr->yield_count++;
        }

        /* Detect OP_put_arg / OP_set_arg: these write back to argv[N], which
         * violates the P10.3 calling convention (argv is borrowed, not owned).
         * Track the written arg indices so the code generator can use a local
         * _arg_cap_buf copy instead of modifying argv[] directly. */
        {
            int _maidx = -1;
            if      (op == OP_put_arg  || op == OP_set_arg)  _maidx = (int)bc_u16(&bc[pc+1]);
            else if (op == OP_put_arg0 || op == OP_set_arg0) _maidx = 0;
            else if (op == OP_put_arg1 || op == OP_set_arg1) _maidx = 1;
            else if (op == OP_put_arg2 || op == OP_set_arg2) _maidx = 2;
            else if (op == OP_put_arg3 || op == OP_set_arg3) _maidx = 3;
            if (_maidx >= 0 && _maidx < 64)
                sr->mutated_arg_mask |= (uint64_t)1 << _maidx;
        }

        /* P30: with_* — register conditional-jump target */
        if (op == OP_with_get_var || op == OP_with_put_var ||
            op == OP_with_delete_var || op == OP_with_make_ref ||
            op == OP_with_get_ref) {
            int32_t diff = bc_get_i32(&bc[pc + 5]);
            int target = pc + 5 + diff;
            if (scan_add_target(sr, target, &cap) < 0) {
                scan_result_free(sr); return -1;
            }
        }

        pc += sz;
    }
    /* P12.3/P12.4: async functions and async generators always need the generator
     * frame (for OP_return_async), even if they have no OP_await. */
    if (sr->func_kind == JS_JIT_FUNC_ASYNC || sr->func_kind == JS_JIT_FUNC_ASYNC_GENERATOR)
        sr->has_yield = 1;
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

/* =======================================================================
 * Phase 5 — Typed variable inference
 *
 * Forward abstract interpretation of the bytecode.  Determines which
 * local variable slots hold numeric (int or float) values throughout the
 * function's lifetime.  Such slots are represented as C `double` instead
 * of JSValue, eliminating the boxing/unboxing overhead on hot arithmetic
 * loops (e.g. sum_loop where s overflows int32 and falls back to vtable).
 *
 * Abstract types:
 *   JIT_T_JSVAL  — unknown, use JSValue _l[idx]
 *   JIT_T_NUMBER — always numeric, use double _ld[idx]
 *
 * Algorithm:
 *   1. Pre-pass: any local targeted by set_loc_uninitialized (TDZ marker)
 *      is forced to JSVAL — the JIT skips TDZ checks, so returning 0.0
 *      instead of throwing a ReferenceError would be wrong.
 *   2. Optimistic start: all remaining locals = NUMBER.
 *   3. Up to 3 forward passes: downgrade to JSVAL when a non-numeric
 *      source writes to a local (put_loc/set_loc/add_loc with JSVAL stack
 *      top).  Re-pass until fixpoint (handles loop back-edges one level
 *      deep).
 * ======================================================================= */

/* Helper: read little-endian integers from bytecode stream */
static inline uint32_t bc_u32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|
           ((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static inline uint16_t bc_u16(const uint8_t *p) {
    return (uint16_t)p[0]|((uint16_t)p[1]<<8);
}

#define JIT_T_JSVAL    0  /* unknown — always use JSValue */
#define JIT_T_NUMBER   1  /* provably always numeric — use C double */
#define JIT_T_INT      2  /* provably always integral — use int64_t _li[] */
/* P8.2 gen_st marker: the value in this stack slot came from a self-recursive
 * OP_get_var.  Used to detect self-recursive call patterns at codegen time so
 * the call can be emitted as a direct C function call instead of _RT->call. */
#define JIT_T_SELF_FUNC 3
/* P10.3: stack slot holds a known JIT function; gen_hsh[] carries the bc_hash */
#define JIT_T_JIT_FUNC  4

/*
 * jit_infer_types() — returns malloc'd uint8_t[var_count] or NULL.
 */
static uint8_t *jit_infer_types(const uint8_t *bc, int bc_len,
                                 const uint8_t *op_sz, int op_sz_count,
                                 int var_count, int stack_size)
{
    if (var_count <= 0) return NULL;

    uint8_t *lt = malloc(var_count);
    int stk_cap = (stack_size < 4 ? 4 : stack_size) + 8;
    uint8_t *st = malloc(stk_cap);
    if (!lt || !st) { free(lt); free(st); return NULL; }

    /* Optimistic start: assume all locals are INT (most specific type) */
    memset(lt, JIT_T_INT, var_count);

    /* No TDZ pre-pass: set_loc_uninitialized is a no-op for type inference.
     * Locals accessed via get_loc_check (might be in TDZ) are forced to JSVAL
     * in the main pass below, which is the only case where the NUMBER→double
     * optimization would break TDZ correctness. */

    /* Multi-pass forward walk until fixpoint */
    for (int pass = 0; pass < 3; pass++) {
        int changed = 0;
        memset(st, JIT_T_JSVAL, stk_cap);
        int sp = 0;

#define _TI_PUSH(t)  do { if (sp < stk_cap-1) st[sp++]=(uint8_t)(t); } while(0)
#define _TI_POP()    (sp > 0 ? st[--sp] : (uint8_t)JIT_T_JSVAL)
#define _TI_PEEK()   (sp > 0 ? st[sp-1] : (uint8_t)JIT_T_JSVAL)
#define _TI_DROPN(n) do { sp -= (n); if (sp < 0) sp = 0; } while(0)
/* Downgrade local[i] if written from a lower-tier source.
 * Hierarchy: JIT_T_INT(2) > JIT_T_NUMBER(1) > JIT_T_JSVAL(0).
 * A local is only as specific as its least-specific assignment. */
#define _TI_WRITE(i, t) do { \
    if ((i) >= 0 && (i) < var_count) { \
        uint8_t _nt = (uint8_t)(t); \
        if (_nt < lt[(i)]) { lt[(i)] = _nt; changed = 1; } \
    } \
} while(0)

        int pc = 0;
        while (pc < bc_len) {
            int op = bc[pc];
            if (op >= op_sz_count || op_sz[op] == 0) break;

            switch (op) {
            /* ---- Integer constant pushes → INT ---- */
            case OP_push_i32: case OP_push_i8: case OP_push_i16:
            case OP_push_0:   case OP_push_1:  case OP_push_2:  case OP_push_3:
            case OP_push_4:   case OP_push_5:  case OP_push_6:  case OP_push_7:
            case OP_push_minus1:
                _TI_PUSH(JIT_T_INT); break;

            /* ---- Non-numeric pushes → JSVAL ---- */
            case OP_push_false: case OP_push_true: case OP_push_empty_string:
            case OP_undefined:  case OP_null:      case OP_push_this:
            case OP_push_const: case OP_push_const8: case OP_push_atom_value:
                _TI_PUSH(JIT_T_JSVAL); break;

            /* ---- get_loc / get_loc_check: propagate local type ---- */
            /* Note: get_loc_check does a TDZ check at runtime.  For the JIT
             * we skip TDZ checks on NUMBER locals because:
             * (a) the function has been called 100+ times without hitting TDZ,
             * (b) a TDZ violation would have thrown in the interpreter first. */
            case OP_get_loc: case OP_get_loc_check: case OP_get_loc_checkthis: {
                int i = (int)bc_u16(&bc[pc+1]);
                _TI_PUSH(i >= 0 && i < var_count ? lt[i] : JIT_T_JSVAL); break;
            }
            case OP_get_loc8: {
                int i = (int)bc[pc+1];
                _TI_PUSH(i >= 0 && i < var_count ? lt[i] : JIT_T_JSVAL); break;
            }
            case OP_get_loc0: _TI_PUSH(lt[0]); break;
            case OP_get_loc1: _TI_PUSH(lt[1]); break;
            case OP_get_loc2: _TI_PUSH(lt[2]); break;
            case OP_get_loc3: _TI_PUSH(lt[3]); break;

            /* ---- External sources → JSVAL ---- */
            case OP_get_arg:  case OP_get_arg0:  case OP_get_arg1:
            case OP_get_arg2: case OP_get_arg3:
            case OP_get_var:
            case OP_get_var_ref:   case OP_get_var_ref_check:
            case OP_get_var_ref0:  case OP_get_var_ref1:
            case OP_get_var_ref2:  case OP_get_var_ref3:
                _TI_PUSH(JIT_T_JSVAL); break;

            /* ---- Property / array / call → JSVAL ---- */
            case OP_get_field:    _TI_DROPN(1); _TI_PUSH(JIT_T_JSVAL); break;
            case OP_get_field2:                 _TI_PUSH(JIT_T_JSVAL); break;
            case OP_get_array_el: _TI_DROPN(2); _TI_PUSH(JIT_T_JSVAL); break;
            case OP_get_length:   _TI_DROPN(1); _TI_PUSH(JIT_T_INT); break; /* P11.8: length is always int */
            case OP_object:                     _TI_PUSH(JIT_T_JSVAL); break;
            case OP_array_from: {
                int n = (int)bc_u16(&bc[pc+1]); _TI_DROPN(n); _TI_PUSH(JIT_T_JSVAL); break;
            }

            /* ---- Binary arithmetic: propagate most specific numeric type ---- */
            case OP_add: {
                /* add can produce a string when one operand is a string;
                 * only type as NUMBER when BOTH inputs are already numeric. */
                uint8_t b = _TI_POP(), a = _TI_POP();
                uint8_t r = (a >= JIT_T_NUMBER && b >= JIT_T_NUMBER)
                            ? (a == JIT_T_INT && b == JIT_T_INT ? JIT_T_INT : JIT_T_NUMBER)
                            : JIT_T_JSVAL;
                _TI_PUSH(r);
                break;
            }
            case OP_sub: case OP_mul: case OP_mod: {
                /* sub/mul/mod: NUMBER only when BOTH inputs are numeric.
                 * Half-typed (one JSVAL) → JSVAL result; gen_body half-typed
                 * fast path skips one tag check but produces JSValue output. */
                uint8_t b = _TI_POP(), a = _TI_POP();
                int an = (a >= JIT_T_NUMBER), bn = (b >= JIT_T_NUMBER);
                uint8_t r = (an && bn)
                            ? (a == JIT_T_INT && b == JIT_T_INT ? JIT_T_INT : JIT_T_NUMBER)
                            : JIT_T_JSVAL;
                _TI_PUSH(r);
                break;
            }
            case OP_div: {
                /* div always produces a number (even JSVAL inputs → NaN is a number). */
                _TI_DROPN(2); _TI_PUSH(JIT_T_NUMBER); break;
            }
            case OP_pow: _TI_DROPN(2); _TI_PUSH(JIT_T_JSVAL); break;

            /* ---- Unary numeric ---- */
            case OP_neg: case OP_plus: case OP_inc: case OP_dec: {
                uint8_t a = _TI_POP();
                _TI_PUSH(a >= JIT_T_NUMBER ? a : JIT_T_JSVAL); break;
            }
            case OP_post_inc: case OP_post_dec: {
                uint8_t a = _TI_POP();
                uint8_t r = a >= JIT_T_NUMBER ? a : JIT_T_JSVAL;
                _TI_PUSH(a); _TI_PUSH(r); break;
            }

            /* ---- Bitwise: and/or/xor/shl/sar/not always produce ToInt32 → INT
             * when both inputs are already INT.  If either input may be JSVAL
             * (e.g. a variable that holds a large integer or a non-integer) the
             * result must be NUMBER so the local that captures it is stored as a
             * double, which can faithfully represent values up to 2^53.  Using
             * INT (int64_t) for such a local truncates silently.
             * shr (>>>) produces ToUint32 which may exceed INT32_MAX → NUMBER. */
            case OP_and: case OP_or:  case OP_xor:
            case OP_shl: case OP_sar: {
                uint8_t _b = _TI_POP(), _a = _TI_POP();
                _TI_PUSH((_a == JIT_T_INT && _b == JIT_T_INT) ? JIT_T_INT : JIT_T_NUMBER);
                break;
            }
            case OP_shr: _TI_DROPN(2); _TI_PUSH(JIT_T_NUMBER); break;
            case OP_not: {
                uint8_t _a = _TI_POP();
                _TI_PUSH(_a == JIT_T_INT ? JIT_T_INT : JIT_T_NUMBER);
                break;
            }

            /* ---- Boolean / comparison / typeof → JSVAL ---- */
            /* P18: type-test ops: consume 1, push bool (JSVAL) */
            case OP_lnot: case OP_typeof:
            case OP_is_null: case OP_is_undefined: case OP_is_undefined_or_null:
            case OP_typeof_is_undefined: case OP_typeof_is_function:
                _TI_DROPN(1); _TI_PUSH(JIT_T_JSVAL); break;
            case OP_lt:  case OP_lte: case OP_gt:  case OP_gte:
            case OP_eq:  case OP_neq: case OP_strict_eq: case OP_strict_neq:
            case OP_instanceof: case OP_in: _TI_DROPN(2); _TI_PUSH(JIT_T_JSVAL); break;
            /* P13: fclosure pushes a new closure object (JSVAL) */
            case OP_fclosure:  _TI_PUSH(JIT_T_JSVAL); break;
            case OP_fclosure8: _TI_PUSH(JIT_T_JSVAL); break;
            /* P16: delete → bool (JSVAL) */
            case OP_delete:     _TI_DROPN(2); _TI_PUSH(JIT_T_JSVAL); break;
            case OP_delete_var: _TI_PUSH(JIT_T_JSVAL); break;
            /* P17: apply pops 3 (func/this/args), apply_eval pops 2 (func/args) */
            case OP_apply:      _TI_DROPN(3); _TI_PUSH(JIT_T_JSVAL); break;
            case OP_apply_eval: _TI_DROPN(2); _TI_PUSH(JIT_T_JSVAL); break;

            /* ---- put_loc: write local ← stack top (pops) ---- */
            case OP_put_loc: case OP_put_loc_check: case OP_put_loc_check_init:
                _TI_WRITE((int)bc_u16(&bc[pc+1]), _TI_POP()); break;
            case OP_put_loc8:  _TI_WRITE((int)bc[pc+1], _TI_POP()); break;
            case OP_put_loc0:  _TI_WRITE(0, _TI_POP()); break;
            case OP_put_loc1:  _TI_WRITE(1, _TI_POP()); break;
            case OP_put_loc2:  _TI_WRITE(2, _TI_POP()); break;
            case OP_put_loc3:  _TI_WRITE(3, _TI_POP()); break;

            /* ---- set_loc: peek-assign (top unchanged) ---- */
            case OP_set_loc:  _TI_WRITE((int)bc_u16(&bc[pc+1]), _TI_PEEK()); break;
            case OP_set_loc8: _TI_WRITE((int)bc[pc+1], _TI_PEEK()); break;
            case OP_set_loc0: _TI_WRITE(0, _TI_PEEK()); break;
            case OP_set_loc1: _TI_WRITE(1, _TI_PEEK()); break;
            case OP_set_loc2: _TI_WRITE(2, _TI_PEEK()); break;
            case OP_set_loc3: _TI_WRITE(3, _TI_PEEK()); break;
            case OP_set_loc_uninitialized: break; /* no-op: TDZ guarded via get_loc_check */

            /* ---- inc_loc / dec_loc: in-place, keeps numeric type ---- */
            case OP_inc_loc: case OP_dec_loc: break;

            /* ---- add_loc: local += pop; downgrade if non-numeric ---- */
            case OP_add_loc: _TI_WRITE((int)bc[pc+1], _TI_POP()); break;

            /* ---- Stack manipulation ---- */
            case OP_nop: break;
            case OP_drop: _TI_DROPN(1); break;
            case OP_dup:  { uint8_t t = _TI_PEEK(); _TI_PUSH(t); break; }
            case OP_dup1: { /* a b → a a b */
                if (sp >= 2) {
                    uint8_t b = st[sp-1], a = st[sp-2];
                    if (sp < stk_cap-1) { st[sp-1] = a; st[sp] = b; sp++; }
                }
                break;
            }
            case OP_dup2: { /* a b → a b a b */
                uint8_t b = _TI_PEEK(), a = sp > 1 ? st[sp-2] : (uint8_t)JIT_T_JSVAL;
                _TI_PUSH(a); _TI_PUSH(b); break;
            }
            case OP_nip: { uint8_t b = _TI_POP(); _TI_DROPN(1); _TI_PUSH(b); break; }
            case OP_swap: {
                if (sp >= 2) { uint8_t t=st[sp-1]; st[sp-1]=st[sp-2]; st[sp-2]=t; }
                break;
            }
            case OP_rot3l: case OP_rot3r:
                if (sp>=1) st[sp-1]=JIT_T_JSVAL;
                if (sp>=2) st[sp-2]=JIT_T_JSVAL;
                if (sp>=3) st[sp-3]=JIT_T_JSVAL;
                break;
            case OP_insert2:
                if (sp>=1) st[sp-1]=JIT_T_JSVAL;
                if (sp>=2) st[sp-2]=JIT_T_JSVAL;
                _TI_PUSH(JIT_T_JSVAL); break;

            /* P18: new stack-shuffle ops */
            case OP_insert3: /* obj prop a -> a obj prop a: +1 */
                if (sp>=1) st[sp-1]=JIT_T_JSVAL;
                if (sp>=2) st[sp-2]=JIT_T_JSVAL;
                if (sp>=3) st[sp-3]=JIT_T_JSVAL;
                _TI_PUSH(JIT_T_JSVAL); break;
            case OP_insert4: /* this obj prop a -> a this obj prop a: +1 */
                if (sp>=1) st[sp-1]=JIT_T_JSVAL;
                if (sp>=2) st[sp-2]=JIT_T_JSVAL;
                if (sp>=3) st[sp-3]=JIT_T_JSVAL;
                if (sp>=4) st[sp-4]=JIT_T_JSVAL;
                _TI_PUSH(JIT_T_JSVAL); break;
            case OP_dup3: /* a b c -> a b c a b c: +3 */
                _TI_PUSH(JIT_T_JSVAL); _TI_PUSH(JIT_T_JSVAL); _TI_PUSH(JIT_T_JSVAL); break;
            case OP_nip1: /* a b c -> b c: -1 (removes 3rd-from-top) */
                if (sp>=3) { st[sp-3]=st[sp-2]; st[sp-2]=st[sp-1]; }
                _TI_DROPN(1); break;
            /* Depth-neutral shuffles: conservatively mark all involved slots JSVAL */
            case OP_perm3: /* 3 slots */
                if (sp>=1) st[sp-1]=JIT_T_JSVAL;
                if (sp>=2) st[sp-2]=JIT_T_JSVAL;
                if (sp>=3) st[sp-3]=JIT_T_JSVAL;
                break;
            case OP_perm4: case OP_rot4l: case OP_swap2: /* 4 slots */
                if (sp>=1) st[sp-1]=JIT_T_JSVAL;
                if (sp>=2) st[sp-2]=JIT_T_JSVAL;
                if (sp>=3) st[sp-3]=JIT_T_JSVAL;
                if (sp>=4) st[sp-4]=JIT_T_JSVAL;
                break;
            case OP_perm5: case OP_rot5l: /* 5 slots */
                if (sp>=1) st[sp-1]=JIT_T_JSVAL;
                if (sp>=2) st[sp-2]=JIT_T_JSVAL;
                if (sp>=3) st[sp-3]=JIT_T_JSVAL;
                if (sp>=4) st[sp-4]=JIT_T_JSVAL;
                if (sp>=5) st[sp-5]=JIT_T_JSVAL;
                break;

            /* ---- Arg/varref writes (no effect on locals) ---- */
            case OP_put_arg:  case OP_put_arg0: case OP_put_arg1:
            case OP_put_arg2: case OP_put_arg3: _TI_DROPN(1); break;
            case OP_set_arg:  case OP_set_arg0: case OP_set_arg1:
            case OP_set_arg2: case OP_set_arg3: break;
            case OP_put_var_ref: case OP_put_var_ref_check: case OP_put_var_ref_check_init:
            case OP_put_var_ref0: case OP_put_var_ref1:
            case OP_put_var_ref2: case OP_put_var_ref3:
            case OP_put_var: case OP_put_var_init: _TI_DROPN(1); break;
            case OP_set_var_ref:  case OP_set_var_ref0: case OP_set_var_ref1:
            case OP_set_var_ref2: case OP_set_var_ref3: break;

            /* ---- Property writes ---- */
            case OP_put_field:    _TI_DROPN(2); break;
            case OP_put_array_el: _TI_DROPN(3); break;
            case OP_define_field: _TI_DROPN(1); break;
            case OP_set_name: break; /* 1-in 1-out, type unchanged */

            /* ---- Branches ---- */
            case OP_if_false:  case OP_if_true:
            case OP_if_false8: case OP_if_true8: _TI_DROPN(1); break;
            case OP_goto: case OP_goto8: case OP_goto16: break;

            /* ---- P14: try/catch/finally ---- */
            case OP_catch:     _TI_PUSH(JIT_T_JSVAL); break; /* push placeholder */
            case OP_gosub:     _TI_PUSH(JIT_T_JSVAL); break; /* push return addr */
            case OP_ret:       _TI_DROPN(1); sp=0; break;    /* pops addr, jumps */
            case OP_nip_catch:
                /* Complex stack collapse: conservative reset to 1 JSVAL */
                sp = 0; _TI_PUSH(JIT_T_JSVAL); break;

            /* ---- P15: iterators ---- */
            /* for_in_start: 1-in 1-out (iter replaces obj) */
            case OP_for_in_start: break;
            /* for_in_next: keeps iter, pushes key+done (+2) */
            case OP_for_in_next:
                _TI_PUSH(JIT_T_JSVAL); _TI_PUSH(JIT_T_JSVAL); break;
            /* for_of_start: obj → iter next catch_offset (+2 net) */
            case OP_for_of_start:
                _TI_PUSH(JIT_T_JSVAL); _TI_PUSH(JIT_T_JSVAL); break;
            /* for_of_next: iter next catch_offset → +2 (value, done) */
            case OP_for_of_next:
                _TI_PUSH(JIT_T_JSVAL); _TI_PUSH(JIT_T_JSVAL); break;
            /* iterator_close: pops 3 (iter, next, catch_offset) */
            case OP_iterator_close:
                _TI_DROPN(3); break;
            /* iterator_check_object: 1-in 1-out (no change) */
            case OP_iterator_check_object: break;
            /* iterator_get_value_done: catch_offset obj → catch_offset value done (+1) */
            case OP_iterator_get_value_done:
                _TI_PUSH(JIT_T_JSVAL); break;
            /* iterator_next: 4-in 4-out (result replaces val, no net change) */
            case OP_iterator_next: break;
            /* iterator_call: 4-in 5-out (result replaces val, flag pushed +1) */
            case OP_iterator_call:
                _TI_PUSH(JIT_T_JSVAL); break;

            /* ---- P15b: special_object — pushes one JSVAL ---- */
            case OP_special_object: _TI_PUSH(JIT_T_JSVAL); break;

            /* ---- P12: generator yield/return opcodes ---- */
            /* OP_initial_yield: pops 0, pushes 0 — no stack effect */
            case OP_initial_yield: break;
            /* OP_yield: pops 1 (yield value), pushes 2 (next_val JSVAL, magic JSVAL) */
            case OP_yield:
                _TI_DROPN(1);
                _TI_PUSH(JIT_T_JSVAL); /* next_val */
                _TI_PUSH(JIT_T_JSVAL); /* magic */
                break;
            /* OP_await: pops 1 (awaited value), pushes 1 (resolved value JSVAL) */
            case OP_await:
                _TI_DROPN(1);
                _TI_PUSH(JIT_T_JSVAL);
                break;
            /* P29: OP_yield_star / OP_async_yield_star: pops 1, pushes 2 (same as OP_yield) */
            case OP_yield_star:
            case OP_async_yield_star:
                _TI_DROPN(1);
                _TI_PUSH(JIT_T_JSVAL); /* next_val */
                _TI_PUSH(JIT_T_JSVAL); /* magic */
                break;
            /* OP_return_async: pops 1 (return value), no push */
            case OP_return_async:
                _TI_DROPN(1); sp = 0; break;

            /* P30: for_await_of_start: obj → iter next catch_ph (+2 net) */
            case OP_for_await_of_start:
                _TI_PUSH(JIT_T_JSVAL); _TI_PUSH(JIT_T_JSVAL); break;
            /* P30: for_await_of_next: pushes promise (+1) */
            case OP_for_await_of_next:
                _TI_PUSH(JIT_T_JSVAL); break;
            /* P30: with_* — all fall-through paths pop obj (-1), found paths jump away */
            case OP_with_get_var:
            case OP_with_put_var:
            case OP_with_delete_var:
            case OP_with_make_ref:
            case OP_with_get_ref:
                _TI_DROPN(1); break;

            /* ---- Return / throw ---- */
            case OP_return: _TI_DROPN(1); sp = 0; break;
            case OP_return_undef: sp = 0; break;
            case OP_throw: _TI_DROPN(1); sp = 0; break;

            /* ---- Calls → JSVAL ---- */
            case OP_call: case OP_tail_call: {
                int n=(int)bc_u16(&bc[pc+1]); _TI_DROPN(n+1); _TI_PUSH(JIT_T_JSVAL); break;
            }
            case OP_call0: _TI_DROPN(1); _TI_PUSH(JIT_T_JSVAL); break;
            case OP_call1: _TI_DROPN(2); _TI_PUSH(JIT_T_JSVAL); break;
            case OP_call2: _TI_DROPN(3); _TI_PUSH(JIT_T_JSVAL); break;
            case OP_call3: _TI_DROPN(4); _TI_PUSH(JIT_T_JSVAL); break;
            case OP_call_method: case OP_tail_call_method: {
                int n=(int)bc_u16(&bc[pc+1]); _TI_DROPN(n+2); _TI_PUSH(JIT_T_JSVAL); break;
            }
            case OP_call_constructor: {
                int n=(int)bc_u16(&bc[pc+1]); _TI_DROPN(n+2); _TI_PUSH(JIT_T_JSVAL); break;
            }

            /* ---- P19: utility ops ---- */
            case OP_get_var_undef: _TI_PUSH(JIT_T_JSVAL); break;
            case OP_throw_error: break; /* no stack effect (always throws) */
            case OP_to_object: case OP_to_propkey:
                _TI_DROPN(1); _TI_PUSH(JIT_T_JSVAL); break;
            case OP_regexp: _TI_DROPN(2); _TI_PUSH(JIT_T_JSVAL); break;
            /* set_name_computed: func name → func name (no net change) */
            case OP_set_name_computed: break;
            /* set_proto: obj proto → obj (-1) */
            case OP_set_proto: _TI_DROPN(1); break;
            /* set_home_object: func home → func home (no net change) */
            case OP_set_home_object: break;
            /* get_array_el2: obj prop → obj val (net 0; prop consumed, val replaces it) */
            case OP_get_array_el2: _TI_DROPN(1); _TI_PUSH(JIT_T_JSVAL); break;
            /* get_array_el3: arr idx → arr idx result (+1) */
            case OP_get_array_el3: _TI_PUSH(JIT_T_JSVAL); break;
            /* define_array_el: arr prop val → arr prop (-1) */
            case OP_define_array_el: _TI_DROPN(1); break;
            case OP_push_bigint_i32: _TI_PUSH(JIT_T_JSVAL); break;
            case OP_close_loc: break; /* no stack effect */

            /* ---- P20: ref-slot ops ---- */
            /* make_loc_ref/make_arg_ref/make_var_ref/make_var_ref_ref: push 2 (obj, atom_val) */
            case OP_make_loc_ref: case OP_make_arg_ref:
            case OP_make_var_ref: case OP_make_var_ref_ref:
                _TI_PUSH(JIT_T_JSVAL); _TI_PUSH(JIT_T_JSVAL); break;
            /* get_ref_value: obj atom_val → obj atom_val result (+1) */
            case OP_get_ref_value: _TI_PUSH(JIT_T_JSVAL); break;
            /* put_ref_value: obj atom_val val → (pops all 3) */
            case OP_put_ref_value: _TI_DROPN(3); break;

            /* ---- P21: spread / rest / copy ---- */
            case OP_rest: _TI_PUSH(JIT_T_JSVAL); break;           /* push restArgs */
            case OP_append: _TI_DROPN(1); break;                   /* pop enumobj; array+pos stay */
            case OP_copy_data_properties: break;                   /* net 0 */

            /* ---- P22: private fields ---- */
            case OP_private_symbol: _TI_PUSH(JIT_T_JSVAL); break;
            case OP_get_private_field: _TI_DROPN(2); _TI_PUSH(JIT_T_JSVAL); break; /* net -1 */
            case OP_put_private_field: _TI_DROPN(3); break;        /* net -3 */
            case OP_define_private_field: _TI_DROPN(2); break;     /* net -2 */
            case OP_private_in: _TI_DROPN(2); _TI_PUSH(JIT_T_JSVAL); break; /* net -1 */

            /* ---- P23: OOP / class helpers (feasible) ---- */
            case OP_check_ctor_return: _TI_PUSH(JIT_T_JSVAL); break; /* net +1 */
            case OP_check_brand: break;                             /* net 0 (2-in 2-out) */
            case OP_add_brand: _TI_DROPN(2); break;                 /* net -2 */
            case OP_get_super: _TI_DROPN(1); _TI_PUSH(JIT_T_JSVAL); break; /* net 0 */
            case OP_get_super_value: _TI_DROPN(3); _TI_PUSH(JIT_T_JSVAL); break; /* net -2 */
            case OP_put_super_value: _TI_DROPN(4); break;          /* net -4 */
            case OP_define_method: _TI_DROPN(1); break;            /* net -1: func consumed, obj stays */
            case OP_define_method_computed: _TI_DROPN(2); break;   /* net -2 */

            /* ---- P26: constructor / class-definition ---- */
            case OP_check_ctor: break;                              /* net 0 */
            case OP_init_ctor: _TI_PUSH(JIT_T_JSVAL); break;       /* net +1 */
            case OP_define_class: break;                            /* net 0: 2-in 2-out */
            case OP_define_class_computed: break;                   /* net 0: 3-in 3-out */

            /* ---- P27: dynamic import ---- */
            case OP_import: _TI_DROPN(2); _TI_PUSH(JIT_T_JSVAL); break; /* net -1 */

            default: break;
            }

#undef _TI_PUSH
#undef _TI_POP
#undef _TI_PEEK
#undef _TI_DROPN
#undef _TI_WRITE

            pc += op_sz[op];
        }

        if (!changed) break; /* fixpoint */
    }

    free(st);
    return lt;
}

/* -----------------------------------------------------------------------
 * Phase 4 — GCC background worker
 *
 * Architecture:
 *   Main thread:   js_jit_queue_gcc() generates C source synchronously,
 *                  enqueues a JITGCCJob, and returns immediately.
 *   Worker thread: dequeues jobs, writes .c to /tmp, fork+execs gcc -O2,
 *                  dlopen()s the .so, atomically installs jit_func.
 *
 * Thread safety:
 *   jit_worker.lock protects the queue and stop flag.
 *   jit_func is stored with __ATOMIC_RELEASE / loaded with __ATOMIC_ACQUIRE.
 *   jit_no_compile is set before enqueueing — prevents duplicate jobs.
 *   js_jit_free() joins the worker, guaranteeing it is fully done before
 *   GC starts freeing JSFunctionBytecode objects.
 * ----------------------------------------------------------------------- */

/* Forward declaration: js_jit_gen_c is defined in the Phase 2 section below */
static int js_jit_gen_c(JSFunctionBytecode *b, JSJITCodeBuf *cb,
                         char *fname_out, size_t fname_sz, int *unsupported,
                         const char *js_func_name, uint64_t bc_hash,
                         JSRuntime *rt, const uint64_t *var_jit_hash);

typedef struct JITGCCJob {
    JSFunctionBytecode *b;
    char               *c_src;     /* malloc'd C source; freed after gcc    */
    char                fname[64]; /* symbol name to look up via dlsym      */
    char                js_name[80]; /* JS function name for registry/profile */
    uint64_t            bc_hash;   /* FNV-1a hash of bytecode + build stamp */
    int                 is_warm;   /* P45b: 1 = warm-IC recompile job       */
    struct JITGCCJob   *next;
} JITGCCJob;

static struct {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    pthread_cond_t  idle_cond;  /* signaled when head==NULL && !busy */
    JITGCCJob      *head;
    JITGCCJob      *tail;
    int             stop;
    int             started;
    int             busy;       /* 1 while compiling a job */
    int             ref_count; /* how many JSRuntime instances share this thread */
} jit_worker;

/* =======================================================================
 * Phase 7.3/7.4 — JIT cache helpers
 *
 * Cache key: FNV-1a 64-bit over bytecode bytes, then folded with a build
 * stamp (__DATE__ __TIME__) so the cache is automatically invalidated when
 * qjs is recompiled.
 *
 * Cache location: $QJS_JIT_CACHE env-var, falling back to ~/.cache/qjs-jit/
 * Cache file:     <dir>/<hash16hex>.so
 *
 * Write path (Phase 7.3): after GCC succeeds, copy .so → cache atomically.
 * Read path  (Phase 7.4): before generating C, dlopen cache hit directly.
 * ======================================================================= */

static const char jit_build_stamp[] = __DATE__ " " __TIME__;

static uint64_t jit_fnv1a_64(const void *data, size_t len, uint64_t hash)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= UINT64_C(0x00000100000001B3);
    }
    return hash;
}

static uint64_t jit_hash_bytecode(const uint8_t *bc, int bc_len)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);  /* FNV-1a offset basis */
    h = jit_fnv1a_64(bc, (size_t)bc_len, h);
    h = jit_fnv1a_64(jit_build_stamp, sizeof(jit_build_stamp) - 1, h);
    return h;
}

/* Hash a function's bytecode AND the closure-variable metadata of every inner
 * function referenced by OP_fclosure / OP_fclosure8.  Two functions can share
 * the same raw bytecode hash if their inner functions have different
 * closure_var_count or closure_var[].closure_type / var_idx values; including
 * this metadata ensures distinct hashes → distinct cache files.
 *
 * This replaces jit_hash_bytecode() at all callsites that have a live
 * JSFunctionBytecode* (i.e. everywhere except the P10.3 callee-hash path). */
static uint64_t jit_hash_function(JSFunctionBytecode *b)
{
    int bc_len;
    const uint8_t *bc = js_jit_fb_get_bytecode(b, &bc_len);
    uint64_t h = jit_hash_bytecode(bc, bc_len);

    /* Walk bytecode looking for OP_fclosure / OP_fclosure8 to hash inner
     * function closure-variable metadata. */
    int op_sz_count;
    const uint8_t *op_sz = js_jit_get_opcode_size_table(&op_sz_count);
    int pc = 0;
    while (pc < bc_len) {
        int op = bc[pc];
        if (op >= op_sz_count || op_sz[op] == 0) break;
        if (op == OP_fclosure || op == OP_fclosure8) {
            int cpool_idx = (op == OP_fclosure8) ? (int)bc[pc+1]
                                                 : (int)((uint32_t)bc[pc+1]
                                                        | ((uint32_t)bc[pc+2] << 8)
                                                        | ((uint32_t)bc[pc+3] << 16)
                                                        | ((uint32_t)bc[pc+4] << 24));
            JSFunctionBytecode *bi = js_jit_cpool_get_fb(b, cpool_idx);
            if (bi) {
                int n_cv = js_jit_fb_get_closure_var_count(bi);
                /* Hash: (pc, cpool_idx, n_cv) as 3 x int32 */
                uint32_t meta[3] = { (uint32_t)pc, (uint32_t)cpool_idx, (uint32_t)n_cv };
                h = jit_fnv1a_64(meta, sizeof(meta), h);
                for (int ci = 0; ci < n_cv; ci++) {
                    uint32_t cv[2] = {
                        (uint32_t)js_jit_fb_get_inner_cv_type(bi, ci),
                        (uint32_t)js_jit_fb_get_inner_cv_var_idx(bi, ci)
                    };
                    h = jit_fnv1a_64(cv, sizeof(cv), h);
                }
            }
        }
        pc += op_sz[op];
    }
    return h;
}

static char jit_cache_dir[512];
static int  jit_cache_enabled;
static int  jit_aot_mode_active;

void js_jit_set_aot_mode(int active) { jit_aot_mode_active = active; }
int  js_jit_get_aot_mode(void)       { return jit_aot_mode_active; }

static void jit_cache_init(void)
{
    const char *env = getenv("QJS_JIT_CACHE");
    if (env && *env) {
        snprintf(jit_cache_dir, sizeof(jit_cache_dir), "%s", env);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) return;
        snprintf(jit_cache_dir, sizeof(jit_cache_dir),
                 "%s/.cache/qjs-jit", home);
    }
    mkdir(jit_cache_dir, 0755); /* no-op if already exists */
    jit_cache_enabled = 1;
}

/* Returns malloc'd path to cached .so if it exists and is readable. */
static char *jit_cache_get(uint64_t hash)
{
    if (!jit_cache_enabled) return NULL;
    char path[600];
    snprintf(path, sizeof(path), "%s/%016llx.so",
             jit_cache_dir, (unsigned long long)hash);
    if (access(path, R_OK) == 0)
        return strdup(path);
    return NULL;
}

/* Returns 1 if a .skip marker exists for this hash (function was permanently
 * excluded from JIT on a previous run due to an unsupported opcode). */
static int jit_cache_is_skip(uint64_t hash)
{
    if (!jit_cache_enabled) return 0;
    char path[600];
    snprintf(path, sizeof(path), "%s/%016llx.skip",
             jit_cache_dir, (unsigned long long)hash);
    return access(path, F_OK) == 0;
}

/* Forward declaration — defined later in P36.1 section. */
void jit_registry_add(uintptr_t func_ptr, uint64_t bc_hash, const char *name);

/* Write a .skip marker so future runs bypass code generation for this hash. */
static void jit_cache_put_skip(uint64_t hash)
{
    if (!jit_cache_enabled) return;
    char path[600];
    snprintf(path, sizeof(path), "%s/%016llx.skip",
             jit_cache_dir, (unsigned long long)hash);
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

/* Copy src_path to <cache_dir>/<hash>.so atomically via temp+rename. */
static void jit_cache_put(const char *src_path, uint64_t hash)
{
    if (!jit_cache_enabled) return;
    char dst_path[600], tmp_path[620];
    snprintf(dst_path, sizeof(dst_path), "%s/%016llx.so",
             jit_cache_dir, (unsigned long long)hash);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dst_path);

    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) return;
    int dst_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) { close(src_fd); return; }

    char buf[65536];
    ssize_t n;
    int ok = 1;
    while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
        if (write(dst_fd, buf, (size_t)n) != n) { ok = 0; break; }
    }
    close(src_fd);
    close(dst_fd);
    if (!ok || n < 0) { unlink(tmp_path); return; }
    rename(tmp_path, dst_path); /* atomic on same filesystem */
}

/* Copy src_path to <cache_dir>/<hash>.c — C source companion for the .so.
 * Non-atomic (supplementary artifact; correctness does not depend on it). */
static void jit_cache_put_c_src(const char *src_path, uint64_t hash)
{
    if (!jit_cache_enabled) return;
    char dst_path[600];
    snprintf(dst_path, sizeof(dst_path), "%s/%016llx.c",
             jit_cache_dir, (unsigned long long)hash);
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) return;
    int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) { close(src_fd); return; }
    char buf[65536];
    ssize_t n;
    while ((n = read(src_fd, buf, sizeof(buf))) > 0)
        write(dst_fd, buf, (size_t)n);
    close(src_fd);
    close(dst_fd);
}

/* Returns 1 if a cached .c source file exists for this hash. */
static int jit_cache_has_c_src(uint64_t hash)
{
    if (!jit_cache_enabled) return 0;
    char path[600];
    snprintf(path, sizeof(path), "%s/%016llx.c",
             jit_cache_dir, (unsigned long long)hash);
    return access(path, R_OK) == 0;
}

/* Returns malloc'd path to cached .c source, or NULL if absent. */
static char *jit_cache_get_c_src(uint64_t hash)
{
    if (!jit_cache_enabled) return NULL;
    char path[600];
    snprintf(path, sizeof(path), "%s/%016llx.c",
             jit_cache_dir, (unsigned long long)hash);
    if (access(path, R_OK) == 0)
        return strdup(path);
    return NULL;
}

/* Save JS source text to <cache>/<hash>.js (--jit-save-sources).
 * Skipped silently if source is unavailable or cache is disabled. */
static void jit_cache_put_js_src(uint64_t hash, const char *src, int src_len)
{
    if (!jit_cache_enabled || !src || src_len <= 0) return;
    char path[600];
    snprintf(path, sizeof(path), "%s/%016llx.js",
             jit_cache_dir, (unsigned long long)hash);
    if (access(path, F_OK) == 0) return;  /* already cached */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    write(fd, src, (size_t)src_len);
    close(fd);
}

static int  jit_dump_c_mode;     /* set by --jit-dump-c        */
static int  jit_save_sources;   /* set by --jit-save-sources  */

void js_jit_set_dump_c_mode(int active)   { jit_dump_c_mode = active; }
void js_jit_set_save_sources(int active)  { jit_save_sources = active; }

/* =======================================================================
 * P10.2/P10.4 — hash registry for --jit-link combiner and manifest install
 *
 * Every bc_hash + bytecode pointer seen in js_jit_queue_gcc() is recorded
 * unconditionally so that js_jit_install_combined() can locate bytecodes
 * by hash when patching jit_func pointers from the manifest.
 * js_jit_link() additionally uses jit_link_hashes to collect .c files.
 * ======================================================================= */
static int                  jit_link_mode;
static uint64_t            *jit_link_hashes;
static JSFunctionBytecode **jit_link_bytecodes;  /* P10.4: parallel to hashes */
static int                  jit_link_hash_count;
static int                  jit_link_hash_cap;
static void                *jit_combined_handle;    /* P10.4: dlopen handle for combined.so */
static JSJITManifestEntry  *jit_combined_manifest;  /* P10.4: manifest array inside combined.so */
static int                  jit_combined_count;     /* P10.4: manifest entry count */

/* -----------------------------------------------------------------------
 * Session map — per-test hash→bytecode table used to install GCC results
 * safely without accessing potentially-freed bytecode pointers from the
 * worker thread.
 *
 * The worker records (bc_hash, func_ptr, handle) in jit_pending_results
 * instead of writing directly to job->b.  The main thread calls
 * js_jit_install_results() after js_jit_drain() to look up live bytecodes
 * by hash and install the compiled function.
 *
 * When a bytecode is freed during execution (js_jit_free_bytecode),
 * it is removed from the session map.  Any pending result whose hash
 * is not found in the session map means the bytecode is gone; the
 * compiled .so handle is dlclose()'d to avoid a leak.
 * ----------------------------------------------------------------------- */

/* Per-test hash→bytecode table. Only accessed from the main thread. */
static struct {
    uint64_t            *hashes;
    JSFunctionBytecode **bytecodes;
    int count, cap;
} jit_session_map;

/* Register a bytecode in the session map when a GCC job is queued. */
static void jit_session_add(uint64_t hash, JSFunctionBytecode *b)
{
    if (jit_session_map.count == jit_session_map.cap) {
        int new_cap = jit_session_map.cap ? jit_session_map.cap * 2 : 64;
        uint64_t *ha = realloc(jit_session_map.hashes,
                               (size_t)new_cap * sizeof(*ha));
        JSFunctionBytecode **ba = realloc(jit_session_map.bytecodes,
                                          (size_t)new_cap * sizeof(*ba));
        if (!ha || !ba) return;   /* alloc failure: entry not added */
        jit_session_map.hashes    = ha;
        jit_session_map.bytecodes = ba;
        jit_session_map.cap       = new_cap;
    }
    jit_session_map.hashes[jit_session_map.count]    = hash;
    jit_session_map.bytecodes[jit_session_map.count] = b;
    jit_session_map.count++;
}

/* Called by js_jit_free_bytecode() when a bytecode is freed.
 * Marks the session entry as dead (sets bytecode pointer to NULL).
 * Only called from main thread (inside free_function_bytecode). */
static void jit_session_remove(JSFunctionBytecode *b)
{
    for (int i = 0; i < jit_session_map.count; i++) {
        if (jit_session_map.bytecodes[i] == b) {
            jit_session_map.bytecodes[i] = NULL;   /* mark dead */
            /* keep hash for deduplication; doesn't matter if bytecode freed */
            return;
        }
    }
}

static JSFunctionBytecode *jit_session_lookup(uint64_t hash)
{
    /* Return the LAST matching live entry (most recently queued wins). */
    for (int i = jit_session_map.count - 1; i >= 0; i--) {
        if (jit_session_map.hashes[i] == hash &&
            jit_session_map.bytecodes[i] != NULL)
            return jit_session_map.bytecodes[i];
    }
    return NULL;
}

static void jit_session_clear(void)
{
    free(jit_session_map.hashes);
    free(jit_session_map.bytecodes);
    jit_session_map.hashes    = NULL;
    jit_session_map.bytecodes = NULL;
    jit_session_map.count     = 0;
    jit_session_map.cap       = 0;
}

/* GCC results produced by the worker — consumed by js_jit_install_results(). */
typedef struct {
    uint64_t    bc_hash;
    char        fname[64];
    char        js_name[80]; /* JS function name for registry/profile */
    void       *handle;
    JSJITFunc   func;
    int         is_warm;     /* P45b: 1 = warm-IC recompile result */
} JITGCCResult;

static struct {
    pthread_mutex_t lock;
    JITGCCResult   *items;
    int count, cap;
} jit_pending_results = { PTHREAD_MUTEX_INITIALIZER };

static void jit_result_add(uint64_t bc_hash, const char *fname,
                            const char *js_name,
                            void *handle, JSJITFunc func, int is_warm)
{
    pthread_mutex_lock(&jit_pending_results.lock);
    if (jit_pending_results.count == jit_pending_results.cap) {
        int new_cap = jit_pending_results.cap ? jit_pending_results.cap * 2 : 16;
        JITGCCResult *a = realloc(jit_pending_results.items,
                                   (size_t)new_cap * sizeof(*a));
        if (!a) { pthread_mutex_unlock(&jit_pending_results.lock); return; }
        jit_pending_results.items = a;
        jit_pending_results.cap   = new_cap;
    }
    JITGCCResult *r = &jit_pending_results.items[jit_pending_results.count++];
    r->bc_hash = bc_hash;
    memcpy(r->fname, fname, sizeof(r->fname));
    strncpy(r->js_name, js_name ? js_name : "", sizeof(r->js_name) - 1);
    r->js_name[sizeof(r->js_name) - 1] = '\0';
    r->handle  = handle;
    r->func    = func;
    r->is_warm = is_warm;
    pthread_mutex_unlock(&jit_pending_results.lock);
}

/* Called from main thread after js_jit_drain().
 * Installs compiled GCC results into live bytecodes; skips and dlclose()s
 * results for bytecodes that were freed during execution.
 * Also clears the session map so stale bytecode pointers are not reused. */
void js_jit_install_results(void)
{
    /* Snapshot the results list under the lock. */
    pthread_mutex_lock(&jit_pending_results.lock);
    int n               = jit_pending_results.count;
    JITGCCResult *items = jit_pending_results.items;
    jit_pending_results.items = NULL;
    jit_pending_results.count = 0;
    jit_pending_results.cap   = 0;
    pthread_mutex_unlock(&jit_pending_results.lock);

    for (int i = 0; i < n; i++) {
        JITGCCResult *r = &items[i];
        JSFunctionBytecode *b = jit_session_lookup(r->bc_hash);
        if (b) {
            if (r->is_warm) {
                /* P45b: warm-IC recompile — replace jit_func but keep cold .so loaded.
                 * Old call ICs may still hold pointers into the cold .so; keeping it
                 * loaded ensures those pointers remain valid.  New direct calls through
                 * jit_func use the warm function immediately. */
                js_jit_fb_set_warm_handle(b, r->handle);
                js_jit_fb_set_warm_func(b, r->func);  /* atomic RELEASE store */
                jit_registry_add((uintptr_t)r->func, r->bc_hash, r->js_name);
            } else {
                /* Bytecode is still alive — normal cold install. */
                js_jit_fb_set_bc_hash(b, r->bc_hash);
                /* P10.3: mark safe — fresh-compiled .so always has the version symbol */
                js_jit_fb_set_p103_safe(b, 1);
                js_jit_fb_set_func(b, r->func, r->handle, 2);
                /* P36.1: register address for sampling profiler */
                jit_registry_add((uintptr_t)r->func, r->bc_hash, r->js_name);
            }
        } else {
            /* Bytecode was freed during execution — discard the .so. */
            if (r->handle)
                dlclose(r->handle);
        }
    }
    free(items);

    /* Clear session map: bytecode pointers become stale after FreeRuntime. */
    jit_session_clear();
}

void js_jit_set_link_mode(int active) { jit_link_mode = active; }

/* Record hash+bytecode unconditionally (both needed for P10.4 manifest install). */
static void jit_link_record(uint64_t hash, JSFunctionBytecode *b)
{
    if (jit_link_hash_count >= jit_link_hash_cap) {
        int new_cap = jit_link_hash_cap ? jit_link_hash_cap * 2 : 128;
        uint64_t *arr = realloc(jit_link_hashes,
                                (size_t)new_cap * sizeof(*arr));
        if (!arr) return;
        jit_link_hashes = arr;
        JSFunctionBytecode **brr = realloc(jit_link_bytecodes,
                                           (size_t)new_cap * sizeof(*brr));
        if (!brr) return;  /* hash array updated; bytecodes stays one version behind */
        jit_link_bytecodes = brr;
        jit_link_hash_cap = new_cap;
    }
    jit_link_hashes[jit_link_hash_count] = hash;
    jit_link_bytecodes[jit_link_hash_count] = b;
    jit_link_hash_count++;
}

/* Write src to a temp file with the given suffix; return malloc'd path. */
static char *jit_write_tmp(const char *src, const char *suffix)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/qjs_jit_XXXXXX%s", suffix);
    int fd = mkstemps(path, (int)strlen(suffix));
    if (fd < 0) return NULL;
    size_t len = strlen(src);
    ssize_t written = write(fd, src, len);
    close(fd);
    if (written != (ssize_t)len) { unlink(path); return NULL; }
    return strdup(path);
}

/* Execute one GCC job: compile C to .so, dlopen, install jit_func. */
static void jit_compile_gcc_job(JITGCCJob *job)
{
    char *c_path = jit_write_tmp(job->c_src, ".c");
    free(job->c_src);
    job->c_src = NULL;
    if (!c_path) goto fail;

    /* Build .so path next to the .c file */
    char so_path[256];
    snprintf(so_path, sizeof(so_path), "%s", c_path);
    char *dot = strrchr(so_path, '.');
    if (dot) strcpy(dot, ".so");

    /* Fork + exec gcc */
    pid_t pid = fork();
    if (pid < 0) { unlink(c_path); free(c_path); goto fail; }
    if (pid == 0) {
        /* Redirect gcc stdout/stderr to /tmp/qjs_jit_gcc.log for debugging */
        int logfd = open("/tmp/qjs_jit_gcc.log", O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (logfd >= 0) { dup2(logfd, 1); dup2(logfd, 2); close(logfd); }
        execl("/usr/bin/gcc", "gcc", "-O2", "-shared", "-fPIC",
              "-DCONFIG_JIT",
#ifdef JIT_INCLUDE_DIR
              "-I", JIT_INCLUDE_DIR,
#endif
              "-o", so_path, c_path, (char *)NULL);
        execlp("gcc", "gcc", "-O2", "-shared", "-fPIC",
               "-DCONFIG_JIT",
#ifdef JIT_INCLUDE_DIR
               "-I", JIT_INCLUDE_DIR,
#endif
               "-o", so_path, c_path, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int gcc_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    /* P10.1: cache .c source before unlinking the temp file.
     * P45b: skip disk-cache writes for warm-recompile jobs (hints are run-specific). */
    if (gcc_ok && !job->is_warm) jit_cache_put_c_src(c_path, job->bc_hash);
    if (!getenv("QJS_JIT_KEEP_C")) unlink(c_path);
    free(c_path);
    if (!gcc_ok) { unlink(so_path); goto fail; }

    /* Cache the compiled .so before unlinking (Phase 7.3).
     * P45b: warm recompile results are not cached — they're observation-specific. */
    if (!job->is_warm) jit_cache_put(so_path, job->bc_hash);

    /* Load the compiled .so; unlink immediately (kernel keeps it mapped).
     * RTLD_GLOBAL: exports this function's symbol (__jit_f_HASH) into the
     * process-wide namespace so that future caller .so files compiled with
     * P10.3 direct-call extern references can resolve this symbol at their
     * own dlopen(RTLD_NOW) time.  Without RTLD_GLOBAL the extern symbol is
     * invisible to other dlopen calls and they fail with RTLD_NOW. */
    void *handle = dlopen(so_path, RTLD_NOW | RTLD_GLOBAL);
    unlink(so_path);
    if (!handle) {
        /* dlopen failure is transient (missing callee symbol not yet loaded,
         * or OS error) — do NOT write .skip; we want to retry on future runs.
         * .skip is reserved for permanent codegen failures (GCC errors). */
        return;
    }

    JSJITFunc f = (JSJITFunc)(uintptr_t)dlsym(handle, job->fname);
    if (!f) {
        dlclose(handle);
        return;  /* transient — symbol mismatch; do not write .skip */
    }

    /* Record the result for the main thread to install safely.
     * The worker must NOT write to job->b: the bytecode may be freed by the
     * main thread (local closure goes out of scope) before the job completes.
     * js_jit_install_results() looks up the live bytecode by hash after drain. */
    jit_result_add(job->bc_hash, job->fname, job->js_name, handle, f, job->is_warm);
    return;
fail:
    /* P45b: don't mark warm-recompile failures as permanently skipped. */
    if (!job->is_warm)
        jit_cache_put_skip(job->bc_hash);
    /* job->b is NOT accessed here — the bytecode may already be freed. */
}

static void *jit_worker_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&jit_worker.lock);
    for (;;) {
        while (!jit_worker.stop && !jit_worker.head)
            pthread_cond_wait(&jit_worker.cond, &jit_worker.lock);
        if (jit_worker.stop && !jit_worker.head) break;
        JITGCCJob *job = jit_worker.head;
        jit_worker.head = job->next;
        if (!jit_worker.head) jit_worker.tail = NULL;
        jit_worker.busy = 1;
        pthread_mutex_unlock(&jit_worker.lock);
        jit_compile_gcc_job(job);
        free(job);

        pthread_mutex_lock(&jit_worker.lock);
        jit_worker.busy = 0;
        if (!jit_worker.head)
            pthread_cond_broadcast(&jit_worker.idle_cond);
    }
    pthread_mutex_unlock(&jit_worker.lock);
    return NULL;
}

/* Runtime override for JIT_THRESHOLD_GCC (set via --jit-threshold-gcc=N).
 * 0 = compile all static functions before first execution (AOT pre-pass).
 * N >= 1 = compile after N calls (default: JIT_THRESHOLD_GCC = 100). */
static int jit_threshold_gcc = JIT_THRESHOLD_GCC;

void js_jit_set_threshold(int n) { jit_threshold_gcc = n; }
int  js_jit_get_threshold(void)  { return jit_threshold_gcc; }

/* P35.1: bytecode size cap (set via --jit-max-bc=N).
 * 0 = no cap; >0 = skip functions whose bytecode exceeds N bytes. */
static int jit_max_bc_len = JIT_MAX_BC_LEN;

void js_jit_set_max_bc_len(int n) { jit_max_bc_len = n; }
int  js_jit_get_max_bc_len(void)  { return jit_max_bc_len; }

/*
 * P45b: enqueue a warm-IC recompile GCC job.
 * Called from js_jit_schedule_warm_recompile() after hints are copied to b.
 * Generates C source with INT-typed get_field paths (using b->jit_vt_hints)
 * and enqueues for background GCC compilation.  Uses a warm function symbol
 * name (bc_hash | high-bit) to distinguish the .so from the cold version.
 * Does NOT write to the disk cache.
 */
static void js_jit_queue_warm_gcc(JSContext *ctx, JSFunctionBytecode *b)
{
    if (!jit_worker.started) return;

    const char *js_name = js_jit_fb_get_func_name(JS_GetRuntime(ctx), b);
    uint64_t bc_hash    = js_jit_fb_get_bc_hash(b);
    if (!bc_hash) return;

    /* Warm symbol name uses high bit to avoid collision with cold .so.
     * This also prevents the disk cache from confusing warm and cold entries. */
    uint64_t warm_hash = bc_hash | 0x8000000000000000ULL;

    /* Generate C source with INT hints (b->jit_vt_hints non-NULL) */
    JSJITCodeBuf cb;
    char fname[64];
    int unsupported = 0;
    if (js_jit_gen_c(b, &cb, fname, sizeof(fname), &unsupported,
                     js_name, warm_hash, JS_GetRuntime(ctx), NULL) < 0) {
        return; /* codegen failed — not fatal, cold function keeps running */
    }

    JITGCCJob *job = malloc(sizeof(*job));
    if (!job) { jit_buf_free(&cb); return; }
    job->b       = b;
    job->c_src   = cb.buf;
    cb.buf       = NULL;
    job->bc_hash = bc_hash;     /* original hash: session map lookup finds b */
    memcpy(job->fname, fname, sizeof(job->fname));
    strncpy(job->js_name, js_name ? js_name : "", sizeof(job->js_name) - 1);
    job->js_name[sizeof(job->js_name) - 1] = '\0';
    job->is_warm = 1;
    job->next    = NULL;

    /* Session map: warm result lookup uses original bc_hash */
    jit_session_add(bc_hash, b);

    pthread_mutex_lock(&jit_worker.lock);
    if (jit_worker.tail) jit_worker.tail->next = job;
    else                 jit_worker.head = job;
    jit_worker.tail = job;
    pthread_cond_signal(&jit_worker.cond);
    pthread_mutex_unlock(&jit_worker.lock);
}

/*
 * P45b: warm-IC recompile scheduler.
 * Called from JS_CallInternal after JIT_WARM_THRESHOLD calls to a JIT function.
 * 1. dlsym __jit_vt_HASH from the cold .so to get observed val_tags.
 * 2. Copy hints to b->jit_vt_hints.
 * 3. Queue warm recompile via js_jit_queue_warm_gcc().
 */
void js_jit_schedule_warm_recompile(JSContext *ctx, JSFunctionBytecode *b)
{
    js_jit_fb_set_warm_done(b); /* prevent re-entry */

    uint64_t bc_hash = js_jit_fb_get_bc_hash(b);
    if (!bc_hash) return;

    void *handle = js_jit_fb_get_handle(b);
    if (!handle) return;

    uint16_t n_gf = js_jit_fb_get_n_gf(b);
    uint8_t  n_ae = js_jit_fb_get_n_ae(b);
    uint16_t n_pf = js_jit_fb_get_n_pf(b); /* P48 */
    uint16_t n_vr = js_jit_fb_get_n_vr(b); /* P49 */
    uint16_t n_pa = js_jit_fb_get_n_pa(b); /* P50 */
    uint16_t n_ad = js_jit_fb_get_n_ad(b); /* P51 */
    uint16_t n_pv = js_jit_fb_get_n_pv(b); /* P52 */
    int n_hints = (int)n_gf + (int)n_ae + (int)n_pf + (int)n_vr + (int)n_pa + (int)n_ad * 2 + (int)n_pv;
    if (n_hints == 0) return;

    /* Look up exported val_tag hints from the cold .so */
    char sym[80];
    snprintf(sym, sizeof(sym), "__jit_vt_%016llx", (unsigned long long)bc_hash);
    uint8_t *hints_in_so = (uint8_t *)(uintptr_t)dlsym(handle, sym);
    if (!hints_in_so) return; /* old .so without P45b/P46; no warm benefit */

    /* Only bother if at least one get_field/get_array_el was observed as INT.
     * Zeros that are uninitialised BSS are indistinguishable from JS_TAG_INT==0;
     * we accept this ambiguity: cold hints default to INT hint which is
     * correct for the benchmarks we're optimising. */
    int any_int = 0;
    for (int i = 0; i < n_hints; i++) {
        if (hints_in_so[i] == 0 /* JS_TAG_INT */) { any_int = 1; break; }
    }
    if (!any_int) return;

    /* Copy hints to stable heap buffer owned by the bytecode */
    uint8_t *hint_copy = malloc((size_t)n_hints);
    if (!hint_copy) return;
    memcpy(hint_copy, hints_in_so, (size_t)n_hints);
    js_jit_fb_set_vt_hints(b, hint_copy); /* frees old hints if any */

    /* Queue warm recompile in background GCC worker */
    js_jit_queue_warm_gcc(ctx, b);
}

void js_jit_init(void)
{
    if (jit_worker.started) {
        jit_worker.ref_count++;
        return;
    }
    jit_cache_init();
    pthread_mutex_init(&jit_worker.lock, NULL);
    pthread_cond_init(&jit_worker.cond, NULL);
    pthread_cond_init(&jit_worker.idle_cond, NULL);
    jit_worker.head = jit_worker.tail = NULL;
    jit_worker.stop = 0;
    jit_worker.busy = 0;
    jit_worker.ref_count = 1;
    if (pthread_create(&jit_worker.thread, NULL, jit_worker_thread, NULL) == 0)
        jit_worker.started = 1;
}

void js_jit_free(void)
{
    if (!jit_worker.started) return;
    if (--jit_worker.ref_count > 0) return;
    pthread_mutex_lock(&jit_worker.lock);
    jit_worker.stop = 1;
    pthread_cond_signal(&jit_worker.cond);
    pthread_mutex_unlock(&jit_worker.lock);
    pthread_join(jit_worker.thread, NULL);
    jit_worker.started = 0;
    /* Discard any jobs that were never processed (shouldn't occur) */
    JITGCCJob *job = jit_worker.head;
    while (job) {
        JITGCCJob *next = job->next;
        free(job->c_src);
        free(job);
        job = next;
    }
    jit_worker.head = jit_worker.tail = NULL;
    pthread_mutex_destroy(&jit_worker.lock);
    pthread_cond_destroy(&jit_worker.cond);
    pthread_cond_destroy(&jit_worker.idle_cond);
    /* P10.4: close combined.so handle if installed */
    if (jit_combined_handle) {
        dlclose(jit_combined_handle);
        jit_combined_handle   = NULL;
        jit_combined_manifest = NULL;
        jit_combined_count    = 0;
    }
}

/* Block until all enqueued GCC jobs have finished compiling.
 * Called from --jit-aot mode before executing the compiled program. */
void js_jit_drain(void)
{
    if (!jit_worker.started) return;
    pthread_mutex_lock(&jit_worker.lock);
    while (jit_worker.head || jit_worker.busy)
        pthread_cond_wait(&jit_worker.idle_cond, &jit_worker.lock);
    pthread_mutex_unlock(&jit_worker.lock);
}

/* Public API: returns 1 if a .c source file is cached for b's bytecode. */
int js_jit_cache_has_c_src(JSFunctionBytecode *b)
{
    return jit_cache_has_c_src(jit_hash_function(b));
}

/* P34.1 — public wrapper around jit_hash_function().
 * Returns the stable bytecode hash used for cache filenames and JIT dispatch
 * tables.  The hash covers raw bytecode bytes plus inner-function closure-var
 * metadata so that two functions with identical opcodes but different closure
 * layouts get distinct hashes. */
uint64_t js_jit_hash_bytecode_pub(JSFunctionBytecode *b)
{
    return jit_hash_function(b);
}

/* P34.2 — recursive bytecode walker internals.
 *
 * We need a visited set to handle shared inner functions: two sibling
 * closures may reference the same JSFunctionBytecode via their cpool, so
 * without deduplication the callback would fire multiple times for the same
 * bytecode object.
 *
 * Implementation: a flat pointer array grown with realloc, O(n) lookup.
 * Module functions typically have <200 inner functions so linear scan is fine.
 */
typedef struct {
    JSFunctionBytecode **ptr;
    int                  count;
    int                  cap;
} JITWalkVisited;

static int jit_walk_visited_has(JITWalkVisited *v, JSFunctionBytecode *b)
{
    for (int i = 0; i < v->count; i++)
        if (v->ptr[i] == b) return 1;
    return 0;
}

static int jit_walk_visited_add(JITWalkVisited *v, JSFunctionBytecode *b)
{
    if (v->count == v->cap) {
        int new_cap = v->cap ? v->cap * 2 : 16;
        JSFunctionBytecode **p = realloc(v->ptr,
                                         (size_t)new_cap * sizeof(*p));
        if (!p) return -1;
        v->ptr = p;
        v->cap = new_cap;
    }
    v->ptr[v->count++] = b;
    return 0;
}

static void jit_walk_rec(JSFunctionBytecode *b,
                         void (*cb)(JSFunctionBytecode *, void *),
                         void *opaque, JITWalkVisited *visited)
{
    if (!b) return;
    if (jit_walk_visited_has(visited, b)) return;
    if (jit_walk_visited_add(visited, b) < 0) return; /* OOM — skip subtree */

    cb(b, opaque);

    int n = js_jit_fb_get_cpool_count(b);
    for (int i = 0; i < n; i++) {
        JSFunctionBytecode *inner = js_jit_cpool_get_fb(b, i);
        if (inner)
            jit_walk_rec(inner, cb, opaque, visited);
    }
}

/* P34.2 — public API.
 * Calls cb(bytecode, opaque) once for b and each inner function reachable
 * through its constant pool, recursively.  Each distinct JSFunctionBytecode*
 * is visited exactly once even if shared by multiple closures.
 * cb is called in depth-first pre-order (outer before inner). */
void js_jit_walk_bytecodes(JSFunctionBytecode *b,
                           void (*cb)(JSFunctionBytecode *, void *),
                           void *opaque)
{
    if (!b || !cb) return;
    JITWalkVisited visited = {NULL, 0, 0};
    jit_walk_rec(b, cb, opaque, &visited);
    free(visited.ptr);
}

/* P34.3 — public convenience wrapper around js_jit_gen_c().
 *
 * Allocates a JSJITCodeBuf internally, runs code generation for b, and
 * returns the resulting C source as a malloc'd NUL-terminated string.
 * The caller must free() the returned pointer.
 *
 * On success  : returns the C source string; *fname_out filled with the
 *               C symbol name (e.g. "__jit_f_aabbccdd11223344").
 * On failure  : returns NULL.  Two distinct failure modes:
 *   unsupported opcode/feature — *unsupported set to 1, returns NULL.
 *   OOM                        — *unsupported left 0,  returns NULL.
 *
 * bc_hash must be js_jit_hash_bytecode_pub(b); it is passed explicitly so
 * the caller can compute it once and reuse it for the dispatch table. */
char *js_jit_gen_c_str(JSContext *ctx, JSFunctionBytecode *b,
                       uint64_t bc_hash,
                       char *fname_out, size_t fname_sz,
                       int *unsupported)
{
    JSJITCodeBuf cb;
    if (jit_buf_init(&cb) < 0) return NULL;

    const char *js_name = js_jit_fb_get_func_name(JS_GetRuntime(ctx), b);
    int ret = js_jit_gen_c(b, &cb, fname_out, fname_sz, unsupported,
                            js_name, bc_hash, JS_GetRuntime(ctx), NULL);
    if (ret < 0 || cb.error) {
        jit_buf_free(&cb);
        return NULL;
    }

    /* Transfer ownership of the buffer to the caller */
    char *src = cb.buf;
    cb.buf = NULL; /* prevent jit_buf_free from freeing it */
    jit_buf_free(&cb);
    return src;
}

void js_jit_queue_gcc(JSContext *ctx, JSFunctionBytecode *b, JSVarRef **var_refs)
{
    if (js_jit_fb_jit_no_compile(b)) return;
    if (js_jit_fb_get_func(b) != NULL) return;
    /* Claim the slot: no other thread or call will enqueue this function */
    js_jit_fb_set_no_compile(b);
    if (!jit_worker.started) return;

    /* P35.1: skip functions whose bytecode exceeds the size cap.
     * jit_no_compile is already set above, so this function is never retried.
     * Prevents runaway GCC compilation of large data-initialisation functions
     * (e.g. a 34 MB C file from a Unicode mapping table that takes 340 min). */
    if (jit_max_bc_len > 0) {
        int _bc_len;
        js_jit_fb_get_bytecode(b, &_bc_len);
        if (_bc_len > jit_max_bc_len) return;
    }

    /* Compute stable bytecode hash for cache lookup and symbol naming.
     * jit_hash_function() includes inner-function closure-var metadata so that
     * two functions sharing identical raw opcodes but different closure_var_count
     * or cv types get distinct hashes (avoiding stale-cache collisions). */
    uint64_t bc_hash = jit_hash_function(b);

    /* P36.4: get JS function name early — needed for registry entries on all paths. */
    const char *js_name = js_jit_fb_get_func_name(JS_GetRuntime(ctx), b);

    /* P10.2/P10.4: record hash+bytecode for link combiner and manifest install.
     * Skip in AOT mode (combined.so already loaded): bytecodes are per-test and
     * become stale after JS_FreeRuntime(); recording them would leave dangling
     * pointers in jit_link_bytecodes[] that corrupt jit_find_bytecode_by_hash. */
    if (!jit_combined_handle)
        jit_link_record(bc_hash, b);

    /* Skip marker: function had an unsupported opcode in a previous run.
     * Avoids re-running code generation and printing noisy messages. */
    if (jit_cache_is_skip(bc_hash)) return;

    /* P10.4: if combined.so is already open, check if this function is in the
     * manifest.  If so, install it from there and skip loading the individual
     * .so — avoids opening (and immediately closing) 500+ individual files.
     * If combined.so is open but the hash is NOT in the manifest, skip GCC
     * entirely: the function is absent from the combined library intentionally
     * (e.g. it had unsupported opcodes during the --jit-warmup run) and we
     * should not spawn a background GCC process during a production --jit-aot
     * run.  The function will be interpreted as usual. */
    if (jit_combined_handle && jit_combined_manifest) {
        for (int _mi = 0; _mi < jit_combined_count; _mi++) {
            if (jit_combined_manifest[_mi].bc_hash == bc_hash) {
                js_jit_fb_set_bc_hash(b, bc_hash);
                /* P10.3: combined.so is always freshly compiled with current
                 * codegen, so mutated_arg_mask protection is guaranteed. */
                js_jit_fb_set_p103_safe(b, 1);
                js_jit_fb_set_func(b, jit_combined_manifest[_mi].func_ptr, NULL, 2);
                /* P36.1: register address for sampling profiler (combined.so path) */
                jit_registry_add((uintptr_t)jit_combined_manifest[_mi].func_ptr,
                                 bc_hash, js_name);
                return;
            }
        }
        /* Not in combined.so — do not invoke GCC; fall back to interpreter. */
        return;
    }

    /* Phase 7.4: cache hit — load pre-compiled .so without running GCC.
     *
     * Guard: run the scan first to confirm this function is eligible.
     * bc_hash is computed from raw bytecode bytes only; two functions can share
     * a hash if they have identical opcodes but different closure-variable
     * metadata (e.g. one inner OP_fclosure8 captures a LOCAL, another captures
     * a GLOBAL).  Installing a .so compiled for function A into function B would
     * corrupt the JIT code (unhandled closure types leave _vr_PC[] uninitialised,
     * producing NULL var_refs entries that crash on OP_get_var). */
    int _cache_eligible = 0;
    {
        JSJITScanResult _sr_check;
        if (js_jit_scan(b, &_sr_check) == 0) _cache_eligible = 1;
        scan_result_free(&_sr_check);
    }
    char *cache_path = _cache_eligible ? jit_cache_get(bc_hash) : NULL;
    if (cache_path) {
        char fname[64];
        snprintf(fname, sizeof(fname), "__jit_f_%016llx",
                 (unsigned long long)bc_hash);
        /* RTLD_GLOBAL: make this symbol visible so callers with P10.3 direct
         * extern references can resolve it when they are dlopen'd later. */
        void *handle = dlopen(cache_path, RTLD_NOW | RTLD_GLOBAL);
        free(cache_path);
        if (handle) {
            JSJITFunc f = (JSJITFunc)(uintptr_t)dlsym(handle, fname);
            if (f) {
                /* P10.3/P44: check version symbol.
                 * If absent or mismatched, the .so was compiled by an older JIT
                 * with a different code generation strategy.  Reject and recompile
                 * so stale cache entries never silently execute wrong code. */
                char cv_sym[64];
                snprintf(cv_sym, sizeof(cv_sym), "__jit_cv_%016llx",
                         (unsigned long long)bc_hash);
                const uint32_t *cv = (const uint32_t *)(uintptr_t)dlsym(handle, cv_sym);
                if (!cv || *cv != JIT_CODEGEN_VERSION) {
                    /* Stale cache entry — close and fall through to recompile */
                    dlclose(handle);
                    goto do_compile;
                }
                js_jit_fb_set_bc_hash(b, bc_hash);
                js_jit_fb_set_p103_safe(b, 1);  /* version matched → safe */
                js_jit_fb_set_func(b, f, handle, 2);
                /* P36.1: register address for sampling profiler (cache-hit path) */
                jit_registry_add((uintptr_t)f, bc_hash, js_name);
                return;
            }
            dlclose(handle);
            /* Corrupted cache entry (symbol missing) — fall through to recompile */
        } else {
            /* dlopen failed — typically a P10.3 callee symbol not yet loaded via
             * RTLD_GLOBAL.  The .so itself is valid; do NOT recompile (that would
             * overwrite cache files and waste GCC cycles on every run).  Return and
             * let the interpreter handle this call; the function will be retried
             * from cache on the next js_jit_queue_gcc call for the same bytecode
             * (next runtime / next test). */
            return;
        }
    }

do_compile:;
    /* P10.3: resolve JIT callee hashes from closure variables for direct call emit */
    int p103_cvc = js_jit_fb_get_closure_var_count(b);
    uint64_t *p103_hash = NULL;
    if (var_refs && p103_cvc > 0) {
        p103_hash = (uint64_t *)calloc(p103_cvc, sizeof(uint64_t));
        if (p103_hash) {
            for (int _pi = 0; _pi < p103_cvc; _pi++) {
                if (!var_refs[_pi]) continue;
                JSValue *_pv = js_jit_var_ref_value(var_refs[_pi]);
                if (!_pv) continue;
                JSFunctionBytecode *_cb = js_jit_get_callee_fb(*_pv);
                if (!_cb) continue;
                int _cl;
                const uint8_t *_cc = js_jit_fb_get_bytecode(_cb, &_cl);
                uint64_t _ch = jit_hash_bytecode(_cc, _cl);
                if (_ch != bc_hash)   /* skip self (P8.2 handles self) */
                    p103_hash[_pi] = _ch;
            }
        }
    }

    JSJITCodeBuf cb;
    char fname[64];
    int unsupported = 0;
    if (js_jit_gen_c(b, &cb, fname, sizeof(fname), &unsupported,
                     js_name, bc_hash, JS_GetRuntime(ctx), p103_hash) < 0) {
        /* Persist the failure so future runs skip code generation silently. */
        free(p103_hash);
        jit_cache_put_skip(bc_hash);
        return;
    }
    free(p103_hash);

    JITGCCJob *job = malloc(sizeof(*job));
    if (!job) { jit_buf_free(&cb); return; }
    job->b       = b;
    /* P10.1-C: --jit-dump-c — print generated C to stdout for inspection */
    if (jit_dump_c_mode)
        fprintf(stdout, "/* ==== JIT: %s [%016llx] ==== */\n%s\n",
                js_name ? js_name : "(anon)", (unsigned long long)bc_hash, cb.buf);

    /* --jit-save-sources: write the original JS source to <cache>/<hash>.js */
    if (jit_save_sources) {
        int src_len = 0;
        const char *src = js_jit_fb_get_source(b, &src_len);
        jit_cache_put_js_src(bc_hash, src, src_len);
    }

    /* Record hash→bytecode in the session map so js_jit_install_results()
     * can find the live bytecode after drain without using job->b directly.
     * If the bytecode is freed before install time, js_jit_free_bytecode()
     * marks the entry dead (NULL) and install_results discards the result. */
    jit_session_add(bc_hash, b);

    job->c_src   = cb.buf;   /* transfer buffer ownership to job */
    cb.buf       = NULL;     /* prevent double-free if jit_buf_free is called */
    job->bc_hash = bc_hash;
    job->is_warm = 0;        /* P45b: must be explicit — malloc does not zero-init */
    memcpy(job->fname, fname, sizeof(job->fname));
    strncpy(job->js_name, js_name ? js_name : "", sizeof(job->js_name) - 1);
    job->js_name[sizeof(job->js_name) - 1] = '\0';
    job->next    = NULL;

    pthread_mutex_lock(&jit_worker.lock);
    if (jit_worker.tail) jit_worker.tail->next = job;
    else                 jit_worker.head = job;
    jit_worker.tail = job;
    pthread_cond_signal(&jit_worker.cond);
    pthread_mutex_unlock(&jit_worker.lock);
}

/* =======================================================================
 * Phase 2.3–2.11 — C code generator
 *
 * js_jit_gen_c(b, cb) translates JSFunctionBytecode *b into a C source
 * string in *cb.  The generated function has the JSJITFunc signature.
 *
 * Generated code model:
 *   JSValue _s[STACK_SIZE]       — evaluation stack (indexed by _sp)
 *   JSValue _jsv_<name>_<i>      — local variables (one per local, P9.1)
 *   int64_t _jsi_<name>_<i>      — INT-typed locals (P8.1/P9.1)
 *   double  _jsd_<name>_<i>      — NUMBER-typed locals (P5/P9.1)
 *   int32_t _jai_<name>_<i>      — integer arg fast-path (P8.4/P9.1)
 *   uint32_t _aim                 — arg-is-int bitmask (P8.4)
 *   JSVarRef **_vr               — aliases var_refs (closure variables)
 *   int _sp                      — runtime stack pointer
 *
 * Every operation that can throw appends "if(_sp_ok){goto _ex;}" via
 * the _CHK macro inside the generated code.  The _ex label frees all
 * live values and returns JS_EXCEPTION.
 *
 * Branch targets from the scan pass become C labels "_L<offset>:".
 * ======================================================================= */

/* P9.1 — Build variable name table.
 *
 * Returns a malloc'd array of (arg_count + var_count) C strings.
 * Entry [i] for i < arg_count: base name for argument i, e.g. "ax_0".
 * Entry [arg_count + j]: base name for local j, e.g. "count_3".
 * Non-ASCII, JS_ATOM_NULL, or keyword-colliding atoms fall back to
 * the numeric index, e.g. "3".
 * Caller must free each string and the array.  Returns NULL on malloc failure.
 */
static char **jit_build_varnames(JSRuntime *rt, JSFunctionBytecode *b,
                                  int arg_count, int var_count)
{
    int total = arg_count + var_count;
    char **names = (char **)calloc((size_t)total, sizeof(char *));
    if (!names) return NULL;
    char atom_buf[64];
    char buf[128];
    for (int i = 0; i < total; i++) {
        JSAtom atom = (i < arg_count)
            ? js_jit_fb_get_arg_atom(b, i)
            : js_jit_fb_get_local_atom(b, i - arg_count);
        const char *aname = NULL;
        if (atom != JS_ATOM_NULL)
            aname = js_jit_atom_get_str(rt, atom_buf, sizeof(atom_buf), atom);
        /* Sanitize JS name to a valid C identifier: $ → S, reject non-ASCII */
        char sane[64];
        int valid = 0;
        if (aname && aname[0]) {
            char c0 = (aname[0] == '$') ? 'S' : aname[0];
            if (isalpha((unsigned char)c0) || c0 == '_') {
                int slen = 0;
                sane[slen++] = c0;
                valid = 1;
                for (const char *p = aname + 1; *p && slen < (int)sizeof(sane)-1; p++) {
                    char c = (*p == '$') ? 'S' : *p;
                    if (!isalnum((unsigned char)c) && c != '_') { valid = 0; break; }
                    sane[slen++] = c;
                }
                sane[slen] = '\0';
            }
        }
        if (valid)
            snprintf(buf, sizeof(buf), "%s_%d", sane, i);
        else
            snprintf(buf, sizeof(buf), "%d", i);
        names[i] = strdup(buf);
        if (!names[i]) {
            for (int j = 0; j < i; j++) free(names[j]);
            free(names);
            return NULL;
        }
    }
    return names;
}

static void jit_free_varnames(char **names, int total)
{
    if (!names) return;
    for (int i = 0; i < total; i++) free(names[i]);
    free(names);
}

/*
 * Emit the C preamble: type definitions and the function signature.
 * The function symbol name encodes the bytecode hash so it is stable
 * across runs and can be looked up in a cached .so (Phase 7.3/7.4).
 */
static void gen_preamble(JSJITCodeBuf *cb, uint64_t bc_hash,
                         int var_count, int arg_count, int stack_size,
                         int closure_var_count, int cpool_count,
                         char *fname_out, size_t fname_sz,
                         const uint8_t *local_type,
                         const char *js_func_name,
                         char **varnames,
                         const JSJITScanResult *sr,
                         int var_ref_count,
                         JSFunctionBytecode *b,
                         int n_gf, int n_ae, int n_pf, int n_vr, int n_pa, int n_ad, int n_pv)
{
    /* Stable symbol name derived from bytecode hash */
    snprintf(fname_out, fname_sz, "__jit_f_%016llx",
             (unsigned long long)bc_hash);

    /* Debug: identify the JS source function */
    jit_buf_printf(cb, "/* JS function: %s */\n",
                   js_func_name ? js_func_name : "<unknown>");
    jit_buf_str(cb,
        "#include <stdint.h>\n"
        "#include <string.h>\n"
        "#include <math.h>\n"
        "#include <quickjs.h>\n"
        "#include <quickjs-jit.h>\n"
        /* quickjs.h undefines js_unlikely at its end (it's an internal macro
         * not intended for embedders).  Redefine it here for generated code. */
        "#ifdef __GNUC__\n"
        "#define js_unlikely(x) __builtin_expect(!!(x),0)\n"
        "#else\n"
        "#define js_unlikely(x) (x)\n"
        "#endif\n"
        "#define _RT  (&js_jit_rt)\n"
        /* JS_DupValue / JS_FreeValue are static inline in quickjs.h;
         * GCC will inline them entirely, eliminating vtable dispatch. */
        "#define _DUP(v)  JS_DupValue(ctx,(v))\n"
        "#define _FREE(v) JS_FreeValue(ctx,(v))\n"
        "#define _CHK(v)  do{if(JS_VALUE_GET_TAG(v)==JS_TAG_EXCEPTION)"
                          "goto _ex;}while(0)\n"
        /* Fast bool extraction: avoids external JS_ToBool call for the common
         * case where the top-of-stack is already JS_TAG_BOOL (result of lt/gt/eq).
         * For other tags JS_ToBool handles the general case.            */
        "#define _BOOL(v) (JS_VALUE_GET_TAG(v)==JS_TAG_BOOL"
                          "?JS_VALUE_GET_INT(v):JS_ToBool(ctx,(v)))\n"
    );

    /* P10.3: version marker — lets the loader detect old cached .so files
     * that lack mutated_arg_mask protection.  If this symbol is present and
     * equals JIT_CODEGEN_VERSION the function is marked jit_p103_safe=1. */
    jit_buf_printf(cb,
        "const uint32_t __jit_cv_%016llx=%uu;\n",
        (unsigned long long)bc_hash, JIT_CODEGEN_VERSION);

    /* P45b/P46/P48/P49/P50: exported val_tag hints array.
     * Layout: [0..n_gf-1]                           = OP_get_field hints,
     *         [n_gf..n_gf+n_ae-1]                   = OP_get_array_el hints,
     *         [n_gf+n_ae..+n_pf-1]                  = OP_put_field write-value (P48),
     *         [n_gf+n_ae+n_pf..+n_vr-1]             = OP_get_var_ref* cell (P49),
     *         [n_gf+n_ae+n_pf+n_vr..+n_pa-1]        = OP_put_array_el write-value (P50),
     *         [n_gf+n_ae+n_pf+n_vr+n_pa..+n_ad*2-1] = OP_add operand tags, 2 per site (P51),
     *         [n_gf+n_ae+n_pf+n_vr+n_pa+n_ad*2..+n_pv-1] = OP_put/set_var_ref* old-value (P52).
     * Updated at runtime; dlsym'd by js_jit_schedule_warm_recompile() after
     * JIT_WARM_THRESHOLD_GCC calls.  BSS-zero; 0 == JS_TAG_INT (default hint).
     * Only emitted when the function has at least one tracked opcode. */
    int n_hints = n_gf + n_ae + n_pf + n_vr + n_pa + n_ad * 2 + n_pv;
    if (n_hints > 0)
        jit_buf_printf(cb,
            "uint8_t __jit_vt_%016llx[%d];\n",
            (unsigned long long)bc_hash, n_hints);

    /* Function signature */
    jit_buf_printf(cb,
        "JSValue %s(\n"
        "    JSContext *ctx, JSValue this_val,\n"
        "    int argc, JSValue *argv,\n"
        "    JSValue *cpool, JSVarRef **var_refs)\n"
        "{\n",
        fname_out);

    /* P9.2: named temp stack slots — declare _tsv0.._tsv{stack_size-1}.
     * P43.5: initialized to JS_UNDEFINED so the _ex cleanup path can safely
     * call JS_FreeValue on any slot when _sp>N.  INT fast paths (OP_and/or/xor/
     * shl/sar/not, add_i, push_i32, get_loc_i, etc.) write _ti{N} without
     * writing _tsv{N} but still increment _sp; if an exception fires while such
     * a slot is live the cleanup reads _tsv{N} for JS_FreeValue.
     * JS_UNDEFINED is a no-op for JS_FreeValue, so initializing to it prevents
     * UAF/heap-corruption from uninitialized stack garbage being treated as a
     * live refcounted JSValue.  ASAN masks the bug (different allocator layout);
     * non-ASAN reproduces as a RayTrace/DeltaBlue incorrect-property-read caused
     * by the heap corruption on the first exception path taken after bitop INT
     * fast paths. */
    for (int j = 0; j < stack_size; j++)
        jit_buf_printf(cb, "    JSValue _tsv%d=JS_UNDEFINED;\n", j);
    /* P9.4: raw double temporaries for JIT_T_NUMBER stack slots.
     * P11.2: not initialized — always assigned before use by the type system. */
    for (int j = 0; j < stack_size; j++)
        jit_buf_printf(cb, "    double _tsd%d;\n", j);
    /* P11.6: raw int64 temporaries for JIT_T_INT stack slots.
     * Avoids float conversion; value in _ti when gen_st[slot]==JIT_T_INT. */
    for (int j = 0; j < stack_size; j++)
        jit_buf_printf(cb, "    int64_t _ti%d;\n", j);
    /* _sp is still needed: updated at throw/exception sites so the _ex
     * cleanup knows which _tsv{} slots are live. */
    jit_buf_str(cb, "    int _sp=0;\n");
    /* P37.2: capture runtime pointer once at function entry.
     * JIT_IC_CHECK_FAST uses _rt for a single pointer comparison instead of
     * the 3-condition runtime guard in JIT_IC_CHECK (null check + pointer +
     * rt_gen).  Safe: the runtime never changes during a single invocation. */
    jit_buf_str(cb, "    JSRuntime *_rt=JS_GetRuntime(ctx);\n");
    jit_buf_str(cb, "    (void)argc; (void)cpool; (void)var_refs;\n");

    /* P14: try/catch/finally support.
     * _tsvp[]: pointer array enabling runtime-indexed slot access at _ex dispatch.
     * _catch_depth / _catch_sp / _catch_h: runtime catch-frame stack. */
    if (sr && sr->has_try && stack_size > 0) {
        jit_buf_str(cb, "    JSValue *_tsvp[] = {");
        for (int j = 0; j < stack_size; j++) {
            if (j > 0) jit_buf_str(cb, ",");
            jit_buf_printf(cb, "&_tsv%d", j);
        }
        jit_buf_str(cb, "};\n");
        jit_buf_str(cb,
            "    int _catch_depth=0, _catch_sp[32], _catch_h[32];\n");
    }

    /* P9.1: named local variable declarations (one scalar per local, not arrays).
     * JSValue  _jsv_<name>_<i>  — always (initialised to JS_UNDEFINED)
     * int64_t  _jsi_<name>_<i>  — INT locals only  (Phase 5/P8.1)
     * double   _jsd_<name>_<i>  — NUMBER locals only (Phase 5)
     * _jsv stays JS_UNDEFINED for typed locals → _FREE in footer is a no-op. */
    for (int j = 0; j < var_count; j++)
        jit_buf_printf(cb, "    JSValue _jsv_%s=JS_UNDEFINED;\n",
                       varnames[arg_count + j]);
    if (local_type) {
        for (int j = 0; j < var_count; j++) {
            if (local_type[j] == JIT_T_INT)
                jit_buf_printf(cb, "    int64_t _jsi_%s=0;\n",
                               varnames[arg_count + j]);
            else if (local_type[j] == JIT_T_NUMBER)
                jit_buf_printf(cb, "    double _jsd_%s=0.0;\n",
                               varnames[arg_count + j]);
        }
    }

    /* P8.4 / P9.1: named integer argument fast-path variables.
     * _jai_<name>_<i> — int32_t for arg i when it is JS_TAG_INT
     * _aim            — bitmask: bit i set iff argv[i] is INT and _jai_ is valid */
    if (arg_count > 0) {
        int n = arg_count < 32 ? arg_count : 32;
        jit_buf_str(cb, "    uint32_t _aim=0;\n");
        for (int j = 0; j < n; j++)
            jit_buf_printf(cb, "    int32_t _jai_%s=0;\n", varnames[j]);
        for (int j = 0; j < n; j++)
            jit_buf_printf(cb,
                "    if(%d<argc&&JS_VALUE_GET_TAG(argv[%d])==JS_TAG_INT)"
                "{_jai_%s=JS_VALUE_GET_INT(argv[%d]);_aim|=%uu;}\n",
                j, j, varnames[j], j, 1u << j);
        jit_buf_str(cb, "    (void)_aim;\n");
    }

    /* P40.2: hoist pvalue pointers for all captured variables into locals.
     * var_refs[i] is stable for the entire JIT call; pvalue within each ref
     * is also stable (changes only at scope-exit detachment which triggers a
     * JIT exit before control returns here).  Caching in _vrp{i} lets GCC
     * keep the pointer in a register and avoids re-loading var_refs[i] and
     * dereferencing pvalue on every get/put_var_ref opcode. */
    if (closure_var_count > 0) {
        for (int vri = 0; vri < closure_var_count; vri++) {
            jit_buf_printf(cb,
                "    JSValue *_vrp%d=*(JSValue**)((char*)var_refs[%d]+JIT_VARREF_PVALUE_OFF);\n",
                vri, vri);
        }
    }

    /* P13.4: shadow arrays for closure capture.
     * Only emitted when the function contains at least one OP_fclosure. */
    if (sr && sr->has_fclosure) {
        /* _cap_buf[local_idx]: canonical slot for captured locals.
         * JSVarRefs point their pvalue here; content equals current local value.
         * Explicit JS_UNDEFINED initialisation required (memset(0) ≠ JS_UNDEFINED). */
        if (var_count > 0) {
            jit_buf_printf(cb, "    JSValue _cap_buf[%d];\n", var_count);
            for (int j = 0; j < var_count && j < 64; j++) {
                if ((sr->captured_local_mask >> j) & 1)
                    jit_buf_printf(cb, "    _cap_buf[%d]=JS_UNDEFINED;\n", j);
            }
        }
        /* _sf_vrefs[var_ref_idx]: shared JSVarRef* cache across all OP_fclosure calls.
         * Multiple closures capturing the same local share one JSVarRef (refcount>1). */
        if (var_ref_count > 0) {
            jit_buf_printf(cb, "    JSVarRef *_sf_vrefs[%d];\n", var_ref_count);
            jit_buf_printf(cb, "    memset(_sf_vrefs,0,sizeof(_sf_vrefs));\n");
        } else {
            /* No var_refs but still has_fclosure (all REF/GLOBAL_REF): emit dummy */
            jit_buf_str(cb, "    JSVarRef **_sf_vrefs=0; (void)_sf_vrefs;\n");
        }
    }
    /* _arg_cap_buf[arg_idx]: local copy of captured/mutated arguments.
     * - Captured args (via captured_arg_mask): JSVarRef pvalue points here.
     * - Mutated non-captured args (via mutated_arg_mask, non-generator only):
     *   OP_put_arg writes here instead of argv[], keeping argv[] immutable for
     *   the P10.3 direct-call convention (argv is borrowed, not owned by callee).
     * Initialised with DUP from argv so we own a reference throughout. */
    if (sr && arg_count > 0) {
        uint64_t _eff_am = sr->captured_arg_mask |
                           (sr->has_yield ? 0ULL : sr->mutated_arg_mask);
        if (_eff_am != 0) {
            jit_buf_printf(cb, "    JSValue _arg_cap_buf[%d];\n", arg_count);
            for (int j = 0; j < arg_count && j < 64; j++) {
                if ((_eff_am >> j) & 1)
                    jit_buf_printf(cb,
                        "    _arg_cap_buf[%d]=(%d<argc)?_DUP(argv[%d]):JS_UNDEFINED;\n",
                        j, j, j);
            }
        }
    }

    /* P12: generator frame setup — emitted when the function has OP_yield.
     *
     * On every entry (initial call and every resume) we:
     *   1. Obtain/create the JSJITGeneratorFrame for this generator invocation.
     *   2. Check for a pending throw (magic == GEN_MAGIC_THROW) and bail early.
     *   3. If resume_idx >= 0 (resuming from a prior yield):
     *        a. Restore all local JSValue slots from _gf->saved_lv[].
     *        b. Dispatch to the matching _Lresume_N label via switch.
     */
    if (sr && sr->has_yield) {
        int j, k;
        /* saved_lv layout: [var_count local JSValues][max_below_yield stack JSValues]
         * The extra stack slots hold live _tsv{j} values below the yielded value. */
        int n_lv_total = var_count + sr->max_below_yield;
        /* Declare the generator frame pointer */
        jit_buf_printf(cb,
            "    JSJITGeneratorFrame *_gf=js_jit_gen_init_frame(ctx,%d,%d);\n",
            n_lv_total, var_ref_count);
        jit_buf_str(cb,
            "    if(!_gf){_sp=0;goto _ex;}\n"
            "    if(_gf->resume_idx>=0){\n");
        /* Restore local JSValue slots from saved_lv (transfer ownership) */
        for (j = 0; j < var_count; j++) {
            jit_buf_printf(cb,
                "        _jsv_%s=_gf->saved_lv[%d]; _gf->saved_lv[%d]=JS_UNDEFINED;\n",
                varnames[arg_count + j], j, j);
        }
        /* P12.1: restore catch state before throw check and dispatch so try/catch
         * handlers are active when the resumed-with-throw path fires goto _ex. */
        if (sr->has_try && stack_size > 0) {
            jit_buf_str(cb,
                "        _catch_depth=_gf->catch_depth;\n"
                "        if(_catch_depth>0){\n"
                "            memcpy(_catch_sp,_gf->catch_sp,(size_t)_catch_depth*sizeof(int));\n"
                "            memcpy(_catch_h,_gf->catch_h,(size_t)_catch_depth*sizeof(int));\n"
                "        }\n");
        }
        /* Throw check AFTER catch state is restored so that .throw() / rejected
         * await inside a try block is routed to the correct catch handler. */
        jit_buf_str(cb,
            "        if(js_jit_gen_get_throw(ctx)){_sp=0;goto _ex;}\n");
        /* P12.2: restore closure var-refs from frame and re-attach to _cap_buf / _arg_cap_buf */
        if (sr->has_fclosure && var_ref_count > 0 && b) {
            int j;
            jit_buf_printf(cb,
                "        js_jit_gen_restore_vrefs(_sf_vrefs,%d,_gf);\n",
                var_ref_count);
            /* Re-attach each captured local: _cap_buf[i] ← vref->value (own ref),
             * vref->pvalue ← &_cap_buf[i], is_detached ← FALSE */
            for (j = 0; j < var_count && j < 64; j++) {
                if ((sr->captured_local_mask >> j) & 1) {
                    int vri = js_jit_fb_get_local_var_ref_idx(b, j);
                    if (vri >= 0) {
                        jit_buf_printf(cb,
                            "        js_jit_varref_reattach(_sf_vrefs[%d],&_cap_buf[%d]);\n",
                            vri, j);
                    }
                }
            }
            /* Re-attach each captured arg: _arg_cap_buf[i] ← vref->value (own ref) */
            if (sr->captured_arg_mask != 0) {
                for (j = 0; j < arg_count && j < 64; j++) {
                    if ((sr->captured_arg_mask >> j) & 1) {
                        int vri = js_jit_fb_get_arg_var_ref_idx(b, j);
                        if (vri >= 0) {
                            jit_buf_printf(cb,
                                "        js_jit_varref_reattach(_sf_vrefs[%d],&_arg_cap_buf[%d]);\n",
                                vri, j);
                        }
                    }
                }
            }
        }
        /* Dispatch to the resume label matching _gf->resume_idx.
         * For each resume label, also restore the live stack temporaries that
         * were below the yielded value (saved in saved_lv[var_count..] at yield time). */
        jit_buf_str(cb, "        switch(_gf->resume_idx){\n");
        /* P12.3: async functions have no OP_initial_yield so resume_idx is never 0 */
        if (sr->func_kind != JS_JIT_FUNC_ASYNC) {
            /* yield_below[0] is for initial_yield: always 0 */
            jit_buf_str(cb, "        case 0: goto _Lresume_0;\n");
        }
        for (k = 1; k <= sr->yield_count; k++) {
            /* yi_k = index into yield_below[] for this resume label.
             * For generators/async-generators: yield_below[0]=initial_yield, yield_below[k]=resume_k.
             * For pure async functions (no OP_initial_yield): yield_below[k-1]=resume_k. */
            int yi_k = (sr->func_kind == JS_JIT_FUNC_ASYNC) ? k - 1 : k;
            int below = (yi_k >= 0 && yi_k < 64) ? sr->yield_below[yi_k] : 0;
            if (below > 0) {
                jit_buf_printf(cb, "        case %d: {\n", k);
                for (j = 0; j < below; j++) {
                    jit_buf_printf(cb,
                        "            _tsv%d=_gf->saved_lv[%d];"
                        " _gf->saved_lv[%d]=JS_UNDEFINED;\n",
                        j, var_count + j, var_count + j);
                }
                /* Set _sp so exception cleanup frees the restored stack slots. */
                jit_buf_printf(cb, "            _sp=%d; goto _Lresume_%d; }\n", below, k);
            } else {
                jit_buf_printf(cb, "        case %d: goto _Lresume_%d;\n", k, k);
            }
        }
        jit_buf_str(cb,
            "        }\n"
            "    } else {\n"
            "        /* Initial call (resume_idx==-1): throw before first next() */\n"
            "        if(js_jit_gen_get_throw(ctx)){_sp=0;goto _ex;}\n"
            "    }\n");
    }
}

/*
 * Emit the exception-cleanup footer and closing brace.
 * P9.2: stack cleanup is unrolled using _tsv{} names, guarded by _sp.
 * _sp is updated at every throw/exception site so this correctly frees
 * only live slots.
 */
static void gen_footer(JSJITCodeBuf *cb, int var_count,
                       int arg_count, char **varnames, int stack_size,
                       const JSJITScanResult *sr, int var_ref_count)
{
    /* P13.7: heap-promote all live var_refs and free shadow arrays before _ex cleanup */
    if (sr && sr->has_fclosure) {
        if (var_ref_count > 0)
            jit_buf_printf(cb, "    js_jit_close_caps(ctx,_sf_vrefs,%d);\n", var_ref_count);
        for (int j = 0; j < var_count && j < 64; j++)
            if ((sr->captured_local_mask >> j) & 1)
                jit_buf_printf(cb, "    _FREE(_cap_buf[%d]);\n", j);
    }
    if (sr && arg_count > 0) {
        uint64_t _eff_am = sr->captured_arg_mask |
                           (sr->has_yield ? 0ULL : sr->mutated_arg_mask);
        for (int j = 0; j < arg_count && j < 64; j++)
            if ((_eff_am >> j) & 1)
                jit_buf_printf(cb, "    _FREE(_arg_cap_buf[%d]);\n", j);
    }
    jit_buf_str(cb, "_ex:\n");
    /* P14: catch dispatch — if there is an active catch handler, redirect to it
     * instead of doing full cleanup.  _tsvp[] allows runtime-indexed slot access. */
    if (sr && sr->has_try && stack_size > 0) {
        jit_buf_str(cb,
            "    if(_catch_depth>0){\n"
            "        _catch_depth--;\n"
            "        { int _cs=_catch_sp[_catch_depth];\n"
            "          int _ch=_catch_h[_catch_depth]; int _j;\n"
            "          for(_j=_sp-1;_j>_cs;_j--)JS_FreeValue(ctx,*_tsvp[_j]);\n"
            "          *_tsvp[_cs]=JS_GetException(ctx); _sp=_cs+1;\n"
            "          switch(_ch){\n");
        for (int i = 0; i < sr->n_catch; i++)
            jit_buf_printf(cb, "            case %d: goto _L%d;\n",
                           sr->catch_handler_pcs[i], sr->catch_handler_pcs[i]);
        jit_buf_str(cb,
            "          }\n"
            "        }\n"
            "    }\n");
    }
    /* P43.6: on final exception exit (all catch handlers exhausted or no handler):
     * close var_refs so captured locals are heap-promoted, then free shadow arrays.
     * js_jit_close_caps is idempotent; _cap_buf[j] is initialized to JS_UNDEFINED
     * so _FREE is always safe even if the slot was never written. */
    if (sr && sr->has_fclosure) {
        if (var_ref_count > 0)
            jit_buf_printf(cb, "    js_jit_close_caps(ctx,_sf_vrefs,%d);\n", var_ref_count);
        for (int j = 0; j < var_count && j < 64; j++)
            if ((sr->captured_local_mask >> j) & 1)
                jit_buf_printf(cb, "    _FREE(_cap_buf[%d]);\n", j);
    }
    if (sr && arg_count > 0) {
        uint64_t _eff_am = sr->captured_arg_mask |
                           (sr->has_yield ? 0ULL : sr->mutated_arg_mask);
        for (int j = 0; j < arg_count && j < 64; j++)
            if ((_eff_am >> j) & 1)
                jit_buf_printf(cb, "    _FREE(_arg_cap_buf[%d]);\n", j);
    }
    for (int j = 0; j < var_count; j++)
        jit_buf_printf(cb, "    _FREE(_jsv_%s);\n", varnames[arg_count + j]);
    /* Unrolled stack cleanup: free _tsv{stack_size-1} down to _tsv{0}
     * but only if _sp says that slot is live. */
    for (int j = stack_size - 1; j >= 0; j--)
        jit_buf_printf(cb, "    if(_sp>%d){_FREE(_tsv%d);}\n", j, j);
    jit_buf_str(cb,
        "    return JS_EXCEPTION;\n"
        "}\n");
}

/* =======================================================================
 * Phase 6.1 — Comparison+branch fusion helper
 *
 * When a comparison opcode (lt/lte/gt/gte/eq/neq/strict_eq/strict_neq) is
 * immediately followed by if_false or if_true, and the if_false/if_true
 * bytecode offset is NOT itself a branch target (i.e. nothing else jumps
 * directly to the if_false instruction), we can fuse the two into a single
 * C block that avoids creating a JSValue bool on the stack.
 *
 * Savings per loop iteration:
 *   - JS_NewBool() call eliminated (was a JSValue push)
 *   - _s[_sp++] / _s[--_sp] pair eliminated
 *   - _BOOL() macro + _FREE() call eliminated
 *   - When both operands are NUMBER: vtable lt/lte/gt/gte call eliminated;
 *     direct double comparison used instead.
 * ======================================================================= */
typedef struct {
    int fuse;      /* 1 = fusion possible */
    int tgt;       /* branch target pc */
    int negate;    /* 1 = if_false (jump when !cond), 0 = if_true (jump when cond) */
    int extra_sz;  /* size of the if_false/if_true instruction to skip */
} JitFuseInfo;

static JitFuseInfo
jit_check_fuse(const uint8_t *bc, int next_pc, int bc_len,
               const uint8_t *op_sz, const JSJITScanResult *sr)
{
    JitFuseInfo fi = {0, 0, 0, 0};
    if (next_pc >= bc_len) return fi;
    /* Only fuse if the if_false/if_true opcode is not itself a jump target —
     * otherwise we must emit its label and cannot skip it. */
    if (scan_is_target(sr, next_pc)) return fi;

    int nop = bc[next_pc];
    if (nop == OP_if_false) {
        fi.fuse = 1; fi.negate = 1;
        fi.tgt = next_pc + 1 + (int)(int32_t)bc_u32(&bc[next_pc + 1]);
        fi.extra_sz = op_sz[OP_if_false];
    } else if (nop == OP_if_true) {
        fi.fuse = 1; fi.negate = 0;
        fi.tgt = next_pc + 1 + (int)(int32_t)bc_u32(&bc[next_pc + 1]);
        fi.extra_sz = op_sz[OP_if_true];
    } else if (nop == OP_if_false8) {
        fi.fuse = 1; fi.negate = 1;
        fi.tgt = next_pc + 1 + (int)(int8_t)bc[next_pc + 1];
        fi.extra_sz = op_sz[OP_if_false8];
    } else if (nop == OP_if_true8) {
        fi.fuse = 1; fi.negate = 0;
        fi.tgt = next_pc + 1 + (int)(int8_t)bc[next_pc + 1];
        fi.extra_sz = op_sz[OP_if_true8];
    }
    return fi;
}

/*
 * Main code generator: iterates over the bytecode and emits a C statement
 * for each opcode.  Unsupported opcodes set *unsupported=1 and return -1.
 */
static int gen_body(JSJITCodeBuf *cb, const uint8_t *bc, int bc_len,
                    const JSJITScanResult *sr,
                    const uint8_t *op_sz, int op_sz_count,
                    int var_count, int arg_count, int stack_size,
                    int *unsupported_out,
                    const uint8_t *local_type,
                    JSFunctionBytecode *b,
                    uint64_t bc_hash,
                    char **varnames,
                    const uint64_t *var_jit_hash,
                    int var_ref_count,
                    const uint8_t *vt_hints, /* P45b: val_tag hints or NULL */
                    int n_gf,               /* P45b: OP_get_field count */
                    int n_ae,               /* P46: OP_get_array_el count */
                    int n_pf,               /* P48: OP_put_field count */
                    int n_vr,               /* P49: OP_get_var_ref* count */
                    int n_pa,               /* P50: OP_put_array_el count */
                    int n_ad,               /* P51: OP_add count */
                    int n_pv)               /* P52: OP_put/set_var_ref* count */
{
    *unsupported_out = 0;
    int pc = 0;

    /* P12: counter for yield resume sites (0 = initial_yield, 1..N = OP_yield sites) */
    int yield_site_counter = 1; /* starts at 1; 0 is reserved for initial_yield */

    /* P45b: get_field index counter for __jit_vt_HASH[] array indexing.
     * Incremented once per OP_get_field in the secondary gen_st switch (runs after
     * the main switch, so the main switch reads gf_idx BEFORE the increment). */
    int gf_idx = 0;
    /* P46: get_array_el index counter for __jit_vt_HASH[] array indexing.
     * Indices n_gf..n_gf+n_ae-1; incremented in secondary gen_st switch. */
    int ae_idx = 0;
    /* P48: put_field index counter for __jit_vt_HASH[] write-value hints.
     * Indices n_gf+n_ae..n_gf+n_ae+n_pf-1; incremented in secondary gen_st switch. */
    int pf_idx = 0;
    /* P49: get_var_ref index counter for __jit_vt_HASH[] cell hints.
     * Indices n_gf+n_ae+n_pf..n_gf+n_ae+n_pf+n_vr-1; incremented in secondary gen_st switch. */
    int vr_idx = 0;
    /* P50: put_array_el index counter for __jit_vt_HASH[] write-value hints.
     * Indices n_gf+n_ae+n_pf+n_vr..+n_pa-1; incremented in secondary gen_st switch. */
    int pa_idx = 0;
    /* P51: add index counter for __jit_vt_HASH[] operand-tag hints.
     * 2 slots per OP_add: [base + ad_idx*2] = left tag, [base + ad_idx*2+1] = right tag.
     * Indices n_gf+n_ae+n_pf+n_vr+n_pa..+n_ad*2-1; incremented in secondary gen_st switch. */
    int ad_idx = 0;
    /* P52: put/set_var_ref* index counter for __jit_vt_HASH[] old-value hints.
     * 1 slot per put/set_var_ref* site: old-value tag observed before the write.
     * Indices n_gf+n_ae+n_pf+n_vr+n_pa+n_ad*2..+n_pv-1; incremented in gen_st switch. */
    int pv_idx = 0;

    /* P14: compile-time tracking of catch placeholder stack depths.
     * OP_catch pushes JS_UNDEFINED as a placeholder at slot d and increments
     * _catch_depth.  The interpreter removes it via OP_nip_catch (which already
     * emits _catch_depth--) or via OP_drop (which previously did NOT decrement
     * _catch_depth, causing a one-per-iteration leak in loops with try/catch).
     *
     * We track the stack slot d where each catch placeholder sits.  When OP_drop
     * would discard the innermost placeholder (d-1 == catch_ph_d[catch_ph_n-1]),
     * we also emit _catch_depth-- to keep the runtime counter in sync. */
    int catch_ph_d[32];  /* stack depths of live catch placeholders */
    int catch_ph_n = 0;  /* number of entries in catch_ph_d */

    /* P33: flag for complex-param functions (rest/default/destructuring args).
     * GEN_GET/PUT/SET_ARG must be unconditional for these because:
     *  - arg_buf is always padded to arg_count by the caller
     *  - OP_rest needs the ORIGINAL argc (not the padded arg_count)
     *  - so we pass original argc to the JIT, and bypass the argc-bound check
     *    in arg access since all slots 0..arg_count-1 are always valid. */
    const int _has_complex_params = (b && !js_jit_fb_has_simple_params(b));

    /* P9.2: per-PC stack depth table (from compute_stack_size pass). */
    const uint16_t *sdt = js_jit_fb_get_stack_depth_tab(b);

    /* P9.3: CF annotations for structured loop emission (while/do-while only). */
    int n_cf = 0;
    const JSJITCFAnnotation *cf_annots = js_jit_fb_cf_annotations(b, &n_cf);
    typedef struct { uint32_t header_pc, exit_pc; } P93Loop;
    P93Loop p93_active[16]; /* max nesting depth */
    int p93_depth = 0;

    /* P9.1: named-variable helpers — defined here so all opcode cases can use them */
#define LNAME(idx) varnames[arg_count + (idx)]
#define ANAME(idx) varnames[(idx)]

    /* Phase 6.1: gen-time type stack.  Tracks the abstract type (JIT_T_NUMBER
     * or JIT_T_JSVAL) of each slot on the value stack during code generation.
     * Used to select optimised comparison paths and to enable comparison+branch
     * fusion.  Conservative: reset to all-JSVAL at every branch target. */
    int gen_stk_cap = (stack_size < 4 ? 4 : stack_size) + 8;
    uint8_t *gen_st = (uint8_t *)calloc(gen_stk_cap, 1);
    int gen_sp = 0;
    if (!gen_st) {
        *unsupported_out = 0;
        return -1;
    }

    /* P10.3: parallel hash array for JIT_T_JIT_FUNC slots */
    int p103_cvc_gb = js_jit_fb_get_closure_var_count(b);
    uint64_t *gen_hsh = (uint64_t *)calloc(gen_stk_cap, sizeof(uint64_t));
    if (!gen_hsh) { free(gen_st); *unsupported_out = 0; return -1; }
    /* Extern declaration tracking (max 16 distinct callees per function) */
    uint64_t p103_externs[16];
    int p103_nexterns = 0;
    int p103_cae_declared = 0; /* js_jit_check_and_extract declaration emitted? */

#define _GS_PUSH(t) do { if (gen_sp < gen_stk_cap) gen_st[gen_sp++] = (uint8_t)(t); } while(0)
#define _GS_POP()   (gen_sp > 0 ? gen_st[--gen_sp] : (uint8_t)JIT_T_JSVAL)
#define _GS_TOP()   (gen_sp > 0 ? gen_st[gen_sp-1]   : (uint8_t)JIT_T_JSVAL)
#define _GS_TOP2()  (gen_sp > 1 ? gen_st[gen_sp-2]   : (uint8_t)JIT_T_JSVAL)
#define _GS_DROP(n) do { gen_sp -= (n); if (gen_sp < 0) gen_sp = 0; } while(0)

/* P9.4: ensure _tsv{slot} has valid JSValue when gen_st says it may be typed.
 * JS_NewFloat64/Int32 on 64-bit are inline struct assignments — no heap alloc.
 * Guard: (slot) < d ensures we only box within the real runtime stack depth;
 * gen_sp may drift above d when some pop-ops are not tracked in gen_st, so
 * checking gen_sp alone can incorrectly fire on stale gen_st entries. */
/* JIT_T_SELF_FUNC (=3) must NOT trigger boxing — it marks a JSValue (function
 * object).  JIT_T_INT (2) values live in _ti{slot}; JIT_T_NUMBER (1) in _tsd. */
#define _P94_ENSURE(slot) do { \
    if ((slot) < d && gen_sp > (slot) && \
        gen_st[(slot)] >= JIT_T_NUMBER && gen_st[(slot)] <= JIT_T_INT) { \
        if (gen_st[(slot)] == JIT_T_INT) \
            jit_buf_printf(cb, \
                "    { int64_t _tv=_ti%d; " \
                "_tsv%d=((int64_t)(int32_t)_tv==_tv)?JS_NewInt32(ctx,(int32_t)_tv)" \
                ":JS_NewFloat64(ctx,(double)_tv); }\n", (slot), (slot)); \
        else \
            jit_buf_printf(cb, \
                "    { double _dv=_tsd%d; " \
                "_tsv%d=((double)(int32_t)_dv==_dv)?JS_NewInt32(ctx,(int32_t)_dv)" \
                ":JS_NewFloat64(ctx,_dv); }\n", (slot), (slot)); \
    } \
} while(0)

    /* P8.2: self-recursive direct call detection.
     * self_func_atom is the function's own name atom.  When OP_get_var loads a
     * closure var whose atom matches self_func_atom, the gen_st slot is marked
     * JIT_T_SELF_FUNC.  When OP_call* sees that the function slot has that
     * marker, we emit a direct C call to __jit_f_<hash> instead of _RT->call.
     * self_jit_sym is the stable symbol name already emitted by gen_preamble. */
    JSAtom self_func_atom = js_jit_fb_get_func_atom(b);
    char   self_jit_sym[32];
    snprintf(self_jit_sym, sizeof(self_jit_sym), "__jit_f_%016llx",
             (unsigned long long)bc_hash);

    /* P38.1: refcount elision depth.  When a get_loc emitter skips DupValue
     * (look-ahead shows next op(s) consume the object), it sets _borrowed_depth=d.
     * The consumer (get_field / get_array_el) reads borrowed_depth_snap and omits
     * _FREE(_o).  -1 means no borrow is active. */
    int _borrowed_depth = -1;

    while (pc < bc_len) {
        /* P38.1: snapshot borrow depth for this iteration.
         * top_borrowed is the compat shim used by get_field emitters.
         * borrowed_depth_snap is used by get_array_el for depth-aware check.
         * Individual opcodes own _borrowed_depth lifecycle; do NOT reset here. */
        int top_borrowed = (_borrowed_depth != -1);          /* compat for get_field */
        int borrowed_depth_snap = _borrowed_depth;           /* full depth for get_array_el */

        /* P9.2: stack depth BEFORE this opcode.  sdt[pc]==0xffff means unreachable.
         * Computed early so _P94_ENSURE (which uses d) works in the label block. */
        int d = (sdt && sdt[pc] != 0xffff) ? (int)sdt[pc] : 0;

        /* P38.1: if this PC is a branch target, other paths may jump here
         * without executing the preceding get_loc.  Discard any borrow. */
        if (scan_is_target(sr, pc)) {
            top_borrowed = 0;
            _borrowed_depth = -1;
        }

        /* Emit label if this offset is a branch target.
         * Also reset gen_st conservatively — multiple control-flow paths merge
         * here so we cannot assume the type stack is consistent. */
        if (scan_is_target(sr, pc)) {
            /* P9.4: box any typed stack slots before the label boundary.
             * Values in _tsd slots (never boxed) must be written to _tsv
             * because post-label code may take the JSVAL path. */
            for (int _s = 0; _s < gen_sp; _s++)
                _P94_ENSURE(_s);
            /* Set gen_sp to the actual bytecode stack depth at this label so that
             * _P94_ENSURE(slot) checks remain valid for all live slots after the
             * reset.  All types are JSVAL since we can't know which path we came from. */
            memset(gen_st, JIT_T_JSVAL, gen_stk_cap);
            memset(gen_hsh, 0, gen_stk_cap * sizeof(uint64_t));
            gen_sp = (sdt != NULL) ? (int)sdt[pc] : 0;
            if (gen_sp < 0 || gen_sp > gen_stk_cap) gen_sp = 0;
            jit_buf_printf(cb, "_L%d:;\n", pc);
            /* P9.3: while(1){} restructuring disabled.
             * It is unsafe whenever a loop's back-edge is a conditional branch
             * rather than OP_goto — the while(1) block opens but never closes.
             * This occurs for do-while loops and any multi-exit loop where the
             * CF annotation heuristic misclassifies the back-edge type.
             * The optimizer correctly handles backward gotos directly. */
            (void)p93_depth; (void)p93_active; (void)n_cf; (void)cf_annots;
        }

        /* P9.4: if gen_sp has drifted above d (due to pop-ops missing from gen_st),
         * reset conservatively so stale INT entries can't cause false _bn=true or
         * spurious boxing in _P94_ENSURE. */
        if (gen_sp > d) {
            memset(gen_st, JIT_T_JSVAL, gen_stk_cap);
            memset(gen_hsh, 0, gen_stk_cap * sizeof(uint64_t));
            gen_sp = d;
        }

        int op = bc[pc];
        if (op >= op_sz_count || op_sz[op] == 0) {
            fprintf(stderr, "[JIT] unsupported opcode 0x%02x at pc=%d\n", op, pc);
            *unsupported_out = 1; free(gen_st); free(gen_hsh); return -1;
        }
        int sz = op_sz[op];

        switch (op) {

        /* ---- nop ---- */
        case OP_nop:
            break;

        /* ---- P14: try/catch/finally ---- */

        /* OP_catch: push a catch frame on the runtime catch stack.
         * Also push JS_UNDEFINED as a placeholder on the tsv stack
         * so sdt[] stack depths remain consistent. */
        case OP_catch: {
            int32_t diff = (int32_t)bc_u32(&bc[pc+1]);
            int handler_pc = (pc + 1) + diff;
            jit_buf_printf(cb,
                "    _catch_sp[_catch_depth]=%d;"
                " _catch_h[_catch_depth]=%d;"
                " _catch_depth++;\n"
                "    _tsv%d=JS_UNDEFINED; _sp=%d;\n",
                d, handler_pc, d, d+1);
            /* Track the slot where the placeholder sits so OP_drop can detect
             * when it's discarding a catch placeholder and emit _catch_depth--. */
            if (catch_ph_n < 32) catch_ph_d[catch_ph_n++] = d;
            break;
        }

        /* OP_nip_catch: remove innermost catch frame, keep ret_val.
         * Stack before: [..., catch_placeholder@cs, ..., ret_val@d-1]
         * Stack after:  [..., ret_val@cs], sp=cs+1
         * Free intermediate slots cs+1..d-2 (typically none). */
        case OP_nip_catch: {
            /* _catch_depth-- is emitted inline; also pop the compile-time tracker. */
            if (catch_ph_n > 0) catch_ph_n--;
            jit_buf_printf(cb,
                "    { int _cs; _catch_depth--; _cs=_catch_sp[_catch_depth];\n"
                "      int _j; for(_j=_cs+1;_j<%d;_j++)JS_FreeValue(ctx,*_tsvp[_j]);\n"
                "      *_tsvp[_cs]=*_tsvp[%d]; _sp=_cs+1; }\n",
                d-1, d-1);
            break;
        }

        /* OP_gosub: push return PC as int, jump to finally subroutine. */
        case OP_gosub: {
            int32_t diff = (int32_t)bc_u32(&bc[pc+1]);
            int sub_target = (pc + 1) + diff;
            int ret_pc = pc + 5;
            jit_buf_printf(cb,
                "    _tsv%d=JS_NewInt32(ctx,%d); _sp=%d;\n"
                "    goto _L%d;\n",
                d, ret_pc, d+1, sub_target);
            break;
        }

        /* OP_ret: pop gosub return address, dispatch to the return PC.
         * Uses a switch over all known gosub return PCs (from scan). */
        case OP_ret: {
            jit_buf_printf(cb,
                "    { int _rpc=JS_VALUE_GET_INT(_tsv%d); _sp=%d;\n"
                "      switch(_rpc){\n",
                d-1, d-1);
            for (int _ri = 0; _ri < sr->n_gosub; _ri++)
                jit_buf_printf(cb, "        case %d: goto _L%d;\n",
                               sr->gosub_ret_pcs[_ri], sr->gosub_ret_pcs[_ri]);
            jit_buf_str(cb,
                "        default: JS_ThrowInternalError(ctx,\"invalid ret\");"
                " goto _ex;\n"
                "      }\n"
                "    }\n");
            break;
        }

        /* ---- P15b: OP_special_object ---- */

        /* OP_special_object pushes one new JSVAL: arguments, this_func, new.target, etc.
         * Delegates to js_jit_special_object which reads ctx->rt->current_stack_frame
         * (set by JS_CallInternal before invoking the JIT function). */
        case OP_special_object: {
            int _kind = (int)bc[pc+1];
            _P94_ENSURE(d);
            jit_buf_printf(cb,
                "    { JSValue _so = js_jit_special_object(ctx,%d,argc,argv);\n"
                "      if(JS_IsException(_so)) goto _ex;\n"
                "      _tsv%d=_so; _sp=%d; }\n",
                _kind, d, d+1);
            break;
        }

        /* ---- P15: iterators / for-in / for-of ---- */

        /* OP_for_in_start: obj → iter (in-place).
         * Calls js_jit_for_in_start which replaces *pobj with the iterator. */
        case OP_for_in_start:
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    if(js_jit_for_in_start(ctx,&_tsv%d)<0) goto _ex;\n",
                d-1);
            break;

        /* OP_for_in_next: iter → iter key done (+2).
         * iter stays at d-1; key at d, done at d+1. */
        case OP_for_in_next:
            _P94_ENSURE(d+1);
            jit_buf_printf(cb,
                "    _tsv%d=JS_UNDEFINED; _tsv%d=JS_UNDEFINED;\n"
                "    if(js_jit_for_in_next(ctx,_tsv%d,&_tsv%d,&_tsv%d)<0)"
                " goto _ex;\n"
                "    _sp=%d;\n",
                d, d+1,
                d-1, d, d+1,
                d+2);
            break;

        /* OP_for_of_start: obj → iter next catch_placeholder (+2 net).
         * Calls js_jit_for_of_start(ctx, &iter, &next, obj).
         * Pushes JS_UNDEFINED as catch_offset placeholder (slot d+1). */
        case OP_for_of_start:
            _P94_ENSURE(d+1);
            jit_buf_printf(cb,
                "    _tsv%d=JS_UNDEFINED; _tsv%d=JS_UNDEFINED;\n"
                "    if(js_jit_for_of_start(ctx,&_tsv%d,&_tsv%d,_tsv%d)<0)"
                " goto _ex;\n"
                "    _sp=%d;\n",
                d, d+1,
                d-1, d, d-1,
                d+2);
            break;

        /* OP_for_of_next: iter next catch_ph [extra] → ... value done (+2).
         * The u8 operand (bc[pc+1]) gives the number of extra items pushed above
         * the catch_ph since for_of_start (e.g. a destructuring target object).
         * iter at d-3-extra, next at d-2-extra; value→d, done→d+1. */
        case OP_for_of_next: {
            int _extra = (int)bc[pc+1];
            _P94_ENSURE(d+1);
            jit_buf_printf(cb,
                "    _tsv%d=JS_UNDEFINED; _tsv%d=JS_UNDEFINED;\n"
                "    if(js_jit_for_of_next(ctx,&_tsv%d,_tsv%d,&_tsv%d,&_tsv%d)<0)"
                " goto _ex;\n"
                "    _sp=%d;\n",
                d, d+1,
                d-3-_extra, d-2-_extra, d, d+1,
                d+2);
            break;
        }

        /* P30: OP_for_await_of_start: obj → iter next catch_ph (+2 net, async=TRUE).
         * Same interface as OP_for_of_start but uses Symbol.asyncIterator. */
        case OP_for_await_of_start:
            _P94_ENSURE(d+1);
            jit_buf_printf(cb,
                "    _tsv%d=JS_UNDEFINED; _tsv%d=JS_UNDEFINED;\n"
                "    if(_RT->for_await_of_start(ctx,&_tsv%d,&_tsv%d,_tsv%d)<0)"
                " goto _ex;\n"
                "    _sp=%d;\n",
                d, d+1,
                d-1, d, d-1,
                d+2);
            break;

        /* P30: OP_for_await_of_next: iter(d-3) next(d-2) catch_ph(d-1) → +promise(d).
         * Clears catch_ph, calls next.call(iter), pushes raw Promise.
         * The caller must follow with OP_await to actually suspend. */
        case OP_for_await_of_next:
            _P94_ENSURE(d);
            jit_buf_printf(cb,
                "    _tsv%d=JS_UNDEFINED;\n"
                "    if(_RT->for_await_of_next(ctx,_tsv%d,_tsv%d,&_tsv%d,&_tsv%d)<0)"
                " goto _ex;\n"
                "    _sp=%d;\n",
                d,
                d-3, d-2, d-1, d,
                d+1);
            break;

        /* OP_iterator_close: iter next catch_ph → (free all, close iter if non-null).
         * iter at d-3, next at d-2, catch_ph at d-1. */
        case OP_iterator_close:
            jit_buf_printf(cb,
                "    JS_FreeValue(ctx,_tsv%d);\n"      /* free catch_ph */
                "    if(js_jit_iterator_close(ctx,_tsv%d,_tsv%d)<0)"
                " goto _ex;\n"
                "    _sp=%d;\n",
                d-1, d-3, d-2,
                d-3);
            break;

        /* OP_iterator_check_object: check sp[-1] is an object. No stack change. */
        case OP_iterator_check_object:
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    if(!JS_IsObject(_tsv%d)){"
                " JS_ThrowTypeError(ctx,\"iterator must return an object\");"
                " goto _ex; }\n",
                d-1);
            break;

        /* OP_iterator_get_value_done: catch_ph obj → catch_ph value done (+1).
         * catch_ph at d-2, obj at d-1; value replaces obj at d-1, done at d.
         * The catch_ph slot (d-2) is reset to JS_UNDEFINED. */
        case OP_iterator_get_value_done:
            _P94_ENSURE(d);
            jit_buf_printf(cb,
                "    _tsv%d=JS_UNDEFINED;\n"
                "    if(js_jit_iterator_get_value_done(ctx,_tsv%d,&_tsv%d,&_tsv%d)<0)"
                " goto _ex;\n"
                "    JS_FreeValue(ctx,_tsv%d); _tsv%d=JS_UNDEFINED;\n"
                "    _sp=%d;\n",
                d,
                d-1, d-1, d,
                d-2, d-2,
                d+1);
            break;

        /* OP_iterator_next: iter next catch_ph val → iter next catch_ph result (0 net).
         * iter at d-4, next at d-3, catch_ph at d-2, val at d-1.
         * Calls next.call(iter, val); result replaces val at d-1. */
        case OP_iterator_next:
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    if(js_jit_iterator_next_step(ctx,_tsv%d,_tsv%d,_tsv%d,&_tsv%d)<0)"
                " goto _ex;\n",
                d-4, d-3, d-1, d-1);
            break;

        /* OP_iterator_call: iter next catch_ph val → iter next catch_ph result flag (+1).
         * iter at d-4, next at d-3, catch_ph at d-2, val at d-1.
         * flags byte follows opcode. result replaces val at d-1; flag pushed at d. */
        case OP_iterator_call: {
            int _flags = (int)bc[pc+1];
            _P94_ENSURE(d);
            jit_buf_printf(cb,
                "    { int _rf=0; _tsv%d=JS_UNDEFINED;\n"
                "      if(js_jit_iterator_call(ctx,_tsv%d,_tsv%d,%d,&_tsv%d,&_rf)<0)"
                " goto _ex;\n"
                "      _tsv%d=JS_NewBool(ctx,_rf); }\n"
                "    _sp=%d;\n",
                d,
                d-4, d-1, _flags, d-1,
                d,
                d+1);
            break;
        }

        /* ---- Push immediate values ---- */
        /* P11.6: push into _ti{d} (raw int64), skip boxing; _sp = d+1.
         * d = sdt[pc] = depth before push.  gen_st will be JIT_T_INT. */
        case OP_push_i32:
            jit_buf_printf(cb,
                "    _ti%d=(int64_t)(int32_t)%uu; _sp=%d;\n",
                d, bc_u32(&bc[pc+1]), d+1);
            break;
        case OP_push_i8:
            jit_buf_printf(cb,
                "    _ti%d=%dLL; _sp=%d;\n",
                d, (int)(int8_t)bc[pc+1], d+1);
            break;
        case OP_push_i16:
            jit_buf_printf(cb,
                "    _ti%d=%dLL; _sp=%d;\n",
                d, (int)(int16_t)bc_u16(&bc[pc+1]), d+1);
            break;
        case OP_push_minus1:
            jit_buf_printf(cb, "    _ti%d=-1LL; _sp=%d;\n", d, d+1); break;
        case OP_push_0:
            jit_buf_printf(cb, "    _ti%d=0LL; _sp=%d;\n", d, d+1); break;
        case OP_push_1:
            jit_buf_printf(cb, "    _ti%d=1LL; _sp=%d;\n", d, d+1); break;
        case OP_push_2:
            jit_buf_printf(cb, "    _ti%d=2LL; _sp=%d;\n", d, d+1); break;
        case OP_push_3:
            jit_buf_printf(cb, "    _ti%d=3LL; _sp=%d;\n", d, d+1); break;
        case OP_push_4:
            jit_buf_printf(cb, "    _ti%d=4LL; _sp=%d;\n", d, d+1); break;
        case OP_push_5:
            jit_buf_printf(cb, "    _ti%d=5LL; _sp=%d;\n", d, d+1); break;
        case OP_push_6:
            jit_buf_printf(cb, "    _ti%d=6LL; _sp=%d;\n", d, d+1); break;
        case OP_push_7:
            jit_buf_printf(cb, "    _ti%d=7LL; _sp=%d;\n", d, d+1); break;
        case OP_push_false:
            jit_buf_printf(cb, "    _tsv%d=JS_FALSE; _sp=%d;\n", d, d+1); break;
        case OP_push_true:
            jit_buf_printf(cb, "    _tsv%d=JS_TRUE; _sp=%d;\n", d, d+1); break;
        case OP_undefined:
            jit_buf_printf(cb, "    _tsv%d=JS_UNDEFINED; _sp=%d;\n", d, d+1); break;
        case OP_null:
            jit_buf_printf(cb, "    _tsv%d=JS_NULL; _sp=%d;\n", d, d+1); break;
        case OP_push_this:
            jit_buf_printf(cb, "    _tsv%d=_DUP(this_val); _sp=%d;\n", d, d+1); break;
        case OP_push_empty_string:
            /* Has _CHK: set _sp before call (so _ex sees correct depth), then push */
            jit_buf_printf(cb,
                "    { JSValue _v=JS_NewStringLen(ctx,\"\",0);"
                " _sp=%d; _CHK(_v); _tsv%d=_v; _sp=%d; }\n",
                d, d, d+1);
            break;

        /* ---- Constant pool ---- */
        case OP_push_const:
            jit_buf_printf(cb,
                "    _tsv%d=_DUP(cpool[%u]); _sp=%d;\n",
                d, bc_u32(&bc[pc+1]), d+1);
            break;
        case OP_push_const8:
            jit_buf_printf(cb,
                "    _tsv%d=_DUP(cpool[%u]); _sp=%d;\n",
                d, (unsigned)bc[pc+1], d+1);
            break;
        case OP_push_atom_value: {
            /* Push the string representation of an interned atom */
            uint32_t atom = bc_u32(&bc[pc+1]);
            jit_buf_printf(cb,
                "    { JSValue _v=JS_AtomToValue(ctx,(JSAtom)%uu);"
                " _sp=%d; _CHK(_v); _tsv%d=_v; _sp=%d; }\n",
                atom, d, d, d+1);
            break;
        }
        /* ---- P13: OP_fclosure / OP_fclosure8 — closure creation via shadow arrays ---- */
        case OP_fclosure:
        case OP_fclosure8: {
            int cpool_idx = (op == OP_fclosure8) ? (int)bc[pc+1] : (int)bc_u32(&bc[pc+1]);
            JSFunctionBytecode *b_inner = js_jit_cpool_get_fb(b, cpool_idx);
            if (!b_inner) {
                fprintf(stderr, "[JIT] gen_body: fclosure cpool[%d] is not a bytecode function\n", cpool_idx);
                *unsupported_out = 1; free(gen_st); free(gen_hsh); return -1;
            }
            int n_cv = js_jit_fb_get_closure_var_count(b_inner);

            /* 1. Declare the per-closure var_ref pointer array */
            if (n_cv > 0)
                jit_buf_printf(cb, "    { JSVarRef *_vr_%d[%d];\n", pc, n_cv);
            else
                jit_buf_printf(cb, "    {\n");

            /* 2. Fill _vr_PC[i] for each closure_var entry */
            for (int ci = 0; ci < n_cv; ci++) {
                int cv_type = js_jit_fb_get_inner_cv_type(b_inner, ci);
                int cv_vidx = js_jit_fb_get_inner_cv_var_idx(b_inner, ci);
                if (cv_type == JIT_CLOSURE_LOCAL) {
                    int vri = js_jit_fb_get_local_var_ref_idx(b, cv_vidx);
                    /* Create or share JSVarRef for this captured local */
                    jit_buf_printf(cb,
                        "      if(!_sf_vrefs[%d]){\n"
                        "        _sf_vrefs[%d]=js_jit_make_var_ref(ctx,&_cap_buf[%d]);\n"
                        "        if(!_sf_vrefs[%d]){_sp=%d; goto _ex;}\n"
                        "      } else { js_jit_var_ref_dup(_sf_vrefs[%d]); }\n"
                        "      _vr_%d[%d]=_sf_vrefs[%d];\n",
                        vri, vri, cv_vidx, vri, d, vri, pc, ci, vri);
                } else if (cv_type == JIT_CLOSURE_ARG) {
                    int vri = js_jit_fb_get_arg_var_ref_idx(b, cv_vidx);
                    jit_buf_printf(cb,
                        "      if(!_sf_vrefs[%d]){\n"
                        "        _sf_vrefs[%d]=js_jit_make_var_ref(ctx,&_arg_cap_buf[%d]);\n"
                        "        if(!_sf_vrefs[%d]){_sp=%d; goto _ex;}\n"
                        "      } else { js_jit_var_ref_dup(_sf_vrefs[%d]); }\n"
                        "      _vr_%d[%d]=_sf_vrefs[%d];\n",
                        vri, vri, cv_vidx, vri, d, vri, pc, ci, vri);
                } else if (cv_type == JIT_CLOSURE_REF || cv_type == JIT_CLOSURE_GLOBAL_REF) {
                    /* Pass-through: increment ref on the existing var_ref */
                    jit_buf_printf(cb,
                        "      _vr_%d[%d]=js_jit_var_ref_dup(var_refs[%d]);\n",
                        pc, ci, cv_vidx);
                }
                /* other types rejected by scan */
            }

            /* 3. Create the closure object */
            jit_buf_printf(cb,
                "      JSValue _bfunc=_DUP(cpool[%d]); _sp=%d;\n",
                cpool_idx, d);
            if (n_cv > 0)
                jit_buf_printf(cb,
                    "      JSValue _cl=js_jit_create_closure(ctx,_bfunc,_vr_%d,%d);\n",
                    pc, n_cv);
            else
                jit_buf_printf(cb,
                    "      JSValue _cl=js_jit_create_closure(ctx,_bfunc,(JSVarRef**)0,0);\n");
            jit_buf_printf(cb,
                "      _CHK(_cl); _tsv%d=_cl; _sp=%d; }\n",
                d, d+1);
            break;
        }

        /* ---- Stack manipulation ---- */
        /* P9.2: use named slots _tsv{d-1}, _tsv{d}, etc. */
        case OP_drop: /* pop top: depth d -> d-1 */
            /* P14: if the value being dropped is a catch placeholder (pushed by
             * OP_catch), also decrement _catch_depth to keep the runtime counter
             * in sync.  The interpreter uses a tagged value on the value stack; the
             * JIT uses a separate counter that must be explicitly decremented here. */
            if (d > 0 && catch_ph_n > 0 && d-1 == catch_ph_d[catch_ph_n-1]) {
                jit_buf_printf(cb, "    _catch_depth--; _FREE(_tsv%d); _sp=%d;\n",
                               d-1, d-1);
                catch_ph_n--;
            /* P11.6: typed slot lives in _ti/_tsd — no refcount, no _FREE needed */
            /* P10.3: JIT_T_JIT_FUNC (=4) is a JSValue, not a typed slot — exclude it */
            } else if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER &&
                       gen_st[gen_sp-1] <= JIT_T_INT) {
                jit_buf_printf(cb, "    _sp=%d;\n", d-1);
            } else {
                jit_buf_printf(cb, "    _FREE(_tsv%d); _sp=%d;\n", d-1, d-1);
            }
            break;
        case OP_dup: /* peek top, push copy: depth d -> d+1 */
            /* P11.6: INT uses _ti, NUMBER uses _tsd, JSVAL/other uses _tsv+_DUP */
            /* P10.3: JIT_T_JIT_FUNC (=4) is a JSValue, not a typed slot — exclude it */
            if (gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_INT)
                jit_buf_printf(cb, "    _ti%d=_ti%d; _sp=%d;\n", d, d-1, d+1);
            else if (gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_NUMBER)
                jit_buf_printf(cb, "    _tsd%d=_tsd%d; _sp=%d;\n", d, d-1, d+1);
            else
                jit_buf_printf(cb, "    _tsv%d=_DUP(_tsv%d); _sp=%d;\n", d, d-1, d+1);
            break;
        case OP_dup1: /* a b -> a a b (insert dup of a below b): depth d -> d+1
                       * _tsv{d-2}=a, _tsv{d-1}=b
                       * result: _tsv{d-2}=a, _tsv{d-1}=dup(a), _tsv{d}=b */
            /* P9.4: box typed slots — a is DUP'd (_tsv{d-2}), b is moved (_tsv{d-1}) */
            _P94_ENSURE(d-2); _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { JSValue _t=_DUP(_tsv%d);"
                " _tsv%d=_tsv%d; _tsv%d=_t; _sp=%d; }\n",
                d-2, d, d-1, d-1, d+1);
            break;
        case OP_dup2: /* a b -> a b a b: depth d -> d+2 */
            /* P9.4: box typed slots before DUP reads _tsv{d-2} and _tsv{d-1} */
            _P94_ENSURE(d-2); _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    _tsv%d=_DUP(_tsv%d); _tsv%d=_DUP(_tsv%d); _sp=%d;\n",
                d, d-2, d+1, d-1, d+2);
            break;
        case OP_insert2: /* obj a -> a obj a (dup_x1): depth d -> d+1
                          * _tsv{d-2}=obj, _tsv{d-1}=a
                          * result: _tsv{d-2}=dup(a), _tsv{d-1}=obj, _tsv{d}=a */
            /* P9.4: box typed slots — a is DUP'd (_tsv{d-1}), obj is moved (_tsv{d-2}) */
            _P94_ENSURE(d-1); _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    { JSValue _t=_DUP(_tsv%d);"
                " _tsv%d=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_t; _sp=%d; }\n",
                d-1, d, d-1, d-1, d-2, d-2, d+1);
            break;
        /* OP_pop does not exist; OP_drop handles the pop case */
        case OP_nip: /* a b -> b: depth d -> d-1 */
            /* P9.4: box typed slots — b is kept (_tsv{d-1}), a is freed (_tsv{d-2}) */
            _P94_ENSURE(d-1); _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    { JSValue _t=_tsv%d; _FREE(_tsv%d); _tsv%d=_t; _sp=%d; }\n",
                d-1, d-2, d-2, d-1);
            break;
        case OP_swap: /* a b -> b a: depth unchanged */
            /* P9.4: box any typed slots before performing the JSValue swap */
            _P94_ENSURE(d-2);
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { JSValue _t=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_t; }\n",
                d-1, d-1, d-2, d-2);
            break;
        case OP_rot3l: /* a b c -> b c a: depth unchanged */
            /* P9.4: box any typed slots before the rotation */
            _P94_ENSURE(d-3); _P94_ENSURE(d-2); _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { JSValue _t=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_t; }\n",
                d-3, d-3, d-2, d-2, d-1, d-1);
            break;
        case OP_rot3r: /* a b c -> c a b: depth unchanged */
            /* P9.4: box any typed slots before the rotation */
            _P94_ENSURE(d-3); _P94_ENSURE(d-2); _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { JSValue _t=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_t; }\n",
                d-1, d-1, d-2, d-2, d-3, d-3);
            break;

        /* ---- P18: new stack-shuffle opcodes ---- */
        case OP_dup3: /* a b c -> a b c a b c: depth d -> d+3 */
            /* P9.4: box typed source slots before DUP reads them */
            _P94_ENSURE(d-3); _P94_ENSURE(d-2); _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    _tsv%d=_DUP(_tsv%d); _tsv%d=_DUP(_tsv%d); _tsv%d=_DUP(_tsv%d); _sp=%d;\n",
                d, d-3, d+1, d-2, d+2, d-1, d+3);
            break;
        case OP_nip1: /* a b c -> b c: depth d -> d-1 (removes 3rd-from-top) */
            _P94_ENSURE(d-3);
            jit_buf_printf(cb,
                "    { JSValue _t1=_tsv%d,_t2=_tsv%d; _FREE(_tsv%d); _tsv%d=_t1; _tsv%d=_t2; _sp=%d; }\n",
                d-2, d-1, d-3, d-3, d-2, d-1);
            break;
        case OP_insert3: /* obj prop a -> a obj prop a: depth d -> d+1 */
            _P94_ENSURE(d-1); _P94_ENSURE(d-2); _P94_ENSURE(d-3);
            jit_buf_printf(cb,
                "    { JSValue _t=_DUP(_tsv%d);"
                " _tsv%d=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_t; _sp=%d; }\n",
                d-1, d, d-1, d-1, d-2, d-2, d-3, d-3, d+1);
            break;
        case OP_insert4: /* this obj prop a -> a this obj prop a: depth d -> d+1 */
            _P94_ENSURE(d-1); _P94_ENSURE(d-2); _P94_ENSURE(d-3); _P94_ENSURE(d-4);
            jit_buf_printf(cb,
                "    { JSValue _t=_DUP(_tsv%d);"
                " _tsv%d=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_t; _sp=%d; }\n",
                d-1, d, d-1, d-1, d-2, d-2, d-3, d-3, d-4, d-4, d+1);
            break;
        case OP_perm3: /* obj a b -> a obj b: swap sp[-3] and sp[-2] */
            _P94_ENSURE(d-3); _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    { JSValue _t=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_t; }\n",
                d-2, d-2, d-3, d-3);
            break;
        case OP_perm4: /* obj prop a b -> a obj prop b */
            _P94_ENSURE(d-4); _P94_ENSURE(d-3); _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    { JSValue _t=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_t; }\n",
                d-2, d-2, d-3, d-3, d-4, d-4);
            break;
        case OP_perm5: /* this obj prop a b -> a this obj prop b */
            _P94_ENSURE(d-5); _P94_ENSURE(d-4); _P94_ENSURE(d-3); _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    { JSValue _t=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_tsv%d;"
                " _tsv%d=_tsv%d; _tsv%d=_t; }\n",
                d-2, d-2, d-3, d-3, d-4, d-4, d-5, d-5);
            break;
        case OP_rot4l: /* x a b c -> a b c x */
            _P94_ENSURE(d-4); _P94_ENSURE(d-3); _P94_ENSURE(d-2); _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { JSValue _t=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_tsv%d;"
                " _tsv%d=_tsv%d; _tsv%d=_t; }\n",
                d-4, d-4, d-3, d-3, d-2, d-2, d-1, d-1);
            break;
        case OP_rot5l: /* x a b c d -> a b c d x */
            _P94_ENSURE(d-5); _P94_ENSURE(d-4); _P94_ENSURE(d-3); _P94_ENSURE(d-2); _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { JSValue _t=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_tsv%d;"
                " _tsv%d=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_t; }\n",
                d-5, d-5, d-4, d-4, d-3, d-3, d-2, d-2, d-1, d-1);
            break;
        case OP_swap2: /* a b c d -> c d a b */
            _P94_ENSURE(d-4); _P94_ENSURE(d-3); _P94_ENSURE(d-2); _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { JSValue _t1=_tsv%d,_t2=_tsv%d;"
                " _tsv%d=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_t1; _tsv%d=_t2; }\n",
                d-4, d-3, d-4, d-2, d-3, d-1, d-2, d-1);
            break;

        /* ---- Local variable access (Phase 5 / P8.1: type-aware) ----
         *
         * JIT_T_INT    → int64_t _li[idx]: single int64 op, no branch in hot path.
         *   get_loc: box to JS_NewInt32 (or JS_NewFloat64 if > INT32_MAX — rare).
         *   put_loc: extract int64 from JSValue (INT → direct, FLOAT64 → truncate).
         *   inc/dec/add_loc: pure int64 arithmetic with no boxing at all.
         *
         * JIT_T_NUMBER → double _ld[idx]: GCC CSE/vectorise for float-heavy loops.
         *
         * _l[idx] stays JS_UNDEFINED for both → _FREE(_l[idx]) in footer is safe. */
#define _IS_INT(idx) \
    (local_type && (idx) >= 0 && (idx) < var_count && \
     local_type[(idx)] == JIT_T_INT)
#define _IS_NUM(idx) \
    (local_type && (idx) >= 0 && (idx) < var_count && \
     local_type[(idx)] == JIT_T_NUMBER)

/* P11.6: INT locals use _ti{d}, NUMBER locals use _tsd{d}, JSVAL use _tsv{d}. */
#define _CAP_LOC(idx) (sr->has_fclosure && (idx) >= 0 && (idx) < 64 && \
                       (((sr->captured_local_mask) >> (idx)) & 1))
/* _CAP_ARG: true when arg idx should be accessed via _arg_cap_buf[] instead of argv[].
 * Covers both closure-captured args and (for non-generator functions) args written by
 * OP_put_arg/OP_set_arg — the latter use a local copy to keep argv[] immutable so the
 * P10.3 direct-call path (which lends argv to the callee) cannot double-free. */
#define _CAP_ARG(idx) ((idx) >= 0 && (idx) < 64 && \
                       (((sr->captured_arg_mask | \
                          (sr->has_yield ? 0ULL : sr->mutated_arg_mask)) >> (idx)) & 1))

#define GEN_GET_LOC(idx) do { \
    if (_IS_INT(idx)) \
        jit_buf_printf(cb, \
            "    _ti%d=_jsi_%s; _sp=%d;\n", d, LNAME(idx), d+1); \
    else if (_IS_NUM(idx)) \
        jit_buf_printf(cb, \
            "    _tsd%d=_jsd_%s; _sp=%d;\n", d, LNAME(idx), d+1); \
    else if (_CAP_LOC(idx)) \
        jit_buf_printf(cb, "    _tsv%d=_DUP(_cap_buf[%d]); _sp=%d;\n", d, (idx), d+1); \
    else \
        jit_buf_printf(cb, "    _tsv%d=_DUP(_jsv_%s); _sp=%d;\n", d, LNAME(idx), d+1); \
} while(0)

/* P38.1 (was P37.3): borrowed variant — omits DupValue for get_loc when the
 * immediately following opcode is get_field.  The object refcount is NOT
 * incremented here; get_field must NOT call _FREE(_o) in this case.
 * Only applies to JSVAL locals (not INT/NUM — those are type-converted). */
#define GEN_GET_LOC_BORROW(idx) do { \
    if (_IS_INT(idx) || _IS_NUM(idx)) { \
        GEN_GET_LOC(idx); /* typed: can't borrow, fall back to normal */ \
        _borrowed_depth = -1; \
    } else if (_CAP_LOC(idx)) { \
        jit_buf_printf(cb, "    _tsv%d=_DUP(_cap_buf[%d]); _sp=%d;\n", d, (idx), d+1); \
        _borrowed_depth = -1; /* captured local: DupValue needed (ref outside frame) */ \
    } else { \
        jit_buf_printf(cb, "    _tsv%d=_jsv_%s; _sp=%d;\n", d, LNAME(idx), d+1); \
        _borrowed_depth = d;  /* no DupValue — get_field must skip _FREE(_o) */ \
    } \
} while(0)

/* P37.3: look-ahead helper: returns 1 if the opcode at next_pc is get_field.
 * Only get_field (not get_field2) is eligible for the borrow optimization:
 * get_field pops the object (calls _FREE), so skipping DupValue + _FREE is
 * safe.  get_field2 keeps the object on the stack; borrowing would leave a
 * dangling alias after the stack slot is later popped. */
#define _NEXT_IS_GET_FIELD(next_pc) \
    ((next_pc) < bc_len && bc[(next_pc)] == OP_get_field)

/* P38.1: 2-opcode look-ahead: next is any get_loc variant (idx) AND next+sz is get_array_el.
 * Used to detect arr[i] pattern and skip DupValue for arr.
 * Includes short-form opcodes OP_get_loc0..3 and OP_get_loc8 since the compiler
 * may use those for locals at indices 0..3 or 0..255. */
#define _IS_GET_LOC_OP(opcode) \
    ((opcode) == OP_get_loc || (opcode) == OP_get_loc_check || \
     (opcode) == OP_get_loc0 || (opcode) == OP_get_loc1 || \
     (opcode) == OP_get_loc2 || (opcode) == OP_get_loc3 || \
     (opcode) == OP_get_loc8)
#define _NEXT2_IS_ARRAY_GET(next_pc) \
    ((next_pc) < bc_len && \
     _IS_GET_LOC_OP(bc[(next_pc)]) && \
     (next_pc) + op_sz[bc[(next_pc)]] < bc_len && \
     bc[(next_pc) + op_sz[bc[(next_pc)]]] == OP_get_array_el)

/* P39.3: 2-opcode look-ahead: next is any get_loc variant AND next+sz is put_field.
 * Detects the o.x = val pattern and enables borrow elision for obj on the write side. */
#define _NEXT2_IS_PUT_FIELD(next_pc) \
    ((next_pc) < bc_len && \
     _IS_GET_LOC_OP(bc[(next_pc)]) && \
     (next_pc) + op_sz[bc[(next_pc)]] < bc_len && \
     bc[(next_pc) + op_sz[bc[(next_pc)]]] == OP_put_field)

#define GEN_PUT_LOC(idx) do { \
    if (_IS_INT(idx) && gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_INT) { \
        /* P11.6: INT source → read _ti directly */ \
        jit_buf_printf(cb, "    _jsi_%s=_ti%d; _sp=%d;\n", LNAME(idx), d-1, d-1); \
    } else if (_IS_INT(idx) && gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_NUMBER) { \
        /* P9.4: NUMBER source → read _tsd directly */ \
        jit_buf_printf(cb, "    _jsi_%s=(int64_t)_tsd%d; _sp=%d;\n", LNAME(idx), d-1, d-1); \
    } else if (_IS_INT(idx)) { \
        jit_buf_printf(cb, \
            "    { JSValue _t=_tsv%d; _sp=%d;" \
            " _jsi_%s=(JS_VALUE_GET_TAG(_t)==JS_TAG_INT)" \
            "?(int64_t)JS_VALUE_GET_INT(_t):(int64_t)JS_VALUE_GET_FLOAT64(_t); }\n", \
            d-1, d-1, LNAME(idx)); \
    } else if (_IS_NUM(idx) && gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) { \
        /* P9.4: typed source → read _tsd directly */ \
        jit_buf_printf(cb, "    _jsd_%s=_tsd%d; _sp=%d;\n", LNAME(idx), d-1, d-1); \
    } else if (_IS_NUM(idx)) { \
        jit_buf_printf(cb, \
            "    { JSValue _t=_tsv%d; _sp=%d;" \
            " _jsd_%s=(JS_VALUE_GET_TAG(_t)==JS_TAG_INT)" \
            "?(double)JS_VALUE_GET_INT(_t):JS_VALUE_GET_FLOAT64(_t); }\n", d-1, d-1, LNAME(idx)); \
    } else if (_CAP_LOC(idx)) { \
        /* P13: captured local — pop into canonical _cap_buf shadow slot. */ \
        _P94_ENSURE(d-1); \
        jit_buf_printf(cb, "    { _FREE(_cap_buf[%d]); _cap_buf[%d]=_tsv%d; _sp=%d; }\n", \
                       idx, idx, d-1, d-1); \
    } else { \
        _P94_ENSURE(d-1); /* P9.4: box typed slot before storing as JSValue */ \
        jit_buf_printf(cb, "    _FREE(_jsv_%s); _jsv_%s=_tsv%d; _sp=%d;\n", \
                       LNAME(idx), LNAME(idx), d-1, d-1); \
    } \
} while(0)

#define GEN_SET_LOC(idx) do { \
    if (_IS_INT(idx) && gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_INT) { \
        /* P11.6: INT source → read _ti directly, non-destructive peek */ \
        jit_buf_printf(cb, "    _jsi_%s=_ti%d;\n", LNAME(idx), d-1); \
    } else if (_IS_INT(idx) && gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_NUMBER) { \
        /* P9.4: NUMBER source → read _tsd directly, non-destructive peek */ \
        jit_buf_printf(cb, "    _jsi_%s=(int64_t)_tsd%d;\n", LNAME(idx), d-1); \
    } else if (_IS_INT(idx)) { \
        jit_buf_printf(cb, \
            "    { JSValue _t=_tsv%d;" \
            " _jsi_%s=(JS_VALUE_GET_TAG(_t)==JS_TAG_INT)" \
            "?(int64_t)JS_VALUE_GET_INT(_t):(int64_t)JS_VALUE_GET_FLOAT64(_t); }\n", \
            d-1, LNAME(idx)); \
    } else if (_IS_NUM(idx) && gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) { \
        /* P9.4: typed source → read _tsd directly, non-destructive peek */ \
        jit_buf_printf(cb, "    _jsd_%s=_tsd%d;\n", LNAME(idx), d-1); \
    } else if (_IS_NUM(idx)) { \
        jit_buf_printf(cb, \
            "    { JSValue _t=_tsv%d;" \
            " _jsd_%s=(JS_VALUE_GET_TAG(_t)==JS_TAG_INT)" \
            "?(double)JS_VALUE_GET_INT(_t):JS_VALUE_GET_FLOAT64(_t); }\n", d-1, LNAME(idx)); \
    } else if (_CAP_LOC(idx)) { \
        /* P13: captured local — peek into canonical _cap_buf shadow slot (no pop). */ \
        _P94_ENSURE(d-1); \
        jit_buf_printf(cb, "    { _FREE(_cap_buf[%d]); _cap_buf[%d]=_DUP(_tsv%d); }\n", \
                       idx, idx, d-1); \
    } else { \
        _P94_ENSURE(d-1); /* P9.4: box typed slot before storing as JSValue */ \
        jit_buf_printf(cb, "    _FREE(_jsv_%s); _jsv_%s=_DUP(_tsv%d);\n", \
                       LNAME(idx), LNAME(idx), d-1); \
    } \
} while(0)

        /* P38.1 (was P37.3): get_loc variants use GEN_GET_LOC_BORROW when next
         * opcode is get_field, or 2-opcode look-ahead for arr[i] pattern. */
        case OP_get_loc:  case OP_get_loc_check:
        case OP_get_loc_checkthis:
        {
            int _loc_idx = (int)bc_u16(&bc[pc+1]);
            if (_NEXT_IS_GET_FIELD(pc + sz)) {
                GEN_GET_LOC_BORROW(_loc_idx);
                /* _borrowed_depth updated inside GEN_GET_LOC_BORROW */
            } else if (_NEXT2_IS_ARRAY_GET(pc + sz) &&
                       !_IS_INT(_loc_idx) && !_IS_NUM(_loc_idx) && !_CAP_LOC(_loc_idx)) {
                /* 2-ahead: this get_loc pushes the array obj; next is get_loc idx;
                 * after that is get_array_el.  Borrow obj — no DupValue.
                 * gen_st push is handled by the secondary pass (phase 6.1 below). */
                jit_buf_printf(cb, "    _tsv%d=_jsv_%s; _sp=%d;\n", d, LNAME(_loc_idx), d+1);
                _borrowed_depth = d;
            } else if (_NEXT2_IS_PUT_FIELD(pc + sz) &&  /* P39.3 */
                       !_IS_INT(_loc_idx) && !_IS_NUM(_loc_idx) && !_CAP_LOC(_loc_idx)) {
                /* 2-ahead: this get_loc pushes the object; next is get_loc val;
                 * after that is put_field.  Borrow obj — no DupValue. */
                jit_buf_printf(cb, "    _tsv%d=_jsv_%s; _sp=%d;\n", d, LNAME(_loc_idx), d+1);
                _borrowed_depth = d;
            } else {
                GEN_GET_LOC(_loc_idx);
                /* do NOT reset _borrowed_depth here — it may be set by prior get_loc for arr */
            }
            break;
        }
        case OP_put_loc:  case OP_put_loc_check:
        case OP_put_loc_check_init: GEN_PUT_LOC((int)bc_u16(&bc[pc+1])); _borrowed_depth = -1; break;
        case OP_set_loc:  GEN_SET_LOC((int)bc_u16(&bc[pc+1])); _borrowed_depth = -1; break;
        /* TDZ init: mark local as uninitialized — skip in JIT (no TDZ checking) */
        case OP_set_loc_uninitialized: break;
        case OP_get_loc8:
        {
            int _loc8 = (int)bc[pc+1];
            if (_NEXT_IS_GET_FIELD(pc + sz))
                GEN_GET_LOC_BORROW(_loc8);
            else if (_NEXT2_IS_ARRAY_GET(pc + sz) &&
                     !_IS_INT(_loc8) && !_IS_NUM(_loc8) && !_CAP_LOC(_loc8)) {
                jit_buf_printf(cb, "    _tsv%d=_jsv_%s; _sp=%d;\n", d, LNAME(_loc8), d+1);
                _borrowed_depth = d;
            } else if (_NEXT2_IS_PUT_FIELD(pc + sz) &&  /* P39.3 */
                       !_IS_INT(_loc8) && !_IS_NUM(_loc8) && !_CAP_LOC(_loc8)) {
                jit_buf_printf(cb, "    _tsv%d=_jsv_%s; _sp=%d;\n", d, LNAME(_loc8), d+1);
                _borrowed_depth = d;
            } else { GEN_GET_LOC(_loc8); } /* do NOT reset _borrowed_depth here */
            break;
        }
        case OP_put_loc8: GEN_PUT_LOC((int)bc[pc+1]); _borrowed_depth = -1; break;
        case OP_set_loc8: GEN_SET_LOC((int)bc[pc+1]); _borrowed_depth = -1; break;
/* P38.1: helper macro for get_loc0..3 borrow extension.
 * Note: the else branch does NOT reset _borrowed_depth — it may have been set
 * by the previous get_loc (for arr) and must survive until get_array_el. */
#define _GEN_GET_LOC_N(n) do { \
    if (_NEXT_IS_GET_FIELD(pc + sz)) GEN_GET_LOC_BORROW(n); \
    else if (_NEXT2_IS_ARRAY_GET(pc + sz) && \
             !_IS_INT(n) && !_IS_NUM(n) && !_CAP_LOC(n)) { \
        jit_buf_printf(cb, "    _tsv%d=_jsv_%s; _sp=%d;\n", d, LNAME(n), d+1); \
        _borrowed_depth = d; \
    } else if (_NEXT2_IS_PUT_FIELD(pc + sz) &&  /* P39.3 */ \
               !_IS_INT(n) && !_IS_NUM(n) && !_CAP_LOC(n)) { \
        jit_buf_printf(cb, "    _tsv%d=_jsv_%s; _sp=%d;\n", d, LNAME(n), d+1); \
        _borrowed_depth = d; \
    } else { GEN_GET_LOC(n); } \
} while(0)
        case OP_get_loc0: _GEN_GET_LOC_N(0); break;
        case OP_get_loc1: _GEN_GET_LOC_N(1); break;
        case OP_get_loc2: _GEN_GET_LOC_N(2); break;
        case OP_get_loc3: _GEN_GET_LOC_N(3); break;
#undef _GEN_GET_LOC_N
        case OP_put_loc0: GEN_PUT_LOC(0); break;
        case OP_put_loc1: GEN_PUT_LOC(1); break;
        case OP_put_loc2: GEN_PUT_LOC(2); break;
        case OP_put_loc3: GEN_PUT_LOC(3); break;
        case OP_set_loc0: GEN_SET_LOC(0); break;
        case OP_set_loc1: GEN_SET_LOC(1); break;
        case OP_set_loc2: GEN_SET_LOC(2); break;
        case OP_set_loc3: GEN_SET_LOC(3); break;

#undef _IS_INT
#undef _IS_NUM
#undef GEN_GET_LOC
#undef GEN_GET_LOC_BORROW
#undef _NEXT_IS_GET_FIELD
#undef _IS_GET_LOC_OP
#undef _NEXT2_IS_ARRAY_GET
#undef _NEXT2_IS_PUT_FIELD
#undef GEN_PUT_LOC
#undef GEN_SET_LOC

        /* ---- Argument access (P8.4: int-arg fast path) ----
         * _aim bit i: argv[i] is a JS_TAG_INT and its value is live in _ai[i].
         * GEN_GET_ARG: prefer _ai[i] (register-friendly int32) over argv[i] load.
         * GEN_PUT/SET_ARG: keep _ai[i] and _aim consistent on arg writes. */
/* P9.2: arg access macros use _tsv{d} for push, _tsv{d-1} for pop/peek. */
#define _AI_VALID(idx) ((idx) < 32)
/* P33 note: for complex-param functions (_has_complex_params), arg_buf is
 * always padded to arg_count by the caller and the JIT receives the ORIGINAL
 * argc (not the padded count).  All arg slots 0..arg_count-1 are therefore
 * valid to read/write unconditionally — the argc-bound checks would give wrong
 * results for rest/default slots beyond the original argc.
 * For simple-param functions the argc check is still correct. */
#define GEN_GET_ARG(idx) do { \
    if (_CAP_ARG(idx)) \
        /* Captured arg: read from shadow slot (canonical location for var refs) */ \
        jit_buf_printf(cb, \
            "    _tsv%d=_DUP(_arg_cap_buf[%d]); _sp=%d;\n", d, (idx), d+1); \
    else if (_AI_VALID(idx)) { \
        if (_has_complex_params) \
            /* Complex params: unconditional — arg_buf padded, argc is original */ \
            jit_buf_printf(cb, \
                "    _tsv%d=((_aim>>%du&1u))" \
                "?JS_MKVAL(JS_TAG_INT,_jai_%s)" \
                ":_DUP(argv[%d]); _sp=%d;\n", \
                d, (unsigned)(idx), ANAME(idx), idx, d+1); \
        else \
            jit_buf_printf(cb, \
                "    _tsv%d=((%d)<argc&&(_aim>>%du&1u))" \
                "?JS_MKVAL(JS_TAG_INT,_jai_%s)" \
                ":((%d)<argc?_DUP(argv[%d]):JS_UNDEFINED); _sp=%d;\n", \
                d, idx, (unsigned)(idx), ANAME(idx), idx, idx, d+1); \
    } else { \
        if (_has_complex_params) \
            jit_buf_printf(cb, \
                "    _tsv%d=_DUP(argv[%d]); _sp=%d;\n", d, idx, d+1); \
        else \
            jit_buf_printf(cb, \
                "    _tsv%d=((%d)<argc?_DUP(argv[%d]):JS_UNDEFINED); _sp=%d;\n", \
                d, idx, idx, d+1); \
    } \
} while(0)
#define GEN_PUT_ARG(idx) do { \
    _P94_ENSURE(d-1); \
    if (_CAP_ARG(idx)) \
        /* Captured arg: update shadow slot, discard argv copy */ \
        jit_buf_printf(cb, \
            "    { _FREE(_arg_cap_buf[%d]); _arg_cap_buf[%d]=_tsv%d; _sp=%d; }\n", \
            (idx), (idx), d-1, d-1); \
    else if (_AI_VALID(idx)) { \
        if (_has_complex_params) \
            /* Complex params: unconditional write (all slots always valid) */ \
            jit_buf_printf(cb, \
                "    { JSValue _t=_tsv%d; _sp=%d;\n" \
                "      if(JS_VALUE_GET_TAG(_t)==JS_TAG_INT){_jai_%s=JS_VALUE_GET_INT(_t);_aim|=%uu;}else{_aim&=~%uu;}\n" \
                "      _FREE(argv[%d]);argv[%d]=_t; }\n", \
                d-1, d-1, ANAME(idx), 1u<<(unsigned)(idx), 1u<<(unsigned)(idx), idx, idx); \
        else \
            jit_buf_printf(cb, \
                "    if((%d)<argc){ JSValue _t=_tsv%d; _sp=%d;\n" \
                "      if(JS_VALUE_GET_TAG(_t)==JS_TAG_INT){_jai_%s=JS_VALUE_GET_INT(_t);_aim|=%uu;}else{_aim&=~%uu;}\n" \
                "      _FREE(argv[%d]);argv[%d]=_t;}else{ _FREE(_tsv%d); _sp=%d; }\n", \
                idx, d-1, d-1, ANAME(idx), 1u<<(unsigned)(idx), 1u<<(unsigned)(idx), idx, idx, d-1, d-1); \
    } else { \
        if (_has_complex_params) \
            /* Complex params: unconditional write */ \
            jit_buf_printf(cb, \
                "    { _FREE(argv[%d]); argv[%d]=_tsv%d; _sp=%d; }\n", \
                idx, idx, d-1, d-1); \
        else \
            jit_buf_printf(cb, \
                "    if((%d)<argc){_FREE(argv[%d]); argv[%d]=_tsv%d; _sp=%d;}else{ _FREE(_tsv%d); _sp=%d; }\n", \
                idx, idx, idx, d-1, d-1, d-1, d-1); \
    } \
} while(0)
#define GEN_SET_ARG(idx) do { \
    _P94_ENSURE(d-1); \
    if (_CAP_ARG(idx)) \
        /* Captured arg: update shadow slot (set_arg is non-destructive peek) */ \
        jit_buf_printf(cb, \
            "    { _FREE(_arg_cap_buf[%d]); _arg_cap_buf[%d]=_DUP(_tsv%d); }\n", \
            (idx), (idx), d-1); \
    else if (_AI_VALID(idx)) { \
        if (_has_complex_params) \
            /* Complex params: unconditional */ \
            jit_buf_printf(cb, \
                "    { JSValue _t=_tsv%d;\n" \
                "      if(JS_VALUE_GET_TAG(_t)==JS_TAG_INT){_jai_%s=JS_VALUE_GET_INT(_t);_aim|=%uu;}else{_aim&=~%uu;}\n" \
                "      _FREE(argv[%d]);argv[%d]=_DUP(_t); };\n", \
                d-1, ANAME(idx), 1u<<(unsigned)(idx), 1u<<(unsigned)(idx), idx, idx); \
        else \
            jit_buf_printf(cb, \
                "    if((%d)<argc){ JSValue _t=_tsv%d;\n" \
                "      if(JS_VALUE_GET_TAG(_t)==JS_TAG_INT){_jai_%s=JS_VALUE_GET_INT(_t);_aim|=%uu;}else{_aim&=~%uu;}\n" \
                "      _FREE(argv[%d]);argv[%d]=_DUP(_t);};\n", \
                idx, d-1, ANAME(idx), 1u<<(unsigned)(idx), 1u<<(unsigned)(idx), idx, idx); \
    } else { \
        if (_has_complex_params) \
            jit_buf_printf(cb, \
                "    { _FREE(argv[%d]); argv[%d]=_DUP(_tsv%d); };\n", \
                idx, idx, d-1); \
        else \
            jit_buf_printf(cb, \
                "    if((%d)<argc){_FREE(argv[%d]); argv[%d]=_DUP(_tsv%d);};\n", \
                idx, idx, idx, d-1); \
    } \
} while(0)

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

#undef _AI_VALID
#undef GEN_GET_ARG
#undef GEN_PUT_ARG
#undef GEN_SET_ARG

        /* ---- Closure variable access ---- */
/* P9.2: closure var access uses _tsv{d} for push, _tsv{d-1} for pop/peek.
 * _VRV(idx) returns a pointer to the JSValue stored inside var_refs[idx].
 * P40.2: use _vrp{idx} (pvalue pointer cached in preamble) instead of a vtable
 * call.  JSVarRef is an opaque type in generated C; _vrp{idx} was set up in
 * the preamble via JIT_VARREF_PVALUE_OFF so no complete-type access is needed. */
/* P49: warm INT — skip DupValue (INT is immediate), extract INT from var_ref cell.
 * Cold/JSVAL: observe cell tag for warm recompile, read with DupValue. */
#define GEN_GET_VR(idx) do { \
    if (vt_hints \
        && (n_gf + n_ae + n_pf + vr_idx) < (n_gf + n_ae + n_pf + n_vr) \
        && vt_hints[n_gf + n_ae + n_pf + vr_idx] == 0 /* JS_TAG_INT */) { \
        /* Warm INT: speculative — valid when gen_state later tracks this slot as INT. */ \
        jit_buf_printf(cb, \
            "    _ti%d=(int64_t)JS_VALUE_GET_INT(*_vrp%d); _sp=%d;\n", \
            d, (idx), d+1); \
    } else { \
        /* Cold/JSVAL: read cell once, record tag, DupValue. */ \
        jit_buf_printf(cb, \
            "    { JSValue _rv%d=*_vrp%d;" \
            " __jit_vt_%016llx[%d]=(uint8_t)JS_VALUE_GET_TAG(_rv%d);" \
            " _tsv%d=_DUP(_rv%d); _sp=%d; }\n", \
            pc, (idx), \
            (unsigned long long)bc_hash, n_gf + n_ae + n_pf + vr_idx, pc, \
            d, pc, d+1); \
    } \
} while(0)
/* P40.3: when the source slot is a raw int64_t (_ti{d-1}), skip boxing with
 * _P94_ENSURE and store JS_MKVAL(TAG_INT, ...) directly.  The js_unlikely hint
 * on the refcount branch lets GCC eliminate it for integer old values.
 * P52: warm INT old-value hint skips JS_VALUE_HAS_REF_COUNT entirely. */
#define GEN_PUT_VR(idx) do { \
    if (gen_st[d-1] == JIT_T_INT) { \
        int _pv_base = n_gf + n_ae + n_pf + n_vr + n_pa + n_ad * 2; \
        int _p52_warm = (vt_hints \
                         && _pv_base + pv_idx < _pv_base + n_pv \
                         && vt_hints[_pv_base + pv_idx] == 0 /* JS_TAG_INT */); \
        if (_p52_warm) { \
            /* Warm: old value was INT (no refcount) — skip free entirely. */ \
            jit_buf_printf(cb, \
                "    *_vrp%d=JS_MKVAL(JS_TAG_INT,(int32_t)_ti%d); _sp=%d;\n", \
                idx, d-1, d-1); \
        } else { \
            /* Cold: record old-value tag, then do the refcount check. */ \
            jit_buf_printf(cb, \
                "    { JSValue _nv=JS_MKVAL(JS_TAG_INT,(int32_t)_ti%d);" \
                " __jit_vt_%016llx[%d]=(uint8_t)JS_VALUE_GET_TAG(*_vrp%d);" \
                " if(js_unlikely(JS_VALUE_HAS_REF_COUNT(*_vrp%d))) _RT->free_value(ctx,*_vrp%d);" \
                " *_vrp%d=_nv; _sp=%d; }\n", \
                d-1, (unsigned long long)bc_hash, _pv_base + pv_idx, \
                idx, idx, idx, idx, d-1); \
        } \
    } else { \
        _P94_ENSURE(d-1); /* P9.4: box typed slot before storing as JSValue */ \
        jit_buf_printf(cb, "    { _FREE(*_vrp%d); *_vrp%d=_tsv%d; _sp=%d; }\n", \
                       idx, idx, d-1, d-1); \
    } \
} while(0)
#define GEN_SET_VR(idx) do { \
    if (gen_st[d-1] == JIT_T_INT) { \
        int _pv_base = n_gf + n_ae + n_pf + n_vr + n_pa + n_ad * 2; \
        int _p52_warm = (vt_hints \
                         && _pv_base + pv_idx < _pv_base + n_pv \
                         && vt_hints[_pv_base + pv_idx] == 0 /* JS_TAG_INT */); \
        if (_p52_warm) { \
            /* Warm: old value was INT — skip free entirely. */ \
            jit_buf_printf(cb, \
                "    *_vrp%d=JS_MKVAL(JS_TAG_INT,(int32_t)_ti%d);\n", \
                idx, d-1); \
        } else { \
            /* Cold: record old-value tag, then do the refcount check. */ \
            jit_buf_printf(cb, \
                "    { JSValue _nv=JS_MKVAL(JS_TAG_INT,(int32_t)_ti%d);" \
                " __jit_vt_%016llx[%d]=(uint8_t)JS_VALUE_GET_TAG(*_vrp%d);" \
                " if(js_unlikely(JS_VALUE_HAS_REF_COUNT(*_vrp%d))) _RT->free_value(ctx,*_vrp%d);" \
                " *_vrp%d=_nv; }\n", \
                d-1, (unsigned long long)bc_hash, _pv_base + pv_idx, \
                idx, idx, idx, idx); \
        } \
    } else { \
        _P94_ENSURE(d-1); /* P9.4: box typed slot before storing as JSValue */ \
        jit_buf_printf(cb, "    { _FREE(*_vrp%d); *_vrp%d=_DUP(_tsv%d); }\n", \
                       idx, idx, d-1); \
    } \
} while(0)

        case OP_get_var_ref: GEN_GET_VR((int)bc_u16(&bc[pc+1])); break;
        case OP_get_var_ref_check: {
            /* OP_get_var_ref_check reads a lexical var that may legitimately be
             * JS_TAG_UNINITIALIZED (e.g. 'this' before super() in a derived ctor).
             * Cannot skip the TDZ check unlike OP_get_var_ref. */
            int idx = (int)bc_u16(&bc[pc+1]);
            JSAtom cv_atom = js_jit_fb_get_closure_var_atom(b, idx);
            jit_buf_printf(cb,
                "    { JSValue _rv%d=*_vrp%d;\n"
                "      if(js_unlikely(JS_VALUE_GET_TAG(_rv%d)==JS_TAG_UNINITIALIZED)){\n"
                "        _RT->throw_error(ctx,(JSAtom)%uu,2); goto _ex;\n"
                "      }\n"
                "      __jit_vt_%016llx[%d]=(uint8_t)JS_VALUE_GET_TAG(_rv%d);\n"
                "      _tsv%d=_DUP(_rv%d); _sp=%d;\n"
                "    }\n",
                pc, idx,
                pc,
                (unsigned)cv_atom,
                (unsigned long long)bc_hash, n_gf + n_ae + n_pf + vr_idx, pc,
                d, pc, d+1);
            break;
        }
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

        /* ---- Global/closure variable access (u16 index into var_refs) ----
         *
         * OP_get_var / OP_put_var: same as get/put_var_ref but with a 16-bit
         * index.  TDZ and const checks are skipped — the JIT only runs on hot
         * functions that have already executed successfully many times, so any
         * TDZ violation would have been caught by the interpreter.
         * ------------------------------------------------------------------ */
        case OP_get_var: {
            int idx = (int)bc_u16(&bc[pc+1]);
            /* Must check for JS_UNINITIALIZED: a non-lexical global var may
             * have been deleted at runtime (delete gvar).  In that case fall
             * back to JS_GetPropertyInternal which throws ReferenceError.
             * Bake the atom number and is_lexical flag into the generated C
             * so the slow path needs no closure_var access at runtime. */
            JSAtom cv_atom     = js_jit_fb_get_closure_var_atom(b, idx);
            int    cv_is_lex   = js_jit_fb_get_closure_var_is_lexical(b, idx);
            /* P9.2: push into _tsv{d}; set _sp before _CHK for exception safety.
             * P40.2: use _vrp{idx} (pvalue cached in preamble). */
            jit_buf_printf(cb,
                "    { if(JS_VALUE_GET_TAG(*_vrp%d)==JS_TAG_UNINITIALIZED){\n"
                "        JSValue _r=_RT->get_var_slow(ctx,%uu,%d);\n"
                "        _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d;\n"
                "      } else { _tsv%d=_DUP(*_vrp%d); _sp=%d; } }\n",
                idx, (unsigned)cv_atom, cv_is_lex, d, d, d+1, d, idx, d+1);
            break;
        }
        case OP_put_var:
        case OP_put_var_init: {
            _P94_ENSURE(d-1); /* P9.4: box typed slot before storing as JSValue */
            int idx = (int)bc_u16(&bc[pc+1]);
            JSAtom cv_atom   = js_jit_fb_get_closure_var_atom(b, idx);
            int    cv_is_lex = js_jit_fb_get_closure_var_is_lexical(b, idx);
            int    is_init   = (op == OP_put_var_init) ? 1 : 0;
            /* OP_put_var_init on a lexical slot is the normal initialisation
             * path (let x = expr): write directly even if UNINITIALIZED.
             * For all other cases, check UNINITIALIZED and call the slow path
             * (implicit global → JS_SetPropertyInternal; lexical TDZ → throw).
             * P40.2: use _vrp{idx} (pvalue cached in preamble). */
            if (is_init && cv_is_lex) {
                /* lexical init: always write directly, never needs slow path */
                jit_buf_printf(cb,
                    "    { _FREE(*_vrp%d); *_vrp%d=_tsv%d; _sp=%d; }\n",
                    idx, idx, d-1, d-1);
            } else {
                jit_buf_printf(cb,
                    "    { JSValue _v=_tsv%d; _sp=%d;\n"
                    "      if(js_unlikely(JS_VALUE_GET_TAG(*_vrp%d)==JS_TAG_UNINITIALIZED)){\n"
                    "        if(_RT->put_var_slow(ctx,%uu,%d,%d,_v)<0) goto _ex;\n"
                    "      } else { _FREE(*_vrp%d); *_vrp%d=_v; } }\n",
                    d-1, d-1, idx, (unsigned)cv_atom, cv_is_lex, is_init, idx, idx);
            }
            break;
        }

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
        /* P8.6: float64 fast paths for arithmetic.
         * When gen-time type stack says both operands are numeric (JIT_T_NUMBER),
         * we know they are INT or FLOAT64 and can skip the vtable entirely.
         * When gen-time types are unknown we still emit a runtime float64 branch
         * so object-property arithmetic (e.g. this.x + this.y) avoids the vtable
         * once the IC has seen float64 values. */

        /* P9.2: binary ops pop 2 (_tsv{d-2}, _tsv{d-1}), push 1 into _tsv{d-2}.
         * Result depth = d-1.  Set _sp=d-2 before any vtable call for safety. */
        case OP_add: {
            int _bn = (gen_sp <= d && _GS_TOP2()>=JIT_T_NUMBER && _GS_TOP2()<=JIT_T_INT && _GS_TOP()>=JIT_T_NUMBER && _GS_TOP()<=JIT_T_INT);
            uint8_t _t2=_GS_TOP2(), _t1=_GS_TOP();
            int _t2n=(_t2>=JIT_T_NUMBER&&_t2<=JIT_T_INT), _t1n=(_t1>=JIT_T_NUMBER&&_t1<=JIT_T_INT);
            if (_bn) {
                if (_t2==JIT_T_INT && _t1==JIT_T_INT) {
                    /* P11.6: INT×INT — pure int64 add */
                    jit_buf_printf(cb, "    _ti%d+=_ti%d; _sp=%d;\n", d-2, d-1, d-1);
                } else if (_t2==JIT_T_INT && _t1==JIT_T_NUMBER) {
                    jit_buf_printf(cb, "    _tsd%d=(double)_ti%d+_tsd%d; _sp=%d;\n", d-2, d-2, d-1, d-1);
                } else if (_t2==JIT_T_NUMBER && _t1==JIT_T_INT) {
                    jit_buf_printf(cb, "    _tsd%d+=( double)_ti%d; _sp=%d;\n", d-2, d-1, d-1);
                } else {
                    /* P9.4: NUMBER×NUMBER — pure double add */
                    jit_buf_printf(cb, "    _tsd%d+=_tsd%d; _sp=%d;\n", d-2, d-1, d-1);
                }
            } else if (_t2n) {
                /* P44: left=NUMBER/INT typed, right=JSVAL.
                 * Avoid boxing left; single tag check on right.
                 * Result JSVAL (add can produce strings for non-numeric right). */
                _P94_ENSURE(d-1);
                if (_t2==JIT_T_INT) {
                    jit_buf_printf(cb,
                        "    { JSValue _b=_tsv%d; int _tb=JS_VALUE_GET_TAG(_b);\n"
                        "      if(js_likely(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                        "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                        "        _tsv%d=JS_NewFloat64(ctx,(double)_ti%d+_db); _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _a=JS_NewInt32(ctx,(int32_t)_ti%d); _sp=%d;\n"
                        "        JSValue _r=_RT->add(ctx,_a,_b); _CHK(_r);\n"
                        "        _tsv%d=_r; _sp=%d;\n"
                        "      } }\n",
                        d-1, d-2, d-2, d-1, d-2, d-2, d-2, d-1);
                } else {
                    jit_buf_printf(cb,
                        "    { JSValue _b=_tsv%d; int _tb=JS_VALUE_GET_TAG(_b);\n"
                        "      if(js_likely(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                        "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                        "        _tsv%d=JS_NewFloat64(ctx,_tsd%d+_db); _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _a=JS_NewFloat64(ctx,_tsd%d); _sp=%d;\n"
                        "        JSValue _r=_RT->add(ctx,_a,_b); _CHK(_r);\n"
                        "        _tsv%d=_r; _sp=%d;\n"
                        "      } }\n",
                        d-1, d-2, d-2, d-1, d-2, d-2, d-2, d-1);
                }
            } else if (_t1n) {
                /* P44: right=NUMBER/INT typed, left=JSVAL. */
                _P94_ENSURE(d-2);
                if (_t1==JIT_T_INT) {
                    jit_buf_printf(cb,
                        "    { JSValue _a=_tsv%d; int _ta=JS_VALUE_GET_TAG(_a);\n"
                        "      if(js_likely(_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)){\n"
                        "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                        "        _tsv%d=JS_NewFloat64(ctx,_da+(double)_ti%d); _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _b=JS_NewInt32(ctx,(int32_t)_ti%d); _sp=%d;\n"
                        "        JSValue _r=_RT->add(ctx,_a,_b); _CHK(_r);\n"
                        "        _tsv%d=_r; _sp=%d;\n"
                        "      } }\n",
                        d-2, d-2, d-1, d-1, d-1, d-2, d-2, d-1);
                } else {
                    jit_buf_printf(cb,
                        "    { JSValue _a=_tsv%d; int _ta=JS_VALUE_GET_TAG(_a);\n"
                        "      if(js_likely(_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)){\n"
                        "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                        "        _tsv%d=JS_NewFloat64(ctx,_da+_tsd%d); _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _b=JS_NewFloat64(ctx,_tsd%d); _sp=%d;\n"
                        "        JSValue _r=_RT->add(ctx,_a,_b); _CHK(_r);\n"
                        "        _tsv%d=_r; _sp=%d;\n"
                        "      } }\n",
                        d-2, d-2, d-1, d-1, d-1, d-2, d-2, d-1);
                }
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                /* P51: base index into __jit_vt_ for this OP_add's hint slots. */
                int _ad_base = n_gf + n_ae + n_pf + n_vr + n_pa + ad_idx * 2;
                int _p51_warm = (vt_hints
                                 && _ad_base + 1 < n_gf + n_ae + n_pf + n_vr + n_pa + n_ad * 2
                                 && vt_hints[_ad_base]   == 0 /* JS_TAG_INT */
                                 && vt_hints[_ad_base+1] == 0 /* JS_TAG_INT */);
                if (_p51_warm) {
                    /* Warm speculative INT+INT: both hints = INT.
                     * Skip tag checks; result in _ti (gen_st emits INT for downstream ops). */
                    jit_buf_printf(cb,
                        "    _ti%d=(int64_t)JS_VALUE_GET_INT(_tsv%d)"
                              "+(int64_t)JS_VALUE_GET_INT(_tsv%d); _sp=%d;\n",
                        d-2, d-2, d-1, d-1);
                } else {
                    /* Cold: observe both operand tags, emit full runtime path. */
                    jit_buf_printf(cb,
                        "    { JSValue _b=_tsv%d,_a=_tsv%d;\n"
                        "      int _ta=JS_VALUE_GET_TAG(_a),_tb=JS_VALUE_GET_TAG(_b);\n"
                        "      __jit_vt_%016llx[%d]=(uint8_t)_ta;"
                        " __jit_vt_%016llx[%d]=(uint8_t)_tb;\n"
                        "      if(_ta==JS_TAG_INT&&_tb==JS_TAG_INT){\n"
                        "        int64_t _r64=(int64_t)JS_VALUE_GET_INT(_a)+JS_VALUE_GET_INT(_b);\n"
                        "        _tsv%d=((int32_t)_r64==_r64)?JS_NewInt32(ctx,(int32_t)_r64)\n"
                        "                                    :JS_NewFloat64(ctx,(double)_r64); _sp=%d;\n"
                        "      } else if((_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)&&"
                                 "(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                        "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                        "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                        "        _tsv%d=JS_NewFloat64(ctx,_da+_db); _sp=%d;\n"
                        "      } else {\n"
                        "        _sp=%d; JSValue _r=_RT->add(ctx,_a,_b); _CHK(_r);\n"
                        "        _tsv%d=_r; _sp=%d;\n"
                        "      } }\n",
                        d-1, d-2,
                        (unsigned long long)bc_hash, _ad_base,
                        (unsigned long long)bc_hash, _ad_base + 1,
                        d-2, d-1, d-2, d-1, d-2, d-2, d-1);
                }
            }
            break;
        }

        /* sub: int fast path + float64 fast path */
        case OP_sub: {
            int _bn = (gen_sp <= d && _GS_TOP2()>=JIT_T_NUMBER && _GS_TOP2()<=JIT_T_INT && _GS_TOP()>=JIT_T_NUMBER && _GS_TOP()<=JIT_T_INT);
            uint8_t _t2=_GS_TOP2(), _t1=_GS_TOP();
            int _t2n=(_t2>=JIT_T_NUMBER&&_t2<=JIT_T_INT), _t1n=(_t1>=JIT_T_NUMBER&&_t1<=JIT_T_INT);
            if (_bn) {
                if (_t2==JIT_T_INT && _t1==JIT_T_INT) {
                    jit_buf_printf(cb, "    _ti%d-=_ti%d; _sp=%d;\n", d-2, d-1, d-1);
                } else if (_t2==JIT_T_INT && _t1==JIT_T_NUMBER) {
                    jit_buf_printf(cb, "    _tsd%d=(double)_ti%d-_tsd%d; _sp=%d;\n", d-2, d-2, d-1, d-1);
                } else if (_t2==JIT_T_NUMBER && _t1==JIT_T_INT) {
                    jit_buf_printf(cb, "    _tsd%d-=(double)_ti%d; _sp=%d;\n", d-2, d-1, d-1);
                } else {
                    /* P9.4: NUMBER×NUMBER — pure double sub */
                    jit_buf_printf(cb, "    _tsd%d-=_tsd%d; _sp=%d;\n", d-2, d-1, d-1);
                }
            } else if (_t2n) {
                /* P44: left=INT/NUMBER typed, right=JSVAL.
                 * INT fast path avoids double conversion; result is JSValue (_tsv). */
                _P94_ENSURE(d-1);
                if (_t2==JIT_T_INT) {
                    jit_buf_printf(cb,
                        "    { JSValue _b=_tsv%d; int _tb=JS_VALUE_GET_TAG(_b);\n"
                        "      if(js_likely(_tb==JS_TAG_INT)){\n"
                        "        int64_t _r64=(int64_t)_ti%d-(int64_t)JS_VALUE_GET_INT(_b);\n"
                        "        _tsv%d=((int32_t)_r64==_r64)?JS_NewInt32(ctx,(int32_t)_r64):JS_NewFloat64(ctx,(double)_r64); _sp=%d;\n"
                        "      } else if(_tb==JS_TAG_FLOAT64){\n"
                        "        _tsv%d=JS_NewFloat64(ctx,(double)_ti%d-JS_VALUE_GET_FLOAT64(_b)); _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _a=JS_NewInt32(ctx,(int32_t)_ti%d); _sp=%d;\n"
                        "        JSValue _r=_RT->sub(ctx,_a,_b); _sp=%d; _CHK(_r);\n"
                        "        _tsv%d=_r; _sp=%d;\n"
                        "      } }\n",
                        d-1, d-2, d-2, d-1,
                        d-2, d-2, d-1,
                        d-2, d-2, d-2, d-2, d-1);
                } else {
                    jit_buf_printf(cb,
                        "    { JSValue _b=_tsv%d; int _tb=JS_VALUE_GET_TAG(_b);\n"
                        "      if(js_likely(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                        "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                        "        _tsv%d=JS_NewFloat64(ctx,_tsd%d-_db); _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _a=JS_NewFloat64(ctx,_tsd%d); _sp=%d;\n"
                        "        JSValue _r=_RT->sub(ctx,_a,_b); _sp=%d; _CHK(_r);\n"
                        "        _tsv%d=_r; _sp=%d;\n"
                        "      } }\n",
                        d-1, d-2, d-2, d-1,
                        d-2, d-2, d-2, d-2, d-1);
                }
            } else if (_t1n) {
                /* P44: right=INT/NUMBER typed, left=JSVAL.
                 * INT fast path avoids double conversion; result is JSValue (_tsv). */
                _P94_ENSURE(d-2);
                if (_t1==JIT_T_INT) {
                    jit_buf_printf(cb,
                        "    { JSValue _a=_tsv%d; int _ta=JS_VALUE_GET_TAG(_a);\n"
                        "      if(js_likely(_ta==JS_TAG_INT)){\n"
                        "        int64_t _r64=(int64_t)JS_VALUE_GET_INT(_a)-(int64_t)_ti%d;\n"
                        "        _tsv%d=((int32_t)_r64==_r64)?JS_NewInt32(ctx,(int32_t)_r64):JS_NewFloat64(ctx,(double)_r64); _sp=%d;\n"
                        "      } else if(_ta==JS_TAG_FLOAT64){\n"
                        "        _tsv%d=JS_NewFloat64(ctx,JS_VALUE_GET_FLOAT64(_a)-(double)_ti%d); _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _b=JS_NewInt32(ctx,(int32_t)_ti%d); _sp=%d;\n"
                        "        JSValue _r=_RT->sub(ctx,_a,_b); _sp=%d; _CHK(_r);\n"
                        "        _tsv%d=_r; _sp=%d;\n"
                        "      } }\n",
                        d-2, d-1, d-2, d-1,
                        d-2, d-1, d-1,
                        d-1, d-2, d-2, d-2, d-1);
                } else {
                    jit_buf_printf(cb,
                        "    { JSValue _a=_tsv%d; int _ta=JS_VALUE_GET_TAG(_a);\n"
                        "      if(js_likely(_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)){\n"
                        "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                        "        _tsv%d=JS_NewFloat64(ctx,_da-_tsd%d); _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _b=JS_NewFloat64(ctx,_tsd%d); _sp=%d;\n"
                        "        JSValue _r=_RT->sub(ctx,_a,_b); _sp=%d; _CHK(_r);\n"
                        "        _tsv%d=_r; _sp=%d;\n"
                        "      } }\n",
                        d-2, d-2, d-1, d-1,
                        d-1, d-2, d-2, d-2, d-1);
                }
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                jit_buf_printf(cb,
                    "    { JSValue _b=_tsv%d,_a=_tsv%d;\n"
                    "      int _ta=JS_VALUE_GET_TAG(_a),_tb=JS_VALUE_GET_TAG(_b);\n"
                    "      if(_ta==JS_TAG_INT&&_tb==JS_TAG_INT){\n"
                    "        int64_t _r64=(int64_t)JS_VALUE_GET_INT(_a)-JS_VALUE_GET_INT(_b);\n"
                    "        _tsv%d=((int32_t)_r64==_r64)?JS_NewInt32(ctx,(int32_t)_r64)\n"
                    "                                    :JS_NewFloat64(ctx,(double)_r64); _sp=%d;\n"
                    "      } else if((_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)&&"
                             "(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                    "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                    "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                    "        _tsv%d=JS_NewFloat64(ctx,_da-_db); _sp=%d;\n"
                    "      } else {\n"
                    "        _sp=%d; JSValue _r=_RT->sub(ctx,_a,_b); _CHK(_r);\n"
                    "        _tsv%d=_r; _sp=%d;\n"
                    "      } }\n",
                    d-1, d-2, d-2, d-1, d-2, d-1, d-2, d-2, d-1);
            }
            break;
        }

        /* mul: int fast path + float64 fast path */
        case OP_mul: {
            int _bn = (gen_sp <= d && _GS_TOP2()>=JIT_T_NUMBER && _GS_TOP2()<=JIT_T_INT && _GS_TOP()>=JIT_T_NUMBER && _GS_TOP()<=JIT_T_INT);
            uint8_t _t2=_GS_TOP2(), _t1=_GS_TOP();
            int _t2n=(_t2>=JIT_T_NUMBER&&_t2<=JIT_T_INT), _t1n=(_t1>=JIT_T_NUMBER&&_t1<=JIT_T_INT);
            if (_bn) {
                if (_t2==JIT_T_INT && _t1==JIT_T_INT) {
                    jit_buf_printf(cb, "    _ti%d*=_ti%d; _sp=%d;\n", d-2, d-1, d-1);
                } else if (_t2==JIT_T_INT && _t1==JIT_T_NUMBER) {
                    jit_buf_printf(cb, "    _tsd%d=(double)_ti%d*_tsd%d; _sp=%d;\n", d-2, d-2, d-1, d-1);
                } else if (_t2==JIT_T_NUMBER && _t1==JIT_T_INT) {
                    jit_buf_printf(cb, "    _tsd%d*=(double)_ti%d; _sp=%d;\n", d-2, d-1, d-1);
                } else {
                    /* P9.4: NUMBER×NUMBER — pure double mul */
                    jit_buf_printf(cb, "    _tsd%d*=_tsd%d; _sp=%d;\n", d-2, d-1, d-1);
                }
            } else if (_t2n) {
                /* P44: left=INT/NUMBER typed, right=JSVAL.
                 * INT fast path avoids double conversion; result is JSValue (_tsv). */
                _P94_ENSURE(d-1);
                if (_t2==JIT_T_INT) {
                    jit_buf_printf(cb,
                        "    { JSValue _b=_tsv%d; int _tb=JS_VALUE_GET_TAG(_b);\n"
                        "      if(js_likely(_tb==JS_TAG_INT)){\n"
                        "        int64_t _ia=(int64_t)_ti%d,_ib=(int64_t)JS_VALUE_GET_INT(_b);\n"
                        "        int64_t _r64=_ia*_ib;\n"
                        "        if((int32_t)_r64==_r64&&!(_r64==0&&((_ia^_ib)>>63)))\n"
                        "          _tsv%d=JS_NewInt32(ctx,(int32_t)_r64);\n"
                        "        else _tsv%d=JS_NewFloat64(ctx,(double)_ia*(double)_ib);\n"
                        "        _sp=%d;\n"
                        "      } else if(_tb==JS_TAG_FLOAT64){\n"
                        "        _tsv%d=JS_NewFloat64(ctx,(double)_ti%d*JS_VALUE_GET_FLOAT64(_b)); _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _a=JS_NewInt32(ctx,(int32_t)_ti%d); _sp=%d;\n"
                        "        JSValue _r=_RT->mul(ctx,_a,_b); _sp=%d; _CHK(_r);\n"
                        "        _tsv%d=_r; _sp=%d;\n"
                        "      } }\n",
                        d-1, d-2,
                        d-2, d-2, d-1,
                        d-2, d-2, d-1,
                        d-2, d-2, d-2, d-2, d-1);
                } else {
                    jit_buf_printf(cb,
                        "    { JSValue _b=_tsv%d; int _tb=JS_VALUE_GET_TAG(_b);\n"
                        "      if(js_likely(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                        "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                        "        _tsv%d=JS_NewFloat64(ctx,_tsd%d*_db); _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _a=JS_NewFloat64(ctx,_tsd%d); _sp=%d;\n"
                        "        JSValue _r=_RT->mul(ctx,_a,_b); _sp=%d; _CHK(_r);\n"
                        "        _tsv%d=_r; _sp=%d;\n"
                        "      } }\n",
                        d-1, d-2, d-2, d-1,
                        d-2, d-2, d-2, d-2, d-1);
                }
            } else if (_t1n) {
                /* P44: right=INT/NUMBER typed, left=JSVAL (mul is commutative). */
                _P94_ENSURE(d-2);
                if (_t1==JIT_T_INT) {
                    jit_buf_printf(cb,
                        "    { JSValue _a=_tsv%d; int _ta=JS_VALUE_GET_TAG(_a);\n"
                        "      if(js_likely(_ta==JS_TAG_INT)){\n"
                        "        int64_t _ia=(int64_t)JS_VALUE_GET_INT(_a),_ib=(int64_t)_ti%d;\n"
                        "        int64_t _r64=_ia*_ib;\n"
                        "        if((int32_t)_r64==_r64&&!(_r64==0&&((_ia^_ib)>>63)))\n"
                        "          _tsv%d=JS_NewInt32(ctx,(int32_t)_r64);\n"
                        "        else _tsv%d=JS_NewFloat64(ctx,(double)_ia*(double)_ib);\n"
                        "        _sp=%d;\n"
                        "      } else if(_ta==JS_TAG_FLOAT64){\n"
                        "        _tsv%d=JS_NewFloat64(ctx,JS_VALUE_GET_FLOAT64(_a)*(double)_ti%d); _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _b=JS_NewInt32(ctx,(int32_t)_ti%d); _sp=%d;\n"
                        "        JSValue _r=_RT->mul(ctx,_a,_b); _sp=%d; _CHK(_r);\n"
                        "        _tsv%d=_r; _sp=%d;\n"
                        "      } }\n",
                        d-2, d-1,
                        d-2, d-2, d-1,
                        d-2, d-1, d-1,
                        d-1, d-2, d-2, d-2, d-1);
                } else {
                    jit_buf_printf(cb,
                        "    { JSValue _a=_tsv%d; int _ta=JS_VALUE_GET_TAG(_a);\n"
                        "      if(js_likely(_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)){\n"
                        "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                        "        _tsv%d=JS_NewFloat64(ctx,_da*_tsd%d); _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _b=JS_NewFloat64(ctx,_tsd%d); _sp=%d;\n"
                        "        JSValue _r=_RT->mul(ctx,_a,_b); _sp=%d; _CHK(_r);\n"
                        "        _tsv%d=_r; _sp=%d;\n"
                        "      } }\n",
                        d-2, d-2, d-1, d-1,
                        d-1, d-2, d-2, d-2, d-1);
                }
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                jit_buf_printf(cb,
                    "    { JSValue _b=_tsv%d,_a=_tsv%d;\n"
                    "      int _ta=JS_VALUE_GET_TAG(_a),_tb=JS_VALUE_GET_TAG(_b);\n"
                    "      if(_ta==JS_TAG_INT&&_tb==JS_TAG_INT){\n"
                    "        int64_t _r64=(int64_t)JS_VALUE_GET_INT(_a)*JS_VALUE_GET_INT(_b);\n"
                    "        if((int32_t)_r64==_r64 && !(_r64==0 && ((JS_VALUE_GET_INT(_a)^JS_VALUE_GET_INT(_b))>>31)))\n"
                    "          _tsv%d=JS_NewInt32(ctx,(int32_t)_r64);\n"
                    "        else\n"
                    "          _tsv%d=JS_NewFloat64(ctx,(double)JS_VALUE_GET_INT(_a)*(double)JS_VALUE_GET_INT(_b));\n"
                    "        _sp=%d;\n"
                    "      } else if((_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)&&"
                             "(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                    "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                    "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                    "        _tsv%d=JS_NewFloat64(ctx,_da*_db); _sp=%d;\n"
                    "      } else {\n"
                    "        _sp=%d; JSValue _r=_RT->mul(ctx,_a,_b); _CHK(_r);\n"
                    "        _tsv%d=_r; _sp=%d;\n"
                    "      } }\n",
                    d-1, d-2, d-2, d-2, d-1, d-2, d-1, d-2, d-2, d-1);
            }
            break;
        }

        /* div: float result; int/int fast path + float64 fast path */
        case OP_div: {
            int _bn = (gen_sp <= d && _GS_TOP2()>=JIT_T_NUMBER && _GS_TOP2()<=JIT_T_INT && _GS_TOP()>=JIT_T_NUMBER && _GS_TOP()<=JIT_T_INT);
            uint8_t _t2=_GS_TOP2(), _t1=_GS_TOP();
            int _t2n=(_t2>=JIT_T_NUMBER&&_t2<=JIT_T_INT), _t1n=(_t1>=JIT_T_NUMBER&&_t1<=JIT_T_INT);
            if (_bn) {
                /* P11.6: div result is NUMBER (may not be integer) — always use _tsd.
                 * Convert _ti inputs to double as needed. */
                const char *_a2 = (_t2==JIT_T_INT) ? "(double)_ti" : "_tsd";
                const char *_a1 = (_t1==JIT_T_INT) ? "(double)_ti" : "_tsd";
                jit_buf_printf(cb,
                    "    _tsd%d=%s%d/%s%d; _sp=%d;\n",
                    d-2, _a2, d-2, _a1, d-1, d-1);
            } else if (_t2n) {
                /* P44: left=NUMBER/INT typed, right=JSVAL.
                 * div always numeric → result in _tsd (NUMBER). */
                _P94_ENSURE(d-1);
                if (_t2==JIT_T_INT) {
                    jit_buf_printf(cb,
                        "    { JSValue _b=_tsv%d; int _tb=JS_VALUE_GET_TAG(_b);\n"
                        "      if(js_likely(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                        "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                        "        _tsd%d=(double)_ti%d/_db; _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _a=JS_NewInt32(ctx,(int32_t)_ti%d); _sp=%d;\n"
                        "        JSValue _r=_RT->div(ctx,_a,_b); _sp=%d; _CHK(_r);\n"
                        "        _tsd%d=JS_VALUE_GET_TAG(_r)==JS_TAG_INT?(double)JS_VALUE_GET_INT(_r):JS_VALUE_GET_FLOAT64(_r);\n"
                        "        _sp=%d;\n"
                        "      } }\n",
                        d-1, d-2, d-2, d-1, d-2, d-2, d-2, d-2, d-1);
                } else {
                    jit_buf_printf(cb,
                        "    { JSValue _b=_tsv%d; int _tb=JS_VALUE_GET_TAG(_b);\n"
                        "      if(js_likely(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                        "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                        "        _tsd%d/=_db; _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _a=JS_NewFloat64(ctx,_tsd%d); _sp=%d;\n"
                        "        JSValue _r=_RT->div(ctx,_a,_b); _sp=%d; _CHK(_r);\n"
                        "        _tsd%d=JS_VALUE_GET_TAG(_r)==JS_TAG_INT?(double)JS_VALUE_GET_INT(_r):JS_VALUE_GET_FLOAT64(_r);\n"
                        "        _sp=%d;\n"
                        "      } }\n",
                        d-1, d-2, d-1, d-2, d-2, d-2, d-2, d-1);
                }
            } else if (_t1n) {
                /* P44: right=NUMBER/INT typed, left=JSVAL. */
                _P94_ENSURE(d-2);
                if (_t1==JIT_T_INT) {
                    jit_buf_printf(cb,
                        "    { JSValue _a=_tsv%d; int _ta=JS_VALUE_GET_TAG(_a);\n"
                        "      if(js_likely(_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)){\n"
                        "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                        "        _tsd%d=_da/(double)_ti%d; _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _b=JS_NewInt32(ctx,(int32_t)_ti%d); _sp=%d;\n"
                        "        JSValue _r=_RT->div(ctx,_a,_b); _sp=%d; _CHK(_r);\n"
                        "        _tsd%d=JS_VALUE_GET_TAG(_r)==JS_TAG_INT?(double)JS_VALUE_GET_INT(_r):JS_VALUE_GET_FLOAT64(_r);\n"
                        "        _sp=%d;\n"
                        "      } }\n",
                        d-2, d-2, d-1, d-1, d-1, d-2, d-2, d-2, d-1);
                } else {
                    jit_buf_printf(cb,
                        "    { JSValue _a=_tsv%d; int _ta=JS_VALUE_GET_TAG(_a);\n"
                        "      if(js_likely(_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)){\n"
                        "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                        "        _tsd%d=_da/_tsd%d; _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _b=JS_NewFloat64(ctx,_tsd%d); _sp=%d;\n"
                        "        JSValue _r=_RT->div(ctx,_a,_b); _sp=%d; _CHK(_r);\n"
                        "        _tsd%d=JS_VALUE_GET_TAG(_r)==JS_TAG_INT?(double)JS_VALUE_GET_INT(_r):JS_VALUE_GET_FLOAT64(_r);\n"
                        "        _sp=%d;\n"
                        "      } }\n",
                        d-2, d-2, d-1, d-1, d-1, d-2, d-2, d-2, d-1);
                }
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                jit_buf_printf(cb,
                    "    { JSValue _b=_tsv%d,_a=_tsv%d;\n"
                    "      int _ta=JS_VALUE_GET_TAG(_a),_tb=JS_VALUE_GET_TAG(_b);\n"
                    "      if(_ta==JS_TAG_INT&&_tb==JS_TAG_INT){\n"
                    "        int32_t ia=JS_VALUE_GET_INT(_a),ib=JS_VALUE_GET_INT(_b);\n"
                    "        _tsv%d=(ib&&ia%%ib==0)?JS_NewInt32(ctx,ia/ib)\n"
                    "                              :JS_NewFloat64(ctx,(double)ia/(double)ib); _sp=%d;\n"
                    "      } else if((_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)&&"
                             "(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                    "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                    "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                    "        _tsv%d=JS_NewFloat64(ctx,_da/_db); _sp=%d;\n"
                    "      } else {\n"
                    "        _sp=%d; JSValue _r=_RT->div(ctx,_a,_b); _CHK(_r);\n"
                    "        _tsv%d=_r; _sp=%d;\n"
                    "      } }\n",
                    d-1, d-2, d-2, d-1, d-2, d-1, d-2, d-2, d-1);
            }
            break;
        }

        /* mod: int fast path + float64 fast path */
        case OP_mod: {
            int _bn = (gen_sp <= d && _GS_TOP2()>=JIT_T_NUMBER && _GS_TOP2()<=JIT_T_INT && _GS_TOP()>=JIT_T_NUMBER && _GS_TOP()<=JIT_T_INT);
            uint8_t _t2=_GS_TOP2(), _t1=_GS_TOP();
            int _t2n=(_t2>=JIT_T_NUMBER&&_t2<=JIT_T_INT), _t1n=(_t1>=JIT_T_NUMBER&&_t1<=JIT_T_INT);
            if (_bn) {
                if (_t2==JIT_T_INT && _t1==JIT_T_INT) {
                    /* P11.6: INT×INT mod — integer remainder (div by zero → keep NaN-like behavior via slow path) */
                    jit_buf_printf(cb, "    if(_ti%d) _ti%d%%=_ti%d; _sp=%d;\n", d-1, d-2, d-1, d-1);
                } else {
                    /* P9.4: mixed or NUMBER×NUMBER — fmod on _tsd (convert if needed) */
                    const char *_a2 = (_t2==JIT_T_INT) ? "(double)_ti" : "_tsd";
                    const char *_a1 = (_t1==JIT_T_INT) ? "(double)_ti" : "_tsd";
                    jit_buf_printf(cb,
                        "    _tsd%d=fmod(%s%d,%s%d); _sp=%d;\n",
                        d-2, _a2, d-2, _a1, d-1, d-1);
                }
            } else if (_t2n) {
                /* P44: left=NUMBER/INT typed, right=JSVAL.
                 * mod always numeric → result in _tsd (NUMBER). */
                _P94_ENSURE(d-1);
                if (_t2==JIT_T_INT) {
                    jit_buf_printf(cb,
                        "    { JSValue _b=_tsv%d; int _tb=JS_VALUE_GET_TAG(_b);\n"
                        "      if(js_likely(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                        "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                        "        _tsd%d=fmod((double)_ti%d,_db); _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _a=JS_NewInt32(ctx,(int32_t)_ti%d); _sp=%d;\n"
                        "        JSValue _r=_RT->mod(ctx,_a,_b); _sp=%d; _CHK(_r);\n"
                        "        _tsd%d=JS_VALUE_GET_TAG(_r)==JS_TAG_INT?(double)JS_VALUE_GET_INT(_r):JS_VALUE_GET_FLOAT64(_r);\n"
                        "        _sp=%d;\n"
                        "      } }\n",
                        d-1, d-2, d-2, d-1, d-2, d-2, d-2, d-2, d-1);
                } else {
                    jit_buf_printf(cb,
                        "    { JSValue _b=_tsv%d; int _tb=JS_VALUE_GET_TAG(_b);\n"
                        "      if(js_likely(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                        "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                        "        _tsd%d=fmod(_tsd%d,_db); _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _a=JS_NewFloat64(ctx,_tsd%d); _sp=%d;\n"
                        "        JSValue _r=_RT->mod(ctx,_a,_b); _sp=%d; _CHK(_r);\n"
                        "        _tsd%d=JS_VALUE_GET_TAG(_r)==JS_TAG_INT?(double)JS_VALUE_GET_INT(_r):JS_VALUE_GET_FLOAT64(_r);\n"
                        "        _sp=%d;\n"
                        "      } }\n",
                        d-1, d-2, d-2, d-1, d-2, d-2, d-2, d-2, d-1);
                }
            } else if (_t1n) {
                /* P44: right=NUMBER/INT typed, left=JSVAL. */
                _P94_ENSURE(d-2);
                if (_t1==JIT_T_INT) {
                    jit_buf_printf(cb,
                        "    { JSValue _a=_tsv%d; int _ta=JS_VALUE_GET_TAG(_a);\n"
                        "      if(js_likely(_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)){\n"
                        "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                        "        _tsd%d=fmod(_da,(double)_ti%d); _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _b=JS_NewInt32(ctx,(int32_t)_ti%d); _sp=%d;\n"
                        "        JSValue _r=_RT->mod(ctx,_a,_b); _sp=%d; _CHK(_r);\n"
                        "        _tsd%d=JS_VALUE_GET_TAG(_r)==JS_TAG_INT?(double)JS_VALUE_GET_INT(_r):JS_VALUE_GET_FLOAT64(_r);\n"
                        "        _sp=%d;\n"
                        "      } }\n",
                        d-2, d-2, d-1, d-1, d-1, d-2, d-2, d-2, d-1);
                } else {
                    jit_buf_printf(cb,
                        "    { JSValue _a=_tsv%d; int _ta=JS_VALUE_GET_TAG(_a);\n"
                        "      if(js_likely(_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)){\n"
                        "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                        "        _tsd%d=fmod(_da,_tsd%d); _sp=%d;\n"
                        "      } else {\n"
                        "        JSValue _b=JS_NewFloat64(ctx,_tsd%d); _sp=%d;\n"
                        "        JSValue _r=_RT->mod(ctx,_a,_b); _sp=%d; _CHK(_r);\n"
                        "        _tsd%d=JS_VALUE_GET_TAG(_r)==JS_TAG_INT?(double)JS_VALUE_GET_INT(_r):JS_VALUE_GET_FLOAT64(_r);\n"
                        "        _sp=%d;\n"
                        "      } }\n",
                        d-2, d-2, d-1, d-1, d-1, d-2, d-2, d-2, d-1);
                }
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                jit_buf_printf(cb,
                    "    { JSValue _b=_tsv%d,_a=_tsv%d;\n"
                    "      int _ta=JS_VALUE_GET_TAG(_a),_tb=JS_VALUE_GET_TAG(_b);\n"
                    "      if(_ta==JS_TAG_INT&&_tb==JS_TAG_INT){\n"
                    "        int32_t ib=JS_VALUE_GET_INT(_b);\n"
                    "        _tsv%d=ib?JS_NewInt32(ctx,JS_VALUE_GET_INT(_a)%%ib)\n"
                    "                 :JS_NewFloat64(ctx,0.0/0.0); _sp=%d;\n"
                    "      } else if((_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)&&"
                             "(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                    "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                    "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                    "        _tsv%d=JS_NewFloat64(ctx,fmod(_da,_db)); _sp=%d;\n"
                    "      } else {\n"
                    "        _sp=%d; JSValue _r=_RT->mod(ctx,_a,_b); _CHK(_r);\n"
                    "        _tsv%d=_r; _sp=%d;\n"
                    "      } }\n",
                    d-1, d-2, d-2, d-1, d-2, d-1, d-2, d-2, d-1);
            }
            break;
        }

        /* Bitwise: ToInt32 already guaranteed by semantics; fast path for int */
        /* P9.2: pop 2 -> push 1: result at _tsv{d-2}, new depth d-1 */
#define GEN_BITOP_INT(op_str, rt_name) \
    jit_buf_printf(cb, \
        "    { JSValue _b=_tsv%d,_a=_tsv%d;\n" \
        "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n" \
        "        _tsv%d=JS_NewInt32(ctx,JS_VALUE_GET_INT(_a) %s JS_VALUE_GET_INT(_b));\n" \
        "      else { _sp=%d; JSValue _r=_RT->%s(ctx,_a,_b); _CHK(_r); _tsv%d=_r; }\n" \
        "      _sp=%d; }\n", \
        d-1, d-2, d-2, op_str, d-2, rt_name, d-2, d-1)

        /* Shift operators must mask the shift count to & 31, matching the
         * JavaScript spec (ToInt32 semantics) and avoiding C UB for shifts
         * by >= 32 (e.g. 1 << 32 must equal 1, not 0). */
        case OP_shl: {
            uint8_t _t2 = _GS_TOP2(), _t1 = _GS_TOP();
            if (_t2 == JIT_T_INT && _t1 == JIT_T_INT) {
                /* P43.4: both INT — emit native shift, result stays in _ti */
                jit_buf_printf(cb,
                    "    _ti%d=(int64_t)(int32_t)((uint32_t)_ti%d<<(_ti%d&31)); _sp=%d;\n",
                    d-2, d-2, d-1, d-1);
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                jit_buf_printf(cb,
                    "    { JSValue _b=_tsv%d,_a=_tsv%d;\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n"
                    "        _tsv%d=JS_NewInt32(ctx,(int32_t)((uint32_t)JS_VALUE_GET_INT(_a)<<(JS_VALUE_GET_INT(_b)&31)));\n"
                    "      else { _sp=%d; JSValue _r=_RT->shl(ctx,_a,_b); _CHK(_r); _tsv%d=_r; }\n"
                    "      _sp=%d; }\n",
                    d-1, d-2, d-2, d-2, d-2, d-1);
            }
            break;
        }
        case OP_sar: {
            uint8_t _t2 = _GS_TOP2(), _t1 = _GS_TOP();
            if (_t2 == JIT_T_INT && _t1 == JIT_T_INT) {
                /* P43.4: both INT — emit native arithmetic shift, result stays in _ti */
                jit_buf_printf(cb,
                    "    _ti%d=(int64_t)(int32_t)(_ti%d>>(_ti%d&31)); _sp=%d;\n",
                    d-2, d-2, d-1, d-1);
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                jit_buf_printf(cb,
                    "    { JSValue _b=_tsv%d,_a=_tsv%d;\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n"
                    "        _tsv%d=JS_NewInt32(ctx,JS_VALUE_GET_INT(_a)>>(JS_VALUE_GET_INT(_b)&31));\n"
                    "      else { _sp=%d; JSValue _r=_RT->sar(ctx,_a,_b); _CHK(_r); _tsv%d=_r; }\n"
                    "      _sp=%d; }\n",
                    d-1, d-2, d-2, d-2, d-2, d-1);
            }
            break;
        }
        case OP_and: {
            uint8_t _t2 = _GS_TOP2(), _t1 = _GS_TOP();
            if (_t2 == JIT_T_INT && _t1 == JIT_T_INT) {
                jit_buf_printf(cb,
                    "    _ti%d=(int64_t)(int32_t)(_ti%d&_ti%d); _sp=%d;\n",
                    d-2, d-2, d-1, d-1);
            } else { _P94_ENSURE(d-2); _P94_ENSURE(d-1); GEN_BITOP_INT("&",  "band"); }
            break;
        }
        case OP_or: {
            uint8_t _t2 = _GS_TOP2(), _t1 = _GS_TOP();
            if (_t2 == JIT_T_INT && _t1 == JIT_T_INT) {
                jit_buf_printf(cb,
                    "    _ti%d=(int64_t)(int32_t)(_ti%d|_ti%d); _sp=%d;\n",
                    d-2, d-2, d-1, d-1);
            } else { _P94_ENSURE(d-2); _P94_ENSURE(d-1); GEN_BITOP_INT("|",  "bor"); }
            break;
        }
        case OP_xor: {
            uint8_t _t2 = _GS_TOP2(), _t1 = _GS_TOP();
            if (_t2 == JIT_T_INT && _t1 == JIT_T_INT) {
                jit_buf_printf(cb,
                    "    _ti%d=(int64_t)(int32_t)(_ti%d^_ti%d); _sp=%d;\n",
                    d-2, d-2, d-1, d-1);
            } else { _P94_ENSURE(d-2); _P94_ENSURE(d-1); GEN_BITOP_INT("^",  "bxor"); }
            break;
        }

        /* shr is unsigned right shift — result may exceed INT32_MAX */
        case OP_shr:
            _P94_ENSURE(d-2); _P94_ENSURE(d-1); /* P9.4: box typed slots */
            jit_buf_printf(cb,
                "    { JSValue _b=_tsv%d,_a=_tsv%d;\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT){\n"
                "        uint32_t _r=(uint32_t)JS_VALUE_GET_INT(_a)>>(JS_VALUE_GET_INT(_b)&31);\n"
                "        _tsv%d=(_r<=(uint32_t)INT32_MAX)?JS_NewInt32(ctx,(int32_t)_r)\n"
                "                                        :JS_NewFloat64(ctx,(double)_r);\n"
                "      } else { _sp=%d; JSValue _r=_RT->shr(ctx,_a,_b); _CHK(_r); _tsv%d=_r; }\n"
                "      _sp=%d; }\n",
                d-1, d-2, d-2, d-2, d-2, d-1);
            break;

#undef GEN_BITOP_INT

        /* pow and remaining ops: full vtable (rare).
         * P9.2: pop 2 -> push 1: result at _tsv{d-2}, new depth d-1 */
        case OP_pow:
            _P94_ENSURE(d-2); _P94_ENSURE(d-1); /* P9.4: box typed slots */
            jit_buf_printf(cb,
                "    { JSValue _b=_tsv%d,_a=_tsv%d;\n"
                "      _sp=%d; JSValue _r=_RT->pow(ctx,_a,_b); _CHK(_r);\n"
                "      _tsv%d=_r; _sp=%d; }\n",
                d-1, d-2, d-2, d-2, d-1);
            break;

        /* ---- Arithmetic (unary) with int fast paths ----
         * P9.2: unary ops: pop 1 (_tsv{d-1}), push 1 back at _tsv{d-1}, depth unchanged */
        case OP_neg:
            /* P11.6: INT uses _ti; NUMBER uses _tsd */
            if (gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_INT) {
                jit_buf_printf(cb, "    _ti%d=-_ti%d; _sp=%d;\n", d-1, d-1, d);
            } else if (gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_NUMBER) {
                jit_buf_printf(cb, "    _tsd%d=-_tsd%d; _sp=%d;\n", d-1, d-1, d);
            } else {
                jit_buf_printf(cb,
                    "    { JSValue _a=_tsv%d; int _ta=JS_VALUE_GET_TAG(_a);\n"
                    "      if(_ta==JS_TAG_INT){\n"
                    "        int32_t ia=JS_VALUE_GET_INT(_a);\n"
                    "        _tsv%d=(ia==INT32_MIN)?JS_NewFloat64(ctx,-(double)ia)\n"
                    "                             :JS_NewInt32(ctx,-ia);\n"
                    "      } else if(_ta==JS_TAG_FLOAT64)\n"
                    "        _tsv%d=JS_NewFloat64(ctx,-JS_VALUE_GET_FLOAT64(_a));\n"
                    "      else { _sp=%d; JSValue _r=_RT->neg(ctx,_a); _CHK(_r); _tsv%d=_r; } }\n"
                    "    _sp=%d;\n",
                    d-1, d-1, d-1, d-1, d-1, d);
            }
            break;
        case OP_plus:
            /* P9.4: typed fast path — already a double, no-op besides _sp update */
            if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) {
                jit_buf_printf(cb, "    /* plus: typed no-op */ _sp=%d;\n", d);
            } else {
                jit_buf_printf(cb,
                    "    { JSValue _a=_tsv%d; int _ta=JS_VALUE_GET_TAG(_a);\n"
                    "      if(_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)\n"
                    "        _tsv%d=_a; /* numeric — already a number, no coercion needed */\n"
                    "      else { _sp=%d; JSValue _r=_RT->plus(ctx,_a); _CHK(_r); _tsv%d=_r; } }\n"
                    "    _sp=%d;\n",
                    d-1, d-1, d-1, d-1, d);
            }
            break;
        case OP_not: { /* bitwise ~ */
            uint8_t _t1 = _GS_TOP();
            if (_t1 == JIT_T_INT) {
                /* P43.4: INT input — emit native bitwise NOT, result stays in _ti */
                jit_buf_printf(cb,
                    "    _ti%d=(int64_t)(int32_t)(~(int32_t)_ti%d); _sp=%d;\n",
                    d-1, d-1, d);
            } else {
                _P94_ENSURE(d-1);
                jit_buf_printf(cb,
                    "    { JSValue _a=_tsv%d;\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT)\n"
                    "        _tsv%d=JS_NewInt32(ctx,~JS_VALUE_GET_INT(_a));\n"
                    "      else { _sp=%d; JSValue _r=_RT->bnot(ctx,_a); _CHK(_r); _tsv%d=_r; }\n"
                    "      _sp=%d; }\n",
                    d-1, d-1, d-1, d-1, d);
            }
            break;
        }
        case OP_typeof:
            _P94_ENSURE(d-1); /* P9.4: box typed slot before JSValue read */
            jit_buf_printf(cb,
                "    { JSValue _a=_tsv%d;\n"
                "      _sp=%d; JSValue _r=_RT->type_of(ctx,_a); _CHK(_r);\n"
                "      _tsv%d=_r; _sp=%d; }\n",
                d-1, d-1, d-1, d);
            break;
        case OP_lnot:
            /* P9.4/P9.2: lnot: box typed slot, pop 1, push 1 at same slot; depth unchanged */
            _P94_ENSURE(d-1); /* P9.4: box typed slot before JSValue read */
            jit_buf_printf(cb,
                "    { JSValue _a=_tsv%d;\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT)\n"
                "        _tsv%d=JS_NewBool(ctx,JS_VALUE_GET_INT(_a)==0);\n"
                "      else if(JS_VALUE_GET_TAG(_a)==JS_TAG_BOOL)\n"
                "        _tsv%d=JS_NewBool(ctx,!JS_VALUE_GET_INT(_a));\n"
                "      else { _tsv%d=JS_NewBool(ctx,!JS_ToBool(ctx,_a)); _FREE(_a); }\n"
                "      _sp=%d; }\n",
                d-1, d-1, d-1, d-1, d);
            break;

        /* ---- P18: type-test opcodes ---- */
        /* is_null / is_undefined / is_undefined_or_null:
         * Check JS_TAG_* directly. _FREE is always safe — NULL/UNDEFINED are
         * immediates so JS_FreeValue is a no-op for them. */
        case OP_is_null:
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { int _is=(JS_VALUE_GET_TAG(_tsv%d)==JS_TAG_NULL);"
                " _FREE(_tsv%d); _tsv%d=JS_NewBool(ctx,_is); _sp=%d; }\n",
                d-1, d-1, d-1, d);
            break;
        case OP_is_undefined:
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { int _is=(JS_VALUE_GET_TAG(_tsv%d)==JS_TAG_UNDEFINED);"
                " _FREE(_tsv%d); _tsv%d=JS_NewBool(ctx,_is); _sp=%d; }\n",
                d-1, d-1, d-1, d);
            break;
        case OP_is_undefined_or_null:
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { int _tag=JS_VALUE_GET_TAG(_tsv%d);"
                " int _is=(_tag==JS_TAG_NULL||_tag==JS_TAG_UNDEFINED);"
                " _FREE(_tsv%d); _tsv%d=JS_NewBool(ctx,_is); _sp=%d; }\n",
                d-1, d-1, d-1, d);
            break;
        /* typeof_is_undefined / typeof_is_function:
         * Use runtime helpers that call js_operator_typeof() (handles HTMLDDA).
         * The helpers consume (free) the value and return int. */
        case OP_typeof_is_undefined:
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { int _is=_RT->typeof_is_undefined(ctx,_tsv%d);"
                " _tsv%d=JS_NewBool(ctx,_is); _sp=%d; }\n",
                d-1, d-1, d);
            break;
        case OP_typeof_is_function:
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { int _is=_RT->typeof_is_function(ctx,_tsv%d);"
                " _tsv%d=JS_NewBool(ctx,_is); _sp=%d; }\n",
                d-1, d-1, d);
            break;

        /* ---- Increment / decrement ---- */
        /* P11.6/P9.4/P9.2: inc/dec: pop 1, push 1 at same slot; depth unchanged */
        case OP_inc:
            if (gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_INT) {
                /* P11.6: INT fast path — increment int64 */
                jit_buf_printf(cb, "    _ti%d++; _sp=%d;\n", d-1, d);
            } else if (gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_NUMBER) {
                /* P9.4: NUMBER fast path — increment double */
                jit_buf_printf(cb, "    _tsd%d+=1.0; _sp=%d;\n", d-1, d);
            } else {
                jit_buf_printf(cb,
                    "    { JSValue _a=_tsv%d;\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT){\n"
                    "        int32_t ia=JS_VALUE_GET_INT(_a);\n"
                    "        _tsv%d=(ia==INT32_MAX)?JS_NewFloat64(ctx,(double)ia+1)\n"
                    "                             :JS_NewInt32(ctx,ia+1);\n"
                    "      } else { _sp=%d; JSValue _r=_RT->add(ctx,_a,JS_NewInt32(ctx,1));\n"
                    "               _CHK(_r); _tsv%d=_r; }\n"
                    "      _sp=%d; }\n",
                    d-1, d-1, d-1, d-1, d);
            }
            break;
        case OP_dec:
            if (gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_INT) {
                /* P11.6: INT fast path — decrement int64 */
                jit_buf_printf(cb, "    _ti%d--; _sp=%d;\n", d-1, d);
            } else if (gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_NUMBER) {
                /* P9.4: NUMBER fast path — decrement double */
                jit_buf_printf(cb, "    _tsd%d-=1.0; _sp=%d;\n", d-1, d);
            } else {
                jit_buf_printf(cb,
                    "    { JSValue _a=_tsv%d;\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT){\n"
                    "        int32_t ia=JS_VALUE_GET_INT(_a);\n"
                    "        _tsv%d=(ia==INT32_MIN)?JS_NewFloat64(ctx,(double)ia-1)\n"
                    "                             :JS_NewInt32(ctx,ia-1);\n"
                    "      } else { _sp=%d; JSValue _r=_RT->sub(ctx,_a,JS_NewInt32(ctx,1));\n"
                    "               _CHK(_r); _tsv%d=_r; }\n"
                    "      _sp=%d; }\n",
                    d-1, d-1, d-1, d-1, d);
            }
            break;
        /* OP_post_inc / OP_post_dec: pop 1, push 2: original at slot{d-1}, result at slot{d}; depth d -> d+1 */
        case OP_post_inc:
            if (gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_INT) {
                /* P11.6: INT fast path — keep _ti slots valid */
                jit_buf_printf(cb,
                    "    { int64_t _ia=_ti%d; _ti%d=_ia; _ti%d=_ia+1LL; _sp=%d; }\n",
                    d-1, d-1, d, d+1);
            } else if (gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_NUMBER) {
                /* P9.4: NUMBER fast path — keep _tsd slots valid, no boxing */
                jit_buf_printf(cb,
                    "    { double _da=_tsd%d; _tsd%d=_da; _tsd%d=_da+1.0; _sp=%d; }\n",
                    d-1, d-1, d, d+1);
            } else {
                jit_buf_printf(cb,
                    "    { JSValue _a=_tsv%d;\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT){\n"
                    "        int32_t ia=JS_VALUE_GET_INT(_a);\n"
                    "        _tsv%d=_a; /* original stays */\n"
                    "        _tsv%d=(ia==INT32_MAX)?JS_NewFloat64(ctx,(double)ia+1)\n"
                    "                             :JS_NewInt32(ctx,ia+1);\n"
                    "        _sp=%d;\n"
                    "      } else { _sp=%d; JSValue _r=_RT->add(ctx,_DUP(_a),JS_NewInt32(ctx,1));\n"
                    "               _CHK(_r); _tsv%d=_a; _tsv%d=_r; _sp=%d; } }\n",
                    d-1, d-1, d, d+1, d, d-1, d, d+1);
            }
            break;
        case OP_post_dec:
            if (gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_INT) {
                /* P11.6: INT fast path — keep _ti slots valid */
                jit_buf_printf(cb,
                    "    { int64_t _ia=_ti%d; _ti%d=_ia; _ti%d=_ia-1LL; _sp=%d; }\n",
                    d-1, d-1, d, d+1);
            } else if (gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_NUMBER) {
                /* P9.4: NUMBER fast path — keep _tsd slots valid, no boxing */
                jit_buf_printf(cb,
                    "    { double _da=_tsd%d; _tsd%d=_da; _tsd%d=_da-1.0; _sp=%d; }\n",
                    d-1, d-1, d, d+1);
            } else {
                jit_buf_printf(cb,
                    "    { JSValue _a=_tsv%d;\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT){\n"
                    "        int32_t ia=JS_VALUE_GET_INT(_a);\n"
                    "        _tsv%d=_a; /* original stays */\n"
                    "        _tsv%d=(ia==INT32_MIN)?JS_NewFloat64(ctx,(double)ia-1)\n"
                    "                             :JS_NewInt32(ctx,ia-1);\n"
                    "        _sp=%d;\n"
                    "      } else { _sp=%d; JSValue _r=_RT->sub(ctx,_DUP(_a),JS_NewInt32(ctx,1));\n"
                    "               _CHK(_r); _tsv%d=_a; _tsv%d=_r; _sp=%d; } }\n",
                    d-1, d-1, d, d+1, d, d-1, d, d+1);
            }
            break;
        /* OP_inc_loc / OP_dec_loc: in-place ±1 on local variable (1-byte index) */
        case OP_inc_loc: {
            int idx = bc[pc + 1];
            if (local_type && idx < var_count && local_type[idx] == JIT_T_INT) {
                /* INT local: branch-free int64 increment — no boxing, no refcount */
                jit_buf_printf(cb, "    _jsi_%s++;\n", LNAME(idx));
            } else if (local_type && idx < var_count && local_type[idx] == JIT_T_NUMBER) {
                jit_buf_printf(cb, "    _jsd_%s+=1.0;\n", LNAME(idx));
            } else if (_CAP_LOC(idx)) {
                /* Captured JSVAL local: operate on shadow slot _cap_buf[idx] */
                jit_buf_printf(cb,
                    "    { JSValue _a=_cap_buf[%d];\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT){\n"
                    "        int32_t ia=JS_VALUE_GET_INT(_a);\n"
                    "        _cap_buf[%d]=(ia==INT32_MAX)?JS_NewFloat64(ctx,(double)ia+1)\n"
                    "                                   :JS_NewInt32(ctx,ia+1);\n"
                    "      } else { JSValue _r=_RT->add(ctx,_a,JS_NewInt32(ctx,1));\n"
                    "               _CHK(_r); _FREE(_cap_buf[%d]); _cap_buf[%d]=_r; } }\n",
                    idx, idx, idx, idx);
            } else {
                jit_buf_printf(cb,
                    "    { JSValue _a=_jsv_%s;\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT){\n"
                    "        int32_t ia=JS_VALUE_GET_INT(_a);\n"
                    "        _jsv_%s=(ia==INT32_MAX)?JS_NewFloat64(ctx,(double)ia+1)\n"
                    "                              :JS_NewInt32(ctx,ia+1);\n"
                    "      } else { JSValue _r=_RT->add(ctx,_a,JS_NewInt32(ctx,1));\n"
                    "               _CHK(_r); _FREE(_jsv_%s); _jsv_%s=_r; } }\n",
                    LNAME(idx), LNAME(idx), LNAME(idx), LNAME(idx));
            }
            break;
        }
        case OP_dec_loc: {
            int idx = bc[pc + 1];
            if (local_type && idx < var_count && local_type[idx] == JIT_T_INT) {
                /* INT local: branch-free int64 decrement — no boxing, no refcount */
                jit_buf_printf(cb, "    _jsi_%s--;\n", LNAME(idx));
            } else if (local_type && idx < var_count && local_type[idx] == JIT_T_NUMBER) {
                jit_buf_printf(cb, "    _jsd_%s-=1.0;\n", LNAME(idx));
            } else if (_CAP_LOC(idx)) {
                /* Captured JSVAL local: operate on shadow slot _cap_buf[idx] */
                jit_buf_printf(cb,
                    "    { JSValue _a=_cap_buf[%d];\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT){\n"
                    "        int32_t ia=JS_VALUE_GET_INT(_a);\n"
                    "        _cap_buf[%d]=(ia==INT32_MIN)?JS_NewFloat64(ctx,(double)ia-1)\n"
                    "                                   :JS_NewInt32(ctx,ia-1);\n"
                    "      } else { JSValue _r=_RT->sub(ctx,_a,JS_NewInt32(ctx,1));\n"
                    "               _CHK(_r); _FREE(_cap_buf[%d]); _cap_buf[%d]=_r; } }\n",
                    idx, idx, idx, idx);
            } else {
                jit_buf_printf(cb,
                    "    { JSValue _a=_jsv_%s;\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT){\n"
                    "        int32_t ia=JS_VALUE_GET_INT(_a);\n"
                    "        _jsv_%s=(ia==INT32_MIN)?JS_NewFloat64(ctx,(double)ia-1)\n"
                    "                              :JS_NewInt32(ctx,ia-1);\n"
                    "      } else { JSValue _r=_RT->sub(ctx,_a,JS_NewInt32(ctx,1));\n"
                    "               _CHK(_r); _FREE(_jsv_%s); _jsv_%s=_r; } }\n",
                    LNAME(idx), LNAME(idx), LNAME(idx), LNAME(idx));
            }
            break;
        }
        /* OP_add_loc: pop stack top and add it in-place to local (1-byte index).
         * P9.2: pop _tsv{d-1}, update _sp to d-1. */
        case OP_add_loc: {
            int idx = bc[pc + 1];
            if (local_type && idx < var_count && local_type[idx] == JIT_T_INT) {
                if (gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_INT) {
                    /* P11.6: INT source — read _ti directly */
                    jit_buf_printf(cb,
                        "    _jsi_%s+=_ti%d; _sp=%d;\n",
                        LNAME(idx), d-1, d-1);
                } else if (gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_NUMBER) {
                    /* P9.4: NUMBER source — read _tsd directly */
                    jit_buf_printf(cb,
                        "    _jsi_%s+=(int64_t)_tsd%d; _sp=%d;\n",
                        LNAME(idx), d-1, d-1);
                } else {
                    /* INT local: extract int64 from immediate stack value (INT or FLOAT64),
                     * add directly — no refcount ops, no boxing on store.
                     * Inference guarantees stack top is numeric when local is INT. */
                    jit_buf_printf(cb,
                        "    { JSValue _b=_tsv%d; _sp=%d;\n"
                        "      _jsi_%s+=(JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n"
                        "              ?(int64_t)JS_VALUE_GET_INT(_b)\n"
                        "              :(int64_t)JS_VALUE_GET_FLOAT64(_b); }\n",
                        d-1, d-1, LNAME(idx));
                }
            } else if (local_type && idx < var_count && local_type[idx] == JIT_T_NUMBER) {
                if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) {
                    /* P9.4: typed source — read _tsd directly */
                    jit_buf_printf(cb,
                        "    _jsd_%s+=_tsd%d; _sp=%d;\n",
                        LNAME(idx), d-1, d-1);
                } else {
                    /* NUMBER local: direct double add.  Inference guarantees the stack
                     * top is INT or FLOAT64 (both immediate — no _FREE needed). GCC
                     * CSE will collapse get_loc(j)+add_loc(i) → _jsd_name+=_jsd_other. */
                    jit_buf_printf(cb,
                        "    { JSValue _b=_tsv%d; _sp=%d;\n"
                        "      if(JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n"
                        "        _jsd_%s+=(double)JS_VALUE_GET_INT(_b);\n"
                        "      else _jsd_%s+=JS_VALUE_GET_FLOAT64(_b); }\n",
                        d-1, d-1, LNAME(idx), LNAME(idx));
                }
            } else if (gen_sp > 0 && gen_st[gen_sp-1] == JIT_T_INT) {
                /* P11.9: INT source — bypass _P94_ENSURE boxing; read _ti directly.
                 * JSVAL local, int64_t source: handle INT and FLOAT64 local fast paths,
                 * fall back to _RT->add for strings/objects (rare). */
                const char *_pv_expr = _CAP_LOC(idx) ? "_cap_buf" : "_jsv_";
                if (_CAP_LOC(idx)) {
                    jit_buf_printf(cb,
                        "    { int64_t _b=_ti%d; _sp=%d; JSValue *_pv=&_cap_buf[%d];\n"
                        "      if(JS_VALUE_GET_TAG(*_pv)==JS_TAG_INT){\n"
                        "        int64_t _r=(int64_t)JS_VALUE_GET_INT(*_pv)+_b;\n"
                        "        *_pv=((int32_t)_r==_r)?JS_NewInt32(ctx,(int32_t)_r)\n"
                        "                              :JS_NewFloat64(ctx,(double)_r);\n"
                        "      } else if(JS_VALUE_GET_TAG(*_pv)==JS_TAG_FLOAT64){\n"
                        "        *_pv=JS_NewFloat64(ctx,JS_VALUE_GET_FLOAT64(*_pv)+(double)_b);\n"
                        "      } else {\n"
                        "        JSValue _bb=JS_NewInt64(ctx,_b);\n"
                        "        JSValue _old=*_pv; *_pv=JS_UNDEFINED;\n"
                        "        JSValue _r=_RT->add(ctx,_old,_bb); _CHK(_r); *_pv=_r;\n"
                        "      } }\n",
                        d-1, d-1, idx);
                } else {
                    jit_buf_printf(cb,
                        "    { int64_t _b=_ti%d; _sp=%d; JSValue *_pv=&_jsv_%s;\n"
                        "      if(JS_VALUE_GET_TAG(*_pv)==JS_TAG_INT){\n"
                        "        int64_t _r=(int64_t)JS_VALUE_GET_INT(*_pv)+_b;\n"
                        "        *_pv=((int32_t)_r==_r)?JS_NewInt32(ctx,(int32_t)_r)\n"
                        "                              :JS_NewFloat64(ctx,(double)_r);\n"
                        "      } else if(JS_VALUE_GET_TAG(*_pv)==JS_TAG_FLOAT64){\n"
                        "        *_pv=JS_NewFloat64(ctx,JS_VALUE_GET_FLOAT64(*_pv)+(double)_b);\n"
                        "      } else {\n"
                        "        JSValue _bb=JS_NewInt64(ctx,_b);\n"
                        "        JSValue _old=*_pv; *_pv=JS_UNDEFINED;\n"
                        "        JSValue _r=_RT->add(ctx,_old,_bb); _CHK(_r); *_pv=_r;\n"
                        "      } }\n",
                        d-1, d-1, LNAME(idx));
                }
                (void)_pv_expr;
            } else if (_CAP_LOC(idx)) {
                /* Captured JSVAL local, JSVAL source */
                _P94_ENSURE(d-1);
                jit_buf_printf(cb,
                    "    { JSValue _b=_tsv%d; _sp=%d; JSValue *_pv=&_cap_buf[%d];\n"
                    "      if(JS_VALUE_GET_TAG(*_pv)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT){\n"
                    "        int64_t _r=(int64_t)JS_VALUE_GET_INT(*_pv)+JS_VALUE_GET_INT(_b);\n"
                    "        *_pv=((int32_t)_r==_r)?JS_NewInt32(ctx,(int32_t)_r)\n"
                    "                              :JS_NewFloat64(ctx,(double)_r);\n"
                    "      } else {\n"
                    "        JSValue _old=*_pv; *_pv=JS_UNDEFINED;\n"
                    "        JSValue _r=_RT->add(ctx,_old,_b); _CHK(_r); *_pv=_r;\n"
                    "      } }\n",
                    d-1, d-1, idx);
            } else {
                _P94_ENSURE(d-1); /* P9.4: box typed slot before use as JSValue */
                jit_buf_printf(cb,
                    "    { JSValue _b=_tsv%d; _sp=%d; JSValue *_pv=&_jsv_%s;\n"
                    "      if(JS_VALUE_GET_TAG(*_pv)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT){\n"
                    "        int64_t _r=(int64_t)JS_VALUE_GET_INT(*_pv)+JS_VALUE_GET_INT(_b);\n"
                    "        *_pv=((int32_t)_r==_r)?JS_NewInt32(ctx,(int32_t)_r)\n"
                    "                              :JS_NewFloat64(ctx,(double)_r);\n"
                    "      } else {\n"
                    /* _RT->add consumes both inputs (same ownership as interpreter
                     * stack ops). Move *_pv out first and clear the slot so the
                     * exception path (_ex) does not double-free it.              */
                    "        JSValue _old=*_pv; *_pv=JS_UNDEFINED;\n"
                    "        JSValue _r=_RT->add(ctx,_old,_b); _CHK(_r); *_pv=_r;\n"
                    "      } }\n",
                    d-1, d-1, LNAME(idx));
            }
            break;
        }

        /* ---- Comparisons (Phase 6.1: fused branch + NUMBER-aware paths) ----
         *
         * For each comparison opcode we try to fuse with the immediately
         * following if_false / if_true / if_false8 / if_true8.  If fusion is
         * possible (next opcode is a branch AND the branch pc is not itself a
         * jump target), we emit a single C block that avoids creating a JSBool
         * on the stack and saves a push/pop pair.
         *
         * When both stack operands are typed NUMBER (INT or FLOAT64) by the
         * gen-time type stack, we additionally skip the slow vtable path and
         * use direct double arithmetic, which is always correct for INT/FLOAT64
         * values and handles FLOAT64 operands that the old INT-only fast path
         * could not reach.                                                     */

/* P9.2: comparison macros use d (depth before opcode).
 * Fused comparisons: pop 2 (_tsv{d-2}, _tsv{d-1}), no push; _sp=d-2.
 * Unfused comparisons: pop 2, push 1 at _tsv{d-2}; _sp=d-1. */

/* Helper: emit fused NUMBER×NUMBER comparison+branch. */
#define GEN_CMP_FUSE_NUM(c_op, ftgt, fneg) \
    jit_buf_printf(cb, \
        "    { JSValue _va=_tsv%d,_vb=_tsv%d; _sp=%d;\n" \
        "      double _da=(JS_VALUE_GET_TAG(_va)==JS_TAG_INT)" \
                         "?(double)JS_VALUE_GET_INT(_va):JS_VALUE_GET_FLOAT64(_va);\n" \
        "      double _db=(JS_VALUE_GET_TAG(_vb)==JS_TAG_INT)" \
                         "?(double)JS_VALUE_GET_INT(_vb):JS_VALUE_GET_FLOAT64(_vb);\n" \
        "      if(%s(_da " c_op " _db)) goto _L%d; }\n", \
        d-2, d-1, d-2, (fneg)?"!":"", (ftgt))

/* P9.3: break-emitting variant of GEN_CMP_FUSE_NUM (used when branch target is loop exit) */
#define GEN_CMP_FUSE_NUM_BRK(c_op, fneg) \
    jit_buf_printf(cb, \
        "    { JSValue _va=_tsv%d,_vb=_tsv%d; _sp=%d;\n" \
        "      double _da=(JS_VALUE_GET_TAG(_va)==JS_TAG_INT)" \
                         "?(double)JS_VALUE_GET_INT(_va):JS_VALUE_GET_FLOAT64(_va);\n" \
        "      double _db=(JS_VALUE_GET_TAG(_vb)==JS_TAG_INT)" \
                         "?(double)JS_VALUE_GET_INT(_vb):JS_VALUE_GET_FLOAT64(_vb);\n" \
        "      if(%s(_da " c_op " _db)) break; }\n", \
        d-2, d-1, d-2, (fneg)?"!":"")

/* P11.6/P9.4: typed fused comparison.
 * INT×INT uses _ti; NUMBER×NUMBER or mixed uses _tsd (converting as needed).
 * Boxes any typed slots BELOW the two operands before the branch. */
#define GEN_CMP_FUSE_TSD(c_op, ftgt, fneg) do { \
    { int _bx; for (_bx=0; _bx < d-2 && _bx < gen_sp; _bx++) _P94_ENSURE(_bx); } \
    { uint8_t _ct2=_GS_TOP2(), _ct1=_GS_TOP(); \
      if (_ct2==JIT_T_INT && _ct1==JIT_T_INT) \
        jit_buf_printf(cb, "    { _sp=%d; if(%s(_ti%d " c_op " _ti%d)) goto _L%d; }\n", \
            d-2, (fneg)?"!":"", d-2, d-1, (ftgt)); \
      else if (_ct2==JIT_T_INT) \
        jit_buf_printf(cb, "    { _sp=%d; if(%s((double)_ti%d " c_op " _tsd%d)) goto _L%d; }\n", \
            d-2, (fneg)?"!":"", d-2, d-1, (ftgt)); \
      else if (_ct1==JIT_T_INT) \
        jit_buf_printf(cb, "    { _sp=%d; if(%s(_tsd%d " c_op " (double)_ti%d)) goto _L%d; }\n", \
            d-2, (fneg)?"!":"", d-2, d-1, (ftgt)); \
      else \
        jit_buf_printf(cb, "    { _sp=%d; if(%s(_tsd%d " c_op " _tsd%d)) goto _L%d; }\n", \
            d-2, (fneg)?"!":"", d-2, d-1, (ftgt)); \
    } \
} while(0)

/* P11.6: break-emitting variant of GEN_CMP_FUSE_TSD */
#define GEN_CMP_FUSE_TSD_BRK(c_op, fneg) do { \
    { int _bx; for (_bx=0; _bx < d-2 && _bx < gen_sp; _bx++) _P94_ENSURE(_bx); } \
    { uint8_t _ct2=_GS_TOP2(), _ct1=_GS_TOP(); \
      if (_ct2==JIT_T_INT && _ct1==JIT_T_INT) \
        jit_buf_printf(cb, "    { _sp=%d; if(%s(_ti%d " c_op " _ti%d)) break; }\n", \
            d-2, (fneg)?"!":"", d-2, d-1); \
      else if (_ct2==JIT_T_INT) \
        jit_buf_printf(cb, "    { _sp=%d; if(%s((double)_ti%d " c_op " _tsd%d)) break; }\n", \
            d-2, (fneg)?"!":"", d-2, d-1); \
      else if (_ct1==JIT_T_INT) \
        jit_buf_printf(cb, "    { _sp=%d; if(%s(_tsd%d " c_op " (double)_ti%d)) break; }\n", \
            d-2, (fneg)?"!":"", d-2, d-1); \
      else \
        jit_buf_printf(cb, "    { _sp=%d; if(%s(_tsd%d " c_op " _tsd%d)) break; }\n", \
            d-2, (fneg)?"!":"", d-2, d-1); \
    } \
} while(0)

/* Helper: emit fused general comparison+branch — INT fast path, float64 middle
 * path (P8.6), vtable fallback. */
#define GEN_CMP_FUSE_GEN(int_op, c_op, rt_call, ftgt, fneg) \
    jit_buf_printf(cb, \
        "    { JSValue _a=_tsv%d,_b=_tsv%d; _sp=%d; int _cond;\n" \
        "      int _ta=JS_VALUE_GET_TAG(_a),_tb=JS_VALUE_GET_TAG(_b);\n" \
        "      if(_ta==JS_TAG_INT&&_tb==JS_TAG_INT)\n" \
        "        _cond=(JS_VALUE_GET_INT(_a) " int_op " JS_VALUE_GET_INT(_b));\n" \
        "      else if((_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)&&" \
                      "(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n" \
        "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n" \
        "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n" \
        "        _cond=(_da " c_op " _db);\n" \
        "      } else{JSValue _r=" rt_call "; _CHK(_r); _cond=JS_VALUE_GET_INT(_r);}\n" \
        "      if(%s_cond) goto _L%d; }\n", \
        d-2, d-1, d-2, (fneg)?"!":"", (ftgt))

/* P9.3: break-emitting variant of GEN_CMP_FUSE_GEN (used when branch target is loop exit) */
#define GEN_CMP_FUSE_GEN_BRK(int_op, c_op, rt_call, fneg) \
    jit_buf_printf(cb, \
        "    { JSValue _a=_tsv%d,_b=_tsv%d; _sp=%d; int _cond;\n" \
        "      int _ta=JS_VALUE_GET_TAG(_a),_tb=JS_VALUE_GET_TAG(_b);\n" \
        "      if(_ta==JS_TAG_INT&&_tb==JS_TAG_INT)\n" \
        "        _cond=(JS_VALUE_GET_INT(_a) " int_op " JS_VALUE_GET_INT(_b));\n" \
        "      else if((_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)&&" \
                      "(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n" \
        "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n" \
        "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n" \
        "        _cond=(_da " c_op " _db);\n" \
        "      } else{JSValue _r=" rt_call "; _CHK(_r); _cond=JS_VALUE_GET_INT(_r);}\n" \
        "      if(%s_cond) break; }\n", \
        d-2, d-1, d-2, (fneg)?"!":"")

/* P44: fused half-typed comparison: left=NUMBER/INT typed, right=JSVAL.
 * c_op: operator (literal string, e.g. "<").
 * slow_vt: vtable call string, uses _aa (boxed typed left) and _b (JSVAL right).
 *   For gt/gte (reversed vtable): use "_RT->lt(ctx,_b,_aa)" etc.
 * When left is INT typed, uses integer comparison path to avoid double conversion.
 * Boxes slots below the two operands before jumping. */
#define GEN_CMP_FUSE_HALF_L(c_op, slow_vt, ftgt, fneg) do { \
    { int _bx; for (_bx=0; _bx < d-2 && _bx < gen_sp; _bx++) _P94_ENSURE(_bx); } \
    { uint8_t _ch2=_GS_TOP2(); \
      if (_ch2==JIT_T_INT) { \
        jit_buf_printf(cb, \
            "    { JSValue _b=_tsv%d; int _tb=JS_VALUE_GET_TAG(_b);\n" \
            "      int _cond;\n" \
            "      if(js_likely(_tb==JS_TAG_INT)){\n" \
            "        _cond=((int32_t)_ti%d " c_op " JS_VALUE_GET_INT(_b));\n" \
            "      } else if(_tb==JS_TAG_FLOAT64){\n" \
            "        _cond=((double)_ti%d " c_op " JS_VALUE_GET_FLOAT64(_b));\n" \
            "      } else { JSValue _aa=JS_NewFloat64(ctx,(double)_ti%d);\n" \
            "               JSValue _rv=" slow_vt "; _CHK(_rv); _cond=JS_VALUE_GET_INT(_rv); }\n" \
            "      _sp=%d; if(%s_cond) goto _L%d; }\n", \
            d-1, d-2, d-2, d-2, d-2, (fneg)?"!":"", (ftgt)); \
      } else { \
        jit_buf_printf(cb, \
            "    { JSValue _b=_tsv%d; int _tb=JS_VALUE_GET_TAG(_b);\n" \
            "      int _cond;\n" \
            "      if(js_likely(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n" \
            "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n" \
            "        _cond=(_tsd%d " c_op " _db);\n" \
            "      } else { JSValue _aa=JS_NewFloat64(ctx,_tsd%d);\n" \
            "               JSValue _rv=" slow_vt "; _CHK(_rv); _cond=JS_VALUE_GET_INT(_rv); }\n" \
            "      _sp=%d; if(%s_cond) goto _L%d; }\n", \
            d-1, d-2, d-2, d-2, (fneg)?"!":"", (ftgt)); \
      } \
    } \
} while(0)

/* P44: fused half-typed comparison: left=JSVAL, right=NUMBER/INT typed.
 * slow_vt: uses _a (JSVAL left) and _bb (boxed typed right).
 * When right is INT typed, uses integer comparison path to avoid double conversion. */
#define GEN_CMP_FUSE_HALF_R(c_op, slow_vt, ftgt, fneg) do { \
    { int _bx; for (_bx=0; _bx < d-2 && _bx < gen_sp; _bx++) _P94_ENSURE(_bx); } \
    { uint8_t _ch1=_GS_TOP(); \
      if (_ch1==JIT_T_INT) { \
        jit_buf_printf(cb, \
            "    { JSValue _a=_tsv%d; int _ta=JS_VALUE_GET_TAG(_a);\n" \
            "      int _cond;\n" \
            "      if(js_likely(_ta==JS_TAG_INT)){\n" \
            "        _cond=(JS_VALUE_GET_INT(_a) " c_op " (int32_t)_ti%d);\n" \
            "      } else if(_ta==JS_TAG_FLOAT64){\n" \
            "        _cond=(JS_VALUE_GET_FLOAT64(_a) " c_op " (double)_ti%d);\n" \
            "      } else { JSValue _bb=JS_NewFloat64(ctx,(double)_ti%d);\n" \
            "               JSValue _rv=" slow_vt "; _CHK(_rv); _cond=JS_VALUE_GET_INT(_rv); }\n" \
            "      _sp=%d; if(%s_cond) goto _L%d; }\n", \
            d-2, d-1, d-1, d-1, d-2, (fneg)?"!":"", (ftgt)); \
      } else { \
        jit_buf_printf(cb, \
            "    { JSValue _a=_tsv%d; int _ta=JS_VALUE_GET_TAG(_a);\n" \
            "      int _cond;\n" \
            "      if(js_likely(_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)){\n" \
            "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n" \
            "        _cond=(_da " c_op " _tsd%d);\n" \
            "      } else { JSValue _bb=JS_NewFloat64(ctx,_tsd%d);\n" \
            "               JSValue _rv=" slow_vt "; _CHK(_rv); _cond=JS_VALUE_GET_INT(_rv); }\n" \
            "      _sp=%d; if(%s_cond) goto _L%d; }\n", \
            d-2, d-1, d-1, d-2, (fneg)?"!":"", (ftgt)); \
      } \
    } \
} while(0)

/* Helper: unfused comparison (produces BOOL on stack).
 * P9.2: pop 2, push 1 at _tsv{d-2}; _sp = d-1.
 * rt_vt is the vtable call expression producing the result JSValue _r. */
#define GEN_CMP_UNFUSED(int_op, c_op, rt_vt) \
    jit_buf_printf(cb, \
        "    { JSValue _b=_tsv%d,_a=_tsv%d;\n" \
        "      int _ta=JS_VALUE_GET_TAG(_a),_tb=JS_VALUE_GET_TAG(_b);\n" \
        "      if(_ta==JS_TAG_INT&&_tb==JS_TAG_INT)\n" \
        "        _tsv%d=JS_NewBool(ctx,JS_VALUE_GET_INT(_a) " int_op " JS_VALUE_GET_INT(_b));\n" \
        "      else if((_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)&&" \
                      "(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n" \
        "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n" \
        "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n" \
        "        _tsv%d=JS_NewBool(ctx,_da " c_op " _db);\n" \
        "      } else { _sp=%d; JSValue _r=" rt_vt "; _CHK(_r); _tsv%d=_r; }\n" \
        "      _sp=%d; }\n", \
        d-1, d-2, d-2, d-2, d-2, d-2, d-1)

/* Shared logic for comparison opcodes. Usage:
 *   DO_CMP(c_op, int_op, vtable_call_expr_normal, vtable_call_expr_if_neg_int)
 * c_op: C double operator (e.g. "<")
 * int_op: C int operator (e.g. "<")
 * vt_call: vtable call using _a,_b (normal order)
 * vt_neg_call: vtable call for negated-int case (neq/strict_neq use eq/strict_eq)
 * For gt: vt_call is _RT->lt(ctx,_b,_a) (reversed)
 * For neq/strict_neq the unfused code is slightly different — handle separately */

        case OP_lt: {
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            uint8_t _t2=_GS_TOP2(), _t1=_GS_TOP();
            int _bn  = (gen_sp<=d && _t2>=JIT_T_NUMBER&&_t2<=JIT_T_INT && _t1>=JIT_T_NUMBER&&_t1<=JIT_T_INT);
            int _t2n = (gen_sp<=d && _t2>=JIT_T_NUMBER&&_t2<=JIT_T_INT);
            int _t1n = (gen_sp<=d && _t1>=JIT_T_NUMBER&&_t1<=JIT_T_INT);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn)        GEN_CMP_FUSE_TSD("<", _fi.tgt, _fi.negate);
                else if (_t2n)  GEN_CMP_FUSE_HALF_L("<", "_RT->lt(ctx,_aa,_b)",  _fi.tgt, _fi.negate);
                else if (_t1n)  GEN_CMP_FUSE_HALF_R("<", "_RT->lt(ctx,_a,_bb)",  _fi.tgt, _fi.negate);
                else { _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                       GEN_CMP_FUSE_GEN("<", "<", "_RT->lt(ctx,_a,_b)", _fi.tgt, _fi.negate); }
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                GEN_CMP_UNFUSED("<", "<", "_RT->lt(ctx,_a,_b)");
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }
        case OP_lte: {
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            uint8_t _t2=_GS_TOP2(), _t1=_GS_TOP();
            int _bn  = (gen_sp<=d && _t2>=JIT_T_NUMBER&&_t2<=JIT_T_INT && _t1>=JIT_T_NUMBER&&_t1<=JIT_T_INT);
            int _t2n = (gen_sp<=d && _t2>=JIT_T_NUMBER&&_t2<=JIT_T_INT);
            int _t1n = (gen_sp<=d && _t1>=JIT_T_NUMBER&&_t1<=JIT_T_INT);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn)        GEN_CMP_FUSE_TSD("<=", _fi.tgt, _fi.negate);
                else if (_t2n)  GEN_CMP_FUSE_HALF_L("<=", "_RT->lte(ctx,_aa,_b)", _fi.tgt, _fi.negate);
                else if (_t1n)  GEN_CMP_FUSE_HALF_R("<=", "_RT->lte(ctx,_a,_bb)", _fi.tgt, _fi.negate);
                else { _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                       GEN_CMP_FUSE_GEN("<=", "<=", "_RT->lte(ctx,_a,_b)", _fi.tgt, _fi.negate); }
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                GEN_CMP_UNFUSED("<=", "<=", "_RT->lte(ctx,_a,_b)");
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }
        case OP_gt: {
            /* a > b  ≡  b < a  (strict): vtable uses lt(b,a) */
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            uint8_t _t2=_GS_TOP2(), _t1=_GS_TOP();
            int _bn  = (gen_sp<=d && _t2>=JIT_T_NUMBER&&_t2<=JIT_T_INT && _t1>=JIT_T_NUMBER&&_t1<=JIT_T_INT);
            int _t2n = (gen_sp<=d && _t2>=JIT_T_NUMBER&&_t2<=JIT_T_INT);
            int _t1n = (gen_sp<=d && _t1>=JIT_T_NUMBER&&_t1<=JIT_T_INT);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn)        GEN_CMP_FUSE_TSD(">", _fi.tgt, _fi.negate);
                else if (_t2n)  GEN_CMP_FUSE_HALF_L(">", "_RT->lt(ctx,_b,_aa)",  _fi.tgt, _fi.negate);
                else if (_t1n)  GEN_CMP_FUSE_HALF_R(">", "_RT->lt(ctx,_bb,_a)",  _fi.tgt, _fi.negate);
                else { _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                       GEN_CMP_FUSE_GEN(">", ">", "_RT->lt(ctx,_b,_a)", _fi.tgt, _fi.negate); }
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                GEN_CMP_UNFUSED(">", ">", "_RT->lt(ctx,_b,_a)");
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }
        case OP_gte: {
            /* a >= b  ≡  b <= a  (inclusive): vtable uses lte(b,a) */
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            uint8_t _t2=_GS_TOP2(), _t1=_GS_TOP();
            int _bn  = (gen_sp<=d && _t2>=JIT_T_NUMBER&&_t2<=JIT_T_INT && _t1>=JIT_T_NUMBER&&_t1<=JIT_T_INT);
            int _t2n = (gen_sp<=d && _t2>=JIT_T_NUMBER&&_t2<=JIT_T_INT);
            int _t1n = (gen_sp<=d && _t1>=JIT_T_NUMBER&&_t1<=JIT_T_INT);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn)        GEN_CMP_FUSE_TSD(">=", _fi.tgt, _fi.negate);
                else if (_t2n)  GEN_CMP_FUSE_HALF_L(">=", "_RT->lte(ctx,_b,_aa)", _fi.tgt, _fi.negate);
                else if (_t1n)  GEN_CMP_FUSE_HALF_R(">=", "_RT->lte(ctx,_bb,_a)", _fi.tgt, _fi.negate);
                else { _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                       GEN_CMP_FUSE_GEN(">=", ">=", "_RT->lte(ctx,_b,_a)", _fi.tgt, _fi.negate); }
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                GEN_CMP_UNFUSED(">=", ">=", "_RT->lte(ctx,_b,_a)");
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }

        case OP_eq: {
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            int _bn = (gen_sp <= d && _GS_TOP2()>=JIT_T_NUMBER && _GS_TOP2()<=JIT_T_INT && _GS_TOP()>=JIT_T_NUMBER && _GS_TOP()<=JIT_T_INT);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn) GEN_CMP_FUSE_TSD("==", _fi.tgt, _fi.negate);
                else { _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                       GEN_CMP_FUSE_GEN("==", "==", "_RT->eq(ctx,_a,_b)",  _fi.tgt, _fi.negate); }
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                GEN_CMP_UNFUSED("==", "==", "_RT->eq(ctx,_a,_b)");
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }
        case OP_neq: {
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            int _bn = (gen_sp <= d && _GS_TOP2()>=JIT_T_NUMBER && _GS_TOP2()<=JIT_T_INT && _GS_TOP()>=JIT_T_NUMBER && _GS_TOP()<=JIT_T_INT);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                /* neq fused: "!=" is the comparison; negate inverts it */
                if (_bn) GEN_CMP_FUSE_TSD("!=", _fi.tgt, _fi.negate);
                else {
                    _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                    jit_buf_printf(cb,
                        "    { JSValue _a=_tsv%d,_b=_tsv%d; _sp=%d; int _cond;\n"
                        "      int _ta=JS_VALUE_GET_TAG(_a),_tb=JS_VALUE_GET_TAG(_b);\n"
                        "      if(_ta==JS_TAG_INT&&_tb==JS_TAG_INT)\n"
                        "        _cond=(JS_VALUE_GET_INT(_a)!=JS_VALUE_GET_INT(_b));\n"
                        "      else if((_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)&&"
                                 "(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                        "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                        "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                        "        _cond=(_da!=_db);\n"
                        "      } else{JSValue _r=_RT->eq(ctx,_a,_b);_CHK(_r);_cond=!JS_VALUE_GET_INT(_r);}\n"
                        "      if(%s_cond) goto _L%d; }\n",
                        d-2, d-1, d-2, _fi.negate?"!":"", _fi.tgt);
                }
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                jit_buf_printf(cb,
                    "    { JSValue _b=_tsv%d,_a=_tsv%d;\n"
                    "      int _ta=JS_VALUE_GET_TAG(_a),_tb=JS_VALUE_GET_TAG(_b);\n"
                    "      if(_ta==JS_TAG_INT&&_tb==JS_TAG_INT)\n"
                    "        _tsv%d=JS_NewBool(ctx,JS_VALUE_GET_INT(_a)!=JS_VALUE_GET_INT(_b));\n"
                    "      else if((_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)&&"
                             "(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                    "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                    "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                    "        _tsv%d=JS_NewBool(ctx,_da!=_db);\n"
                    "      } else { _sp=%d; JSValue _r=_RT->eq(ctx,_a,_b); _CHK(_r);\n"
                    "               _tsv%d=JS_NewBool(ctx,!JS_VALUE_GET_INT(_r)); _FREE(_r); }\n"
                    "      _sp=%d; }\n",
                    d-1, d-2, d-2, d-2, d-2, d-2, d-1);
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }
        case OP_strict_eq: {
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            int _bn = (gen_sp <= d && _GS_TOP2()>=JIT_T_NUMBER && _GS_TOP2()<=JIT_T_INT && _GS_TOP()>=JIT_T_NUMBER && _GS_TOP()<=JIT_T_INT);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn) GEN_CMP_FUSE_TSD("==", _fi.tgt, _fi.negate);
                else { _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                       GEN_CMP_FUSE_GEN("==", "==", "_RT->strict_eq(ctx,_a,_b)", _fi.tgt, _fi.negate); }
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                GEN_CMP_UNFUSED("==", "==", "_RT->strict_eq(ctx,_a,_b)");
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }
        case OP_strict_neq: {
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            int _bn = (gen_sp <= d && _GS_TOP2()>=JIT_T_NUMBER && _GS_TOP2()<=JIT_T_INT && _GS_TOP()>=JIT_T_NUMBER && _GS_TOP()<=JIT_T_INT);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn) GEN_CMP_FUSE_TSD("!=", _fi.tgt, _fi.negate);
                else {
                    _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                    jit_buf_printf(cb,
                        "    { JSValue _a=_tsv%d,_b=_tsv%d; _sp=%d; int _cond;\n"
                        "      int _ta=JS_VALUE_GET_TAG(_a),_tb=JS_VALUE_GET_TAG(_b);\n"
                        "      if(_ta==JS_TAG_INT&&_tb==JS_TAG_INT)\n"
                        "        _cond=(JS_VALUE_GET_INT(_a)!=JS_VALUE_GET_INT(_b));\n"
                        "      else if((_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)&&"
                                 "(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                        "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                        "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                        "        _cond=(_da!=_db);\n"
                        "      } else{JSValue _r=_RT->strict_eq(ctx,_a,_b);_CHK(_r);_cond=!JS_VALUE_GET_INT(_r);}\n"
                        "      if(%s_cond) goto _L%d; }\n",
                        d-2, d-1, d-2, _fi.negate?"!":"", _fi.tgt);
                }
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                jit_buf_printf(cb,
                    "    { JSValue _b=_tsv%d,_a=_tsv%d;\n"
                    "      int _ta=JS_VALUE_GET_TAG(_a),_tb=JS_VALUE_GET_TAG(_b);\n"
                    "      if(_ta==JS_TAG_INT&&_tb==JS_TAG_INT)\n"
                    "        _tsv%d=JS_NewBool(ctx,JS_VALUE_GET_INT(_a)!=JS_VALUE_GET_INT(_b));\n"
                    "      else if((_ta==JS_TAG_INT||_ta==JS_TAG_FLOAT64)&&"
                             "(_tb==JS_TAG_INT||_tb==JS_TAG_FLOAT64)){\n"
                    "        double _da=_ta==JS_TAG_INT?(double)JS_VALUE_GET_INT(_a):JS_VALUE_GET_FLOAT64(_a);\n"
                    "        double _db=_tb==JS_TAG_INT?(double)JS_VALUE_GET_INT(_b):JS_VALUE_GET_FLOAT64(_b);\n"
                    "        _tsv%d=JS_NewBool(ctx,_da!=_db);\n"
                    "      } else { _sp=%d; JSValue _r=_RT->strict_eq(ctx,_a,_b); _CHK(_r);\n"
                    "               _tsv%d=JS_NewBool(ctx,!JS_VALUE_GET_INT(_r)); _FREE(_r); }\n"
                    "      _sp=%d; }\n",
                    d-1, d-2, d-2, d-2, d-2, d-2, d-1);
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }

#undef GEN_CMP_FUSE_NUM
#undef GEN_CMP_FUSE_NUM_BRK
#undef GEN_CMP_FUSE_TSD
#undef GEN_CMP_FUSE_TSD_BRK
#undef GEN_CMP_FUSE_GEN
#undef GEN_CMP_FUSE_GEN_BRK
#undef GEN_CMP_UNFUSED

        /* ---- instanceof / in ---- */
        /* P9.4/P9.2: pop 2, push 1 at _tsv{d-2}; result depth d-1 */
        case OP_instanceof:
            _P94_ENSURE(d-2); _P94_ENSURE(d-1); /* P9.4: box typed slots */
            jit_buf_printf(cb,
                "    { JSValue _b=_tsv%d,_a=_tsv%d; _sp=%d;\n"
                "      int _r=JS_OrdinaryIsInstanceOf(ctx,_a,_b);\n"
                "      _FREE(_a); _FREE(_b);\n"
                "      if(_r<0) goto _ex;\n"
                "      _tsv%d=JS_NewBool(ctx,_r); _sp=%d; }\n",
                d-1, d-2, d-2, d-2, d-1);
            break;
        case OP_in:
            _P94_ENSURE(d-2); _P94_ENSURE(d-1); /* P9.4: box typed slots */
            jit_buf_printf(cb,
                "    { JSValue _b=_tsv%d,_a=_tsv%d; _sp=%d;\n"
                "      int _r=JS_HasProperty(ctx,_b,JS_ValueToAtom(ctx,_a));\n"
                "      _FREE(_a); _FREE(_b);\n"
                "      if(_r<0) goto _ex;\n"
                "      _tsv%d=JS_NewBool(ctx,_r); _sp=%d; }\n",
                d-1, d-2, d-2, d-2, d-1);
            break;

        /* ---- Control flow ---- */
        /* P9.4/P9.2: if_false/if_true: pop 1 from _tsv{d-1}/_tsd{d-1}; _sp = d-1 */
        case OP_if_false: {
            int32_t delta = (int32_t)bc_u32(&bc[pc+1]);
            int tgt = pc + 1 + delta;
            _borrowed_depth = -1; /* P38.1: borrow cannot survive a branch */
            if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) {
                /* P9.4: typed fast path — 0.0 or NaN is falsy.
                 * Box any typed slots below the condition before branching. */
                { int _bx; for (_bx=0; _bx < d-1 && _bx < gen_sp-1; _bx++) _P94_ENSURE(_bx); }
                jit_buf_printf(cb,
                    "    { _sp=%d; if(_tsd%d==0.0||_tsd%d!=_tsd%d) goto _L%d; }\n",
                    d-1, d-1, d-1, d-1, tgt);
            } else {
                /* Always use goto rather than C break: break only lands at exit_pc
                 * when no other labels exist between the while back-edge and exit_pc.
                 * In loops with multiple "continue" paths (e.g. while-with-manual-advance),
                 * those extra labels appear right after } while and break lands there
                 * instead, causing incorrect control flow (P9.3 bug fix). */
                _P94_ENSURE(d-1);
                jit_buf_printf(cb,
                    "    { JSValue _v=_tsv%d; _sp=%d; int _b=_BOOL(_v);"
                    " _FREE(_v); if(!_b) goto _L%d; }\n", d-1, d-1, tgt);
            }
            break;
        }
        case OP_if_true: {
            int32_t delta = (int32_t)bc_u32(&bc[pc+1]);
            int tgt = pc + 1 + delta;
            _borrowed_depth = -1; /* P38.1: borrow cannot survive a branch */
            if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) {
                /* P9.4: typed fast path — non-zero and non-NaN is truthy.
                 * Box any typed slots below the condition before branching. */
                { int _bx; for (_bx=0; _bx < d-1 && _bx < gen_sp-1; _bx++) _P94_ENSURE(_bx); }
                jit_buf_printf(cb,
                    "    { _sp=%d; if(_tsd%d!=0.0&&_tsd%d==_tsd%d) goto _L%d; }\n",
                    d-1, d-1, d-1, d-1, tgt);
            } else {
                /* Always use goto — see OP_if_false comment above. */
                _P94_ENSURE(d-1);
                jit_buf_printf(cb,
                    "    { JSValue _v=_tsv%d; _sp=%d; int _b=_BOOL(_v);"
                    " _FREE(_v); if(_b) goto _L%d; }\n", d-1, d-1, tgt);
            }
            break;
        }
        case OP_goto: {
            int32_t delta = (int32_t)bc_u32(&bc[pc+1]);
            int tgt = pc + 1 + delta;
            _borrowed_depth = -1; /* P38.1: borrow cannot survive a goto */
            /* P9.4: box typed surviving slots before goto — target label resets gen_st */
            { int _bx; for (_bx=0; _bx < gen_sp; _bx++) _P94_ENSURE(_bx); }
            jit_buf_printf(cb, "    goto _L%d;\n", tgt);
            break;
        }
        case OP_if_false8: {
            int tgt = pc + 1 + (int)(int8_t)bc[pc+1];
            _borrowed_depth = -1; /* P38.1: borrow cannot survive a branch */
            if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) {
                /* P9.4: typed fast path — box surviving slots below condition */
                { int _bx; for (_bx=0; _bx < d-1 && _bx < gen_sp-1; _bx++) _P94_ENSURE(_bx); }
                jit_buf_printf(cb,
                    "    { _sp=%d; if(_tsd%d==0.0||_tsd%d!=_tsd%d) goto _L%d; }\n",
                    d-1, d-1, d-1, d-1, tgt);
            } else {
                /* Always use goto — see OP_if_false comment above. */
                _P94_ENSURE(d-1);
                jit_buf_printf(cb,
                    "    { JSValue _v=_tsv%d; _sp=%d; int _b=_BOOL(_v);"
                    " _FREE(_v); if(!_b) goto _L%d; }\n", d-1, d-1, tgt);
            }
            break;
        }
        case OP_if_true8: {
            int tgt = pc + 1 + (int)(int8_t)bc[pc+1];
            _borrowed_depth = -1; /* P38.1: borrow cannot survive a branch */
            if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) {
                /* P9.4: typed fast path — box surviving slots below condition */
                { int _bx; for (_bx=0; _bx < d-1 && _bx < gen_sp-1; _bx++) _P94_ENSURE(_bx); }
                jit_buf_printf(cb,
                    "    { _sp=%d; if(_tsd%d!=0.0&&_tsd%d==_tsd%d) goto _L%d; }\n",
                    d-1, d-1, d-1, d-1, tgt);
            } else {
                /* Always use goto — see OP_if_false comment above. */
                _P94_ENSURE(d-1);
                jit_buf_printf(cb,
                    "    { JSValue _v=_tsv%d; _sp=%d; int _b=_BOOL(_v);"
                    " _FREE(_v); if(_b) goto _L%d; }\n", d-1, d-1, tgt);
            }
            break;
        }
        case OP_goto8: {
            int tgt = pc + 1 + (int)(int8_t)bc[pc+1];
            _borrowed_depth = -1; /* P38.1: borrow cannot survive a goto */
            /* P9.4: box typed surviving slots before goto */
            { int _bx; for (_bx=0; _bx < gen_sp; _bx++) _P94_ENSURE(_bx); }
            jit_buf_printf(cb, "    goto _L%d;\n", tgt);
            break;
        }
        case OP_goto16: {
            int tgt = pc + 1 + (int)(int16_t)bc_u16(&bc[pc+1]);
            _borrowed_depth = -1; /* P38.1: borrow cannot survive a goto */
            /* P9.4: box typed surviving slots before goto */
            { int _bx; for (_bx=0; _bx < gen_sp; _bx++) _P94_ENSURE(_bx); }
            jit_buf_printf(cb, "    goto _L%d;\n", tgt);
            break;
        }

        /* ---- Property access (with inline bimorphic property cache) ---- */
        /* P9.4/P9.2: get_field: pop obj, push result; depth unchanged (d->d).
         * P37.4: upgraded to JSJITICEntry2 (bimorphic IC with 2 shape slots).
         * Primary hit: e[0].  Secondary hit: e[1] (checked when n>=2).
         * P37.3: top_borrowed==1 → preceding get_loc skipped DupValue, so
         * omit _FREE(_o) in both IC hit paths and the miss path. */
        case OP_get_field: {
            uint32_t atom = bc_u32(&bc[pc+1]);
            _P94_ENSURE(d-1); /* P9.4: box typed obj slot (defensive) */
            /* P11.1/P11.5: IC hit path inlined; _CHK only in miss path.
             * P43.2: quadrimorphic — four shape slots (e[0]..e[3]).
             * P39.2 fix: read prop array from the current object, not the IC.
             * P45: DupValue elision for INT (tag check in hit path).
             * P45b: INT-hint fast path — speculative INT extraction when warm
             *       hints say this property is always int (val_tag==0==JS_TAG_INT).
             *       gen_st=INT when hint active; downstream arithmetic uses _ti. */
            /* P45b: check if warm hint says this get_field always returns INT. */
            int _p45b_use_int = (vt_hints && gf_idx < n_gf &&
                                 vt_hints[gf_idx] == 0 /* JS_TAG_INT */);
            /* Macro shorthand for the __jit_vt_ miss-path update */
#define _GF_VT_UPDATE "             __jit_vt_%016llx[%d]=_ic%d.e[0].val_tag;\n"
            if (_p45b_use_int) {
                /* P45b warm INT path:
                 * IC hit: _ti{d-1} = JS_VALUE_GET_INT(_r) (speculative, no DupValue)
                 * IC miss: extract INT, JS_FreeValue for non-INT cleanup */
                if (top_borrowed) {
                    jit_buf_printf(cb,
                        "    { static JSJITICEntry2 _ic%d={{},0};\n"
                        "      JSValue _o=_tsv%d, _r;\n"
                        "      if (js_likely(JIT_IC_CHECK_FAST(_o,&_ic%d.e[0]))){\n"
                        "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                        "          _r=_pp[_ic%d.e[0].slot];\n"
                        "          _ti%d=(int64_t)JS_VALUE_GET_INT(_r); _sp=%d; }\n"
                        "      else if (js_likely(_ic%d.n>=2&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[1]))){\n"
                        "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                        "          _r=_pp[_ic%d.e[1].slot];\n"
                        "          _ti%d=(int64_t)JS_VALUE_GET_INT(_r); _sp=%d; }\n"
                        "      else if (js_likely(_ic%d.n>=3&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[2]))){\n"
                        "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                        "          _r=_pp[_ic%d.e[2].slot];\n"
                        "          _ti%d=(int64_t)JS_VALUE_GET_INT(_r); _sp=%d; }\n"
                        "      else if (js_likely(_ic%d.n>=4&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[3]))){\n"
                        "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                        "          _r=_pp[_ic%d.e[3].slot];\n"
                        "          _ti%d=(int64_t)JS_VALUE_GET_INT(_r); _sp=%d; }\n"
                        "      else { _r=_RT->get_prop(ctx,_o,(JSAtom)%uu);\n"
                        "             js_jit_ic2_fill_get(ctx,_o,(JSAtom)%uu,&_ic%d);\n"
                        _GF_VT_UPDATE
                        "             _sp=%d; _CHK(_r);\n"
                        "             _ti%d=(int64_t)(JS_VALUE_GET_TAG(_r)==JS_TAG_INT\n"
                        "                            ?JS_VALUE_GET_INT(_r):0);\n"
                        "             JS_FreeValue(ctx,_r); _sp=%d; } }\n",
                        pc,                             /* _ic%d static */
                        d-1,                            /* _o=_tsv%d */
                        pc,                             /* IC e[0] */
                        pc,                             /* e[0].slot */
                        d-1, d,                         /* _ti%d; _sp=%d */
                        pc, pc,                         /* n>=2 && e[1] */
                        pc,                             /* e[1].slot */
                        d-1, d,                         /* _ti%d; _sp=%d */
                        pc, pc,                         /* n>=3 && e[2] */
                        pc,                             /* e[2].slot */
                        d-1, d,                         /* _ti%d; _sp=%d */
                        pc, pc,                         /* n>=4 && e[3] */
                        pc,                             /* e[3].slot */
                        d-1, d,                         /* _ti%d; _sp=%d */
                        atom, atom, pc,                 /* miss get_prop + fill */
                        (unsigned long long)bc_hash, gf_idx, pc, /* VT update */
                        d-1,                            /* _sp=d-1 before CHK */
                        d-1,                            /* _ti%d extract */
                        d);                             /* _sp=d after */
                } else {
                    jit_buf_printf(cb,
                        "    { static JSJITICEntry2 _ic%d={{},0};\n"
                        "      JSValue _o=_tsv%d, _r;\n"
                        "      if (js_likely(JIT_IC_CHECK_FAST(_o,&_ic%d.e[0]))){\n"
                        "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                        "          _r=_pp[_ic%d.e[0].slot];\n"
                        "          _ti%d=(int64_t)JS_VALUE_GET_INT(_r); _FREE(_o); _sp=%d; }\n"
                        "      else if (js_likely(_ic%d.n>=2&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[1]))){\n"
                        "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                        "          _r=_pp[_ic%d.e[1].slot];\n"
                        "          _ti%d=(int64_t)JS_VALUE_GET_INT(_r); _FREE(_o); _sp=%d; }\n"
                        "      else if (js_likely(_ic%d.n>=3&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[2]))){\n"
                        "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                        "          _r=_pp[_ic%d.e[2].slot];\n"
                        "          _ti%d=(int64_t)JS_VALUE_GET_INT(_r); _FREE(_o); _sp=%d; }\n"
                        "      else if (js_likely(_ic%d.n>=4&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[3]))){\n"
                        "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                        "          _r=_pp[_ic%d.e[3].slot];\n"
                        "          _ti%d=(int64_t)JS_VALUE_GET_INT(_r); _FREE(_o); _sp=%d; }\n"
                        "      else { _r=_RT->get_prop(ctx,_o,(JSAtom)%uu);\n"
                        "             js_jit_ic2_fill_get(ctx,_o,(JSAtom)%uu,&_ic%d);\n"
                        _GF_VT_UPDATE
                        "             _FREE(_o); _sp=%d; _CHK(_r);\n"
                        "             _ti%d=(int64_t)(JS_VALUE_GET_TAG(_r)==JS_TAG_INT\n"
                        "                            ?JS_VALUE_GET_INT(_r):0);\n"
                        "             JS_FreeValue(ctx,_r); _sp=%d; } }\n",
                        pc,                             /* _ic%d static */
                        d-1,                            /* _o=_tsv%d */
                        pc,                             /* IC e[0] */
                        pc,                             /* e[0].slot */
                        d-1, d,                         /* _ti%d; _FREE(_o); _sp=%d */
                        pc, pc,                         /* n>=2 && e[1] */
                        pc,                             /* e[1].slot */
                        d-1, d,                         /* _ti%d; _FREE(_o); _sp=%d */
                        pc, pc,                         /* n>=3 && e[2] */
                        pc,                             /* e[2].slot */
                        d-1, d,                         /* _ti%d; _FREE(_o); _sp=%d */
                        pc, pc,                         /* n>=4 && e[3] */
                        pc,                             /* e[3].slot */
                        d-1, d,                         /* _ti%d; _FREE(_o); _sp=%d */
                        atom, atom, pc,                 /* miss get_prop + fill */
                        (unsigned long long)bc_hash, gf_idx, pc, /* VT update */
                        d-1,                            /* _FREE(_o); _sp=d-1 */
                        d-1,                            /* _ti%d extract */
                        d);                             /* _sp=d after */
                }
            } else {
                /* JSVAL path (no INT hint): same as P45 but with __jit_vt_ update
                 * in the miss path so warm-recompile can read the observed tags. */
#define _GF_DUP_OR_SKIP "if(js_likely(JS_VALUE_GET_TAG(_r)!=JS_TAG_INT)) JS_DupValue(ctx,_r);\n"
                if (top_borrowed) {
                    jit_buf_printf(cb,
                        "    { static JSJITICEntry2 _ic%d={{},0};\n"
                        "      JSValue _o=_tsv%d, _r;\n"
                        "      if (js_likely(JIT_IC_CHECK_FAST(_o,&_ic%d.e[0]))){\n"
                        "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                        "          _r=_pp[_ic%d.e[0].slot]; " _GF_DUP_OR_SKIP
                        "          _tsv%d=_r; _sp=%d; }\n"
                        "      else if (js_likely(_ic%d.n>=2&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[1]))){\n"
                        "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                        "          _r=_pp[_ic%d.e[1].slot]; " _GF_DUP_OR_SKIP
                        "          _tsv%d=_r; _sp=%d; }\n"
                        "      else if (js_likely(_ic%d.n>=3&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[2]))){\n"
                        "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                        "          _r=_pp[_ic%d.e[2].slot]; " _GF_DUP_OR_SKIP
                        "          _tsv%d=_r; _sp=%d; }\n"
                        "      else if (js_likely(_ic%d.n>=4&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[3]))){\n"
                        "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                        "          _r=_pp[_ic%d.e[3].slot]; " _GF_DUP_OR_SKIP
                        "          _tsv%d=_r; _sp=%d; }\n"
                        "      else { _r=_RT->get_prop(ctx,_o,(JSAtom)%uu);\n"
                        "             js_jit_ic2_fill_get(ctx,_o,(JSAtom)%uu,&_ic%d);\n"
                        _GF_VT_UPDATE
                        "             _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; } }\n",
                        pc,         /* _ic%d static */
                        d-1,        /* _o=_tsv%d */
                        pc,         /* JIT_IC_CHECK_FAST e[0] */
                        pc,         /* e[0].slot */
                        d-1, d,     /* _tsv%d=_r; _sp=%d */
                        pc, pc,     /* n>=2 && e[1] */
                        pc,         /* e[1].slot */
                        d-1, d,     /* _tsv%d=_r; _sp=%d */
                        pc, pc,     /* n>=3 && e[2] */
                        pc,         /* e[2].slot */
                        d-1, d,     /* _tsv%d=_r; _sp=%d */
                        pc, pc,     /* n>=4 && e[3] */
                        pc,         /* e[3].slot */
                        d-1, d,     /* _tsv%d=_r; _sp=%d */
                        atom, atom, pc, /* miss path */
                        (unsigned long long)bc_hash, gf_idx, pc, /* VT update */
                        d-1, d-1, d);   /* _sp=%d; _CHK; _tsv%d=_r; _sp=%d */
                } else {
                    jit_buf_printf(cb,
                        "    { static JSJITICEntry2 _ic%d={{},0};\n"
                        "      JSValue _o=_tsv%d, _r;\n"
                        "      if (js_likely(JIT_IC_CHECK_FAST(_o,&_ic%d.e[0]))){\n"
                        "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                        "          _r=_pp[_ic%d.e[0].slot]; " _GF_DUP_OR_SKIP
                        "          _FREE(_o); _tsv%d=_r; _sp=%d; }\n"
                        "      else if (js_likely(_ic%d.n>=2&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[1]))){\n"
                        "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                        "          _r=_pp[_ic%d.e[1].slot]; " _GF_DUP_OR_SKIP
                        "          _FREE(_o); _tsv%d=_r; _sp=%d; }\n"
                        "      else if (js_likely(_ic%d.n>=3&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[2]))){\n"
                        "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                        "          _r=_pp[_ic%d.e[2].slot]; " _GF_DUP_OR_SKIP
                        "          _FREE(_o); _tsv%d=_r; _sp=%d; }\n"
                        "      else if (js_likely(_ic%d.n>=4&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[3]))){\n"
                        "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                        "          _r=_pp[_ic%d.e[3].slot]; " _GF_DUP_OR_SKIP
                        "          _FREE(_o); _tsv%d=_r; _sp=%d; }\n"
                        "      else { _r=_RT->get_prop(ctx,_o,(JSAtom)%uu);\n"
                        "             js_jit_ic2_fill_get(ctx,_o,(JSAtom)%uu,&_ic%d);\n"
                        _GF_VT_UPDATE
                        "             _FREE(_o); _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; } }\n",
                        pc,         /* _ic%d static */
                        d-1,        /* _o=_tsv%d */
                        pc,         /* JIT_IC_CHECK_FAST e[0] */
                        pc,         /* e[0].slot */
                        d-1, d,     /* _FREE(_o); _tsv%d=_r; _sp=%d */
                        pc, pc,     /* n>=2 && e[1] */
                        pc,         /* e[1].slot */
                        d-1, d,     /* _FREE(_o); _tsv%d=_r; _sp=%d */
                        pc, pc,     /* n>=3 && e[2] */
                        pc,         /* e[2].slot */
                        d-1, d,     /* _FREE(_o); _tsv%d=_r; _sp=%d */
                        pc, pc,     /* n>=4 && e[3] */
                        pc,         /* e[3].slot */
                        d-1, d,     /* _FREE(_o); _tsv%d=_r; _sp=%d */
                        atom, atom, pc, /* miss path */
                        (unsigned long long)bc_hash, gf_idx, pc, /* VT update */
                        d-1, d-1, d);   /* _FREE; _sp=%d; _CHK; _tsv%d=_r; _sp=%d */
                }
#undef _GF_DUP_OR_SKIP
            }
#undef _GF_VT_UPDATE
            _borrowed_depth = -1; /* P38.1: borrow consumed by get_field */
            break;
        }
        case OP_get_field2: { /* keep object on stack; push result: depth d -> d+1 */
            uint32_t atom = bc_u32(&bc[pc+1]);
            _P94_ENSURE(d-1); /* P9.4: box typed obj slot (defensive) */
            /* P11.1/P11.5: IC hit path inlined; _CHK in miss branch.
             * P43.2: quadrimorphic — four shape slots.
             * P37.3: get_field2 keeps the object on stack (d→d+1), no _FREE(_o). */
            /* P39.2 fix: read prop array from current object, not IC. */
            jit_buf_printf(cb,
                "    { static JSJITICEntry2 _ic%d={{},0};\n"
                "      JSValue _r;\n"
                "      if (js_likely(JIT_IC_CHECK_FAST(_tsv%d,&_ic%d.e[0]))){\n"
                "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_tsv%d)+JIT_OBJ_PROP_OFF);\n"
                "          _r=_pp[_ic%d.e[0].slot]; JS_DupValue(ctx,_r);\n"
                "          _tsv%d=_r; _sp=%d; }\n"
                "      else if (js_likely(_ic%d.n>=2&&JIT_IC_CHECK_FAST(_tsv%d,&_ic%d.e[1]))){\n"
                "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_tsv%d)+JIT_OBJ_PROP_OFF);\n"
                "          _r=_pp[_ic%d.e[1].slot]; JS_DupValue(ctx,_r);\n"
                "          _tsv%d=_r; _sp=%d; }\n"
                "      else if (js_likely(_ic%d.n>=3&&JIT_IC_CHECK_FAST(_tsv%d,&_ic%d.e[2]))){\n"
                "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_tsv%d)+JIT_OBJ_PROP_OFF);\n"
                "          _r=_pp[_ic%d.e[2].slot]; JS_DupValue(ctx,_r);\n"
                "          _tsv%d=_r; _sp=%d; }\n"
                "      else if (js_likely(_ic%d.n>=4&&JIT_IC_CHECK_FAST(_tsv%d,&_ic%d.e[3]))){\n"
                "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_tsv%d)+JIT_OBJ_PROP_OFF);\n"
                "          _r=_pp[_ic%d.e[3].slot]; JS_DupValue(ctx,_r);\n"
                "          _tsv%d=_r; _sp=%d; }\n"
                "      else { _r=_RT->get_prop(ctx,_tsv%d,(JSAtom)%uu);\n"
                "             js_jit_ic2_fill_get(ctx,_tsv%d,(JSAtom)%uu,&_ic%d);\n"
                "             _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; } }\n",
                pc,          /* _ic%d */
                d-1, pc,     /* _tsv%d (IC check), e[0] */
                d-1,         /* _tsv%d (prop_arr read) */
                pc,          /* e[0].slot */
                d, d+1,      /* _tsv%d=_r; _sp=%d */
                pc, d-1, pc, /* n>=2 && _tsv%d, e[1] */
                d-1,         /* _tsv%d (prop_arr read) */
                pc,          /* e[1].slot */
                d, d+1,      /* _tsv%d=_r; _sp=%d */
                pc, d-1, pc, /* n>=3 && _tsv%d, e[2] */
                d-1,         /* _tsv%d (prop_arr read) */
                pc,          /* e[2].slot */
                d, d+1,      /* _tsv%d=_r; _sp=%d */
                pc, d-1, pc, /* n>=4 && _tsv%d, e[3] */
                d-1,         /* _tsv%d (prop_arr read) */
                pc,          /* e[3].slot */
                d, d+1,      /* _tsv%d=_r; _sp=%d */
                d-1, atom,   /* miss get_prop(_tsv%d, atom) */
                d-1, atom, pc, /* ic2_fill_get */
                d, d, d+1);    /* _sp=%d; _CHK; _tsv%d=_r; _sp=%d */
            break;
        }
        case OP_put_field: { /* pop val, pop obj; depth d -> d-2 */
            uint32_t atom = bc_u32(&bc[pc+1]);
            /* P48: warm INT fast path — skip boxing when:
             *   (a) vt_hint says value has always been JS_TAG_INT at runtime, AND
             *   (b) gen_state confirms val slot is INT (no overflow occurred).
             * When gen_state==INT, _ti[d-1] already holds a valid int32 (any overflow
             * would have already triggered a JIT exit).  Skip _P94_ENSURE + _tsv write. */
            int _p48_use_int = (vt_hints
                                && (n_gf + n_ae + pf_idx) < (n_gf + n_ae + n_pf)
                                && vt_hints[n_gf + n_ae + pf_idx] == 0 /* JS_TAG_INT */
                                && gen_sp > (d-1) && gen_st[d-1] == JIT_T_INT);
            if (!_p48_use_int)
                _P94_ENSURE(d-1); /* P9.4: box typed val slot before use as JSValue */
            /* P11.1: IC write hit path inlined — no call to js_jit_ic_write.
             * P37.4: bimorphic — two shape slots for write IC.
             * P39.2: use cached prop_arr from IC entry instead of obj→prop dereference.
             * P39.3: if obj was borrowed (get_loc skipped DupValue), skip _FREE(_o). */
            int _pf_borrowed = (borrowed_depth_snap == d-2);   /* P39.3 */

            /* Emit opening: IC entry decl + _v initialisation.
             * Warm INT: use _ti[d-1] directly (already valid int32).
             * Cold: use boxed _tsv[d-1] and record write-value tag for warm recompile. */
            if (_p48_use_int) {
                jit_buf_printf(cb,
                    "    { static JSJITICEntry2 _ic%d={{},0};\n"
                    "      JSValue _v=JS_MKVAL(JS_TAG_INT,(int32_t)_ti%d), _o=_tsv%d; _sp=%d; int _ret;\n",
                    pc, d-1, d-2, d-2);
            } else {
                jit_buf_printf(cb,
                    "    { static JSJITICEntry2 _ic%d={{},0};\n"
                    "      JSValue _v=_tsv%d, _o=_tsv%d; _sp=%d; int _ret;\n"
                    "      __jit_vt_%016llx[%d]=(uint8_t)JS_VALUE_GET_TAG(_v);\n",
                    pc, d-1, d-2, d-2,
                    (unsigned long long)bc_hash, n_gf + n_ae + pf_idx);
            }
            /* Emit IC body: 4 shape-matching entries + miss path.
             * P39.2 fix + P43.2: read prop array from current object, not IC. */
            jit_buf_printf(cb,
                "      if (js_likely(JIT_IC_CHECK_FAST(_o,&_ic%d.e[0]))){\n"
                "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                "          JSValue _old=_pp[_ic%d.e[0].slot]; _pp[_ic%d.e[0].slot]=_v;\n"
                "          JS_FreeValue(ctx,_old); _ret=0;}\n"
                "      else if (js_likely(_ic%d.n>=2&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[1]))){\n"
                "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                "          JSValue _old=_pp[_ic%d.e[1].slot]; _pp[_ic%d.e[1].slot]=_v;\n"
                "          JS_FreeValue(ctx,_old); _ret=0;}\n"
                "      else if (js_likely(_ic%d.n>=3&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[2]))){\n"
                "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                "          JSValue _old=_pp[_ic%d.e[2].slot]; _pp[_ic%d.e[2].slot]=_v;\n"
                "          JS_FreeValue(ctx,_old); _ret=0;}\n"
                "      else if (js_likely(_ic%d.n>=4&&JIT_IC_CHECK_FAST(_o,&_ic%d.e[3]))){\n"
                "          JSValue *_pp=(JSValue*)*(void**)((char*)JS_VALUE_GET_PTR(_o)+JIT_OBJ_PROP_OFF);\n"
                "          JSValue _old=_pp[_ic%d.e[3].slot]; _pp[_ic%d.e[3].slot]=_v;\n"
                "          JS_FreeValue(ctx,_old); _ret=0;}\n"
                "      else { _ret=_RT->set_prop(ctx,_o,(JSAtom)%uu,_v);\n"
                "             js_jit_ic2_fill_put(ctx,_o,(JSAtom)%uu,&_ic%d); }\n",
                pc,            /* JIT_IC_CHECK_FAST e[0] */
                pc, pc,        /* e[0].slot (old), e[0].slot (new) */
                pc, pc,        /* n>=2 && e[1] */
                pc, pc,        /* e[1].slot (old), e[1].slot (new) */
                pc, pc,        /* n>=3 && e[2] */
                pc, pc,        /* e[2].slot (old), e[2].slot (new) */
                pc, pc,        /* n>=4 && e[3] */
                pc, pc,        /* e[3].slot (old), e[3].slot (new) */
                atom, atom, pc); /* miss path */
            /* Emit closing: obj free (non-borrowed only) + error check. */
            if (_pf_borrowed)
                jit_buf_str(cb, "      if(_ret<0) goto _ex; }\n"); /* no _FREE(_o) — borrowed */
            else
                jit_buf_str(cb, "      _FREE(_o); if(_ret<0) goto _ex; }\n");
            _borrowed_depth = -1; /* P39.3: borrow consumed by put_field */
            break;
        }
        /* P11.4: dense array element fast path — fully inlined.
         * Guard: obj is JS_TAG_OBJECT && idx is JS_TAG_INT
         *        && class_id == JIT_CLASS_ARRAY && idx < u.array.count.
         * Fast path: direct JSValue* read/write from u.array.u.values[],
         *   no function call, no _CHK (value is never JS_EXCEPTION).
         * Slow path: _RT->get/set_array_el goes through JS_ValueToAtom + GetProperty.
         * P9.4/P9.2: get_array_el: box typed idx, pop idx(_tsv{d-1}),
         *   pop obj(_tsv{d-2}), push result(_tsv{d-2}); depth d->d-1 */
        /* P11.4: dense array element fast path — fully inlined.
         * Guard: obj is JS_TAG_OBJECT && idx is JS_TAG_INT
         *        && class_id == JIT_CLASS_ARRAY && idx < u.array.count.
         * Fast path: direct JSValue* read/write from u.array.u.values[],
         *   no function call, no _CHK (value is never JS_EXCEPTION).
         * Slow path: _RT->get/set_array_el goes through JS_ValueToAtom + GetProperty.
         * P9.4/P9.2: get_array_el: box typed idx slot, pop idx(_tsv{d-1}),
         *   pop obj(_tsv{d-2}), push result(_tsv{d-2}); depth d->d-1 */
        case OP_get_array_el:
        {
            /* P38.2: detect typed index before boxing */
            int _idx_typed = (gen_sp > (d-1) && gen_st[d-1] == JIT_T_INT);
            int _obj_borrowed = (borrowed_depth_snap == d-2);   /* P38.1 */
            _borrowed_depth = -1; /* consume borrow */

            /* P46: INT element hint — speculative extract for warm recompile.
             * Only applies when index is already a native INT (_idx_typed). */
            int _p46_use_int = (_idx_typed && vt_hints
                                && (n_gf + ae_idx) < (n_gf + n_ae)
                                && vt_hints[n_gf + ae_idx] == 0);

            if (_idx_typed) {
                /* P38.2: Index is a native int64_t _ti{d-1}; no boxing or tag check.
                 * Object may or may not be borrowed (P38.1). */
                _P94_ENSURE(d-2);  /* box obj slot if somehow typed (defensive) */
                const char *_o_free = _obj_borrowed ? "" : "_FREE(_o);";
                if (_p46_use_int) {
                    /* P46: INT hint — speculative INT extract; no DupValue needed.
                     * Fast path: reads element, checks tag is INT, stores to _ti{d-2}.
                     *   If element is not INT (type change after warm compile), falls
                     *   through to slow path instead of extracting garbage.
                     * Slow path: calls get_array_el, checks tag; if INT extract else free+0. */
                    jit_buf_printf(cb,
                        "    { JSValue _o=_tsv%d; JSValue _r;\n"
                        "      if(js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT)){\n"
                        "        char *_op=(char*)JS_VALUE_GET_PTR(_o);\n"
                        "        uint32_t _ai=(uint32_t)_ti%d;\n"
                        "        if(js_likely(*(uint16_t*)(_op+JIT_OBJ_CLASSID_OFF)==JIT_CLASS_ARRAY\n"
                        "                   &&_ai<(uint32_t)*(int*)(_op+JIT_ARR_COUNT_OFF))){\n"
                        "          _r=(*(JSValue**)(_op+JIT_ARR_VALUES_OFF))[_ai];\n"
                        "          if(js_likely(JS_VALUE_GET_TAG(_r)==JS_TAG_INT)) {\n"
                        "            _ti%d=(int64_t)JS_VALUE_GET_INT(_r);\n"
                        "            %s _sp=%d; goto _aok%d;}}}\n"
                        "      { int64_t _iv%d=_ti%d;\n"
                        "        JSValue _idx=((int64_t)(int32_t)_iv%d==_iv%d)\n"
                        "            ?JS_MKVAL(JS_TAG_INT,(int32_t)_iv%d)\n"
                        "            :JS_NewFloat64(ctx,(double)_iv%d);\n"
                        "        _r=_RT->get_array_el(ctx,_o,_idx);\n"
                        "        %s _sp=%d; _CHK(_r);\n"
                        "        _ti%d=(JS_VALUE_GET_TAG(_r)==JS_TAG_INT)?(int64_t)JS_VALUE_GET_INT(_r):(JS_FreeValue(ctx,_r),0LL);\n"
                        "        _sp=%d; }\n"
                        "      _aok%d:; }\n",
                        d-2,                                    /* _o=_tsv{d-2} */
                        d-1,                                    /* _ai=(uint32_t)_ti{d-1} */
                        d-2,                                    /* fast: _ti{d-2}=INT */
                        _o_free, d-1, pc,                       /* fast: free,sp,goto */
                        pc, d-1, pc, pc, pc, pc,                /* slow: box _ti{d-1} → _idx */
                        _o_free, d-2,                           /* slow: free,sp */
                        d-2,                                    /* slow: _ti{d-2}=speculative INT */
                        d-1,                                    /* slow: _sp=d-1 */
                        pc);                                    /* _aok label */
                } else {
                    jit_buf_printf(cb,
                        "    { JSValue _o=_tsv%d; JSValue _r;\n"
                        "      if(js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT)){\n"
                        "        char *_op=(char*)JS_VALUE_GET_PTR(_o);\n"
                        "        uint32_t _ai=(uint32_t)_ti%d;\n"
                        "        if(js_likely(*(uint16_t*)(_op+JIT_OBJ_CLASSID_OFF)==JIT_CLASS_ARRAY\n"
                        "                   &&_ai<(uint32_t)*(int*)(_op+JIT_ARR_COUNT_OFF))){\n"
                        "          _r=(*(JSValue**)(_op+JIT_ARR_VALUES_OFF))[_ai];\n"
                        "          __jit_vt_%016llx[%d]=(uint8_t)JS_VALUE_GET_TAG(_r);\n"
                        "          JS_DupValue(ctx,_r);\n"
                        "          %s _sp=%d; _tsv%d=_r; _sp=%d; goto _aok%d;}}\n"
                        "      { int64_t _iv%d=_ti%d;\n"
                        "        JSValue _idx=((int64_t)(int32_t)_iv%d==_iv%d)\n"
                        "            ?JS_MKVAL(JS_TAG_INT,(int32_t)_iv%d)\n"
                        "            :JS_NewFloat64(ctx,(double)_iv%d);\n"
                        "        _r=_RT->get_array_el(ctx,_o,_idx);\n"
                        "        __jit_vt_%016llx[%d]=(uint8_t)JS_VALUE_GET_TAG(_r);\n"
                        "        %s _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n"
                        "      _aok%d:; }\n",
                        d-2,                                    /* _o=_tsv{d-2} */
                        d-1,                                    /* _ai=(uint32_t)_ti{d-1} */
                        (unsigned long long)bc_hash, n_gf + ae_idx,  /* vt update fast */
                        _o_free, d-2, d-2, d-1, pc,            /* fast: free,sp,tsv,sp,goto */
                        pc, d-1, pc, pc, pc, pc,                /* slow: box _ti{d-1} → _idx */
                        (unsigned long long)bc_hash, n_gf + ae_idx,  /* vt update slow */
                        _o_free, d-2, d-2, d-1, pc);            /* slow: free,sp,chk,tsv,sp,label */
                }
            } else {
                /* Not typed index: box the index slot, then use P38.1 borrow for obj */
                _P94_ENSURE(d-1); /* P9.4: box typed idx slot before index check */
                if (_obj_borrowed) {
                    /* P38.1: arr was pushed without DupValue — skip _FREE(_o) */
                    jit_buf_printf(cb,
                        "    { JSValue _idx=_tsv%d,_o=_tsv%d; JSValue _r;\n"
                        "      if(js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT"
                                       "&&JS_VALUE_GET_TAG(_idx)==JS_TAG_INT)){\n"
                        "        char *_op=(char*)JS_VALUE_GET_PTR(_o);\n"
                        "        uint32_t _ai=(uint32_t)JS_VALUE_GET_INT(_idx);\n"
                        "        if(js_likely(*(uint16_t*)(_op+JIT_OBJ_CLASSID_OFF)==JIT_CLASS_ARRAY\n"
                        "                   &&_ai<(uint32_t)*(int*)(_op+JIT_ARR_COUNT_OFF))){\n"
                        "          _r=(*(JSValue**)(_op+JIT_ARR_VALUES_OFF))[_ai];\n"
                        "          __jit_vt_%016llx[%d]=(uint8_t)JS_VALUE_GET_TAG(_r);\n"
                        "          JS_DupValue(ctx,_r);\n"
                        "          _FREE(_idx); _sp=%d; _tsv%d=_r; _sp=%d;\n"  /* no _FREE(_o) */
                        "          goto _aok%d;}}\n"
                        "      _r=_RT->get_array_el(ctx,_o,_idx);\n"
                        "      __jit_vt_%016llx[%d]=(uint8_t)JS_VALUE_GET_TAG(_r);\n"
                        "      _FREE(_idx); _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d;\n" /* no _FREE(_o) */
                        "      _aok%d:; }\n",
                        d-1, d-2,
                        (unsigned long long)bc_hash, n_gf + ae_idx,  /* vt update fast */
                        d-2, d-2, d-1, pc,  /* fast path: _sp, _tsv, _sp, goto */
                        (unsigned long long)bc_hash, n_gf + ae_idx,  /* vt update slow */
                        d-2, d-2, d-1, pc); /* slow path: _sp, _tsv, _sp, label */
                } else {
                    /* Original path: both _FREE(_o) and _FREE(_idx) */
                    jit_buf_printf(cb,
                        "    { JSValue _idx=_tsv%d,_o=_tsv%d; JSValue _r;\n"
                        "      if(js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT"
                                       "&&JS_VALUE_GET_TAG(_idx)==JS_TAG_INT)){\n"
                        "        char *_op=(char*)JS_VALUE_GET_PTR(_o);\n"
                        "        uint32_t _ai=(uint32_t)JS_VALUE_GET_INT(_idx);\n"
                        "        if(js_likely(*(uint16_t*)(_op+JIT_OBJ_CLASSID_OFF)==JIT_CLASS_ARRAY\n"
                        "                   &&_ai<(uint32_t)*(int*)(_op+JIT_ARR_COUNT_OFF))){\n"
                        "          _r=(*(JSValue**)(_op+JIT_ARR_VALUES_OFF))[_ai];\n"
                        "          __jit_vt_%016llx[%d]=(uint8_t)JS_VALUE_GET_TAG(_r);\n"
                        "          JS_DupValue(ctx,_r);\n"
                        "          _FREE(_o);_FREE(_idx); _sp=%d; _tsv%d=_r; _sp=%d;\n"
                        "          goto _aok%d;}}\n"
                        "      _r=_RT->get_array_el(ctx,_o,_idx);\n"
                        "      __jit_vt_%016llx[%d]=(uint8_t)JS_VALUE_GET_TAG(_r);\n"
                        "      _FREE(_o);_FREE(_idx); _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d;\n"
                        "      _aok%d:; }\n",
                        d-1, d-2,
                        (unsigned long long)bc_hash, n_gf + ae_idx,  /* vt update fast */
                        d-2, d-2, d-1, pc,  /* fast path: _sp, _tsv, _sp, goto label */
                        (unsigned long long)bc_hash, n_gf + ae_idx,  /* vt update slow */
                        d-2, d-2, d-1, pc); /* slow path: _sp, _tsv, _sp, label */
                }
            }
            break;
        }
        /* P11.4: put_array_el inlined similarly.
         * P38.2: typed index fast path for put_array_el.
         * P50: warm INT write — skip _P94_ENSURE and use _ti[d-1] directly when
         *      vt_hint says value is always INT and gen_state confirms it.
         * P9.4/P9.2: box typed slots, pop v(_tsv{d-1}), idx(_tsv{d-2}),
         *   obj(_tsv{d-3}); depth d->d-3 */
        case OP_put_array_el:
        {
            int _pidx_typed = (gen_sp > (d-2) && gen_st[d-2] == JIT_T_INT);
            _borrowed_depth = -1;
            /* P50: warm INT fast path — skip boxing write value when hint=INT
             * and gen_state confirms val slot is INT (overflow exits already done). */
            int _pa_base = n_gf + n_ae + n_pf + n_vr;
            int _p50_use_int = (vt_hints
                                && (_pa_base + pa_idx) < (_pa_base + n_pa)
                                && vt_hints[_pa_base + pa_idx] == 0 /* JS_TAG_INT */
                                && gen_sp > (d-1) && gen_st[d-1] == JIT_T_INT);
            _P94_ENSURE(d-3); /* P9.4: box typed obj slot (unlikely but safe) */
            /* P38.2: skip _P94_ENSURE(d-2) when index is a typed int */
            if (!_pidx_typed)
                _P94_ENSURE(d-2); /* P9.4: box typed idx slot before index check */
            if (!_p50_use_int)
                _P94_ENSURE(d-1); /* P9.4: box typed val slot before use as JSValue */

            if (_pidx_typed) {
                /* Typed index (_ti{d-2}) path: fast array bounds check, no idx boxing. */
                if (_p50_use_int) {
                    /* P50 warm INT: write value directly from _ti{d-1}. */
                    jit_buf_printf(cb,
                        "    { JSValue _v=JS_MKVAL(JS_TAG_INT,(int32_t)_ti%d),_o=_tsv%d; _sp=%d;\n"
                        "      if(js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT)){\n"
                        "        char *_op=(char*)JS_VALUE_GET_PTR(_o);\n"
                        "        uint32_t _ai=(uint32_t)_ti%d;\n"
                        "        if(js_likely(*(uint16_t*)(_op+JIT_OBJ_CLASSID_OFF)==JIT_CLASS_ARRAY\n"
                        "                   &&_ai<(uint32_t)*(int*)(_op+JIT_ARR_COUNT_OFF))){\n"
                        "          JSValue *_vp=*(JSValue**)(_op+JIT_ARR_VALUES_OFF);\n"
                        "          JSValue _old=_vp[_ai]; _vp[_ai]=_v; JS_FreeValue(ctx,_old);\n"
                        "          _FREE(_o); goto _aok%d;}}\n"
                        "      { int64_t _iv%d=_ti%d;\n"
                        "        JSValue _idx=((int64_t)(int32_t)_iv%d==_iv%d)\n"
                        "            ?JS_MKVAL(JS_TAG_INT,(int32_t)_iv%d)\n"
                        "            :JS_NewFloat64(ctx,(double)_iv%d);\n"
                        "        int _ret=_RT->set_array_el(ctx,_o,_idx,_v);\n"
                        "        _FREE(_o); if(_ret<0) goto _ex; }\n"
                        "      _aok%d:; }\n",
                        d-1, d-3, d-3,          /* _v=JS_MKVAL(_ti{d-1}), _o=_tsv{d-3}, _sp */
                        d-2,                    /* _ai=(uint32_t)_ti{d-2} */
                        pc,                     /* goto _aok */
                        pc, d-2, pc, pc, pc, pc, /* slow: box _ti{d-2} → _idx */
                        pc);                    /* _aok label */
                } else {
                    /* Cold/JSVAL: observe write-value tag for warm recompile. */
                    jit_buf_printf(cb,
                        "    { JSValue _v=_tsv%d,_o=_tsv%d; _sp=%d;\n"
                        "      __jit_vt_%016llx[%d]=(uint8_t)JS_VALUE_GET_TAG(_v);\n"
                        "      if(js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT)){\n"
                        "        char *_op=(char*)JS_VALUE_GET_PTR(_o);\n"
                        "        uint32_t _ai=(uint32_t)_ti%d;\n"
                        "        if(js_likely(*(uint16_t*)(_op+JIT_OBJ_CLASSID_OFF)==JIT_CLASS_ARRAY\n"
                        "                   &&_ai<(uint32_t)*(int*)(_op+JIT_ARR_COUNT_OFF))){\n"
                        "          JSValue *_vp=*(JSValue**)(_op+JIT_ARR_VALUES_OFF);\n"
                        "          JSValue _old=_vp[_ai]; _vp[_ai]=_v; JS_FreeValue(ctx,_old);\n"
                        "          _FREE(_o); goto _aok%d;}}\n"
                        "      { int64_t _iv%d=_ti%d;\n"
                        "        JSValue _idx=((int64_t)(int32_t)_iv%d==_iv%d)\n"
                        "            ?JS_MKVAL(JS_TAG_INT,(int32_t)_iv%d)\n"
                        "            :JS_NewFloat64(ctx,(double)_iv%d);\n"
                        "        int _ret=_RT->set_array_el(ctx,_o,_idx,_v);\n"
                        "        _FREE(_o); if(_ret<0) goto _ex; }\n"
                        "      _aok%d:; }\n",
                        d-1, d-3, d-3,                              /* _v, _o, _sp */
                        (unsigned long long)bc_hash, _pa_base+pa_idx, /* vt observe */
                        d-2,                                        /* _ai */
                        pc,                                         /* goto _aok */
                        pc, d-2, pc, pc, pc, pc,                    /* slow: box idx */
                        pc);                                        /* _aok label */
                }
            } else {
                /* JSVAL index path: tag-check idx at runtime. */
                if (_p50_use_int) {
                    /* P50 warm INT: write value directly from _ti{d-1}. */
                    jit_buf_printf(cb,
                        "    { JSValue _v=JS_MKVAL(JS_TAG_INT,(int32_t)_ti%d),_idx=_tsv%d,_o=_tsv%d; _sp=%d;\n"
                        "      if(js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT"
                                       "&&JS_VALUE_GET_TAG(_idx)==JS_TAG_INT)){\n"
                        "        char *_op=(char*)JS_VALUE_GET_PTR(_o);\n"
                        "        uint32_t _ai=(uint32_t)JS_VALUE_GET_INT(_idx);\n"
                        "        if(js_likely(*(uint16_t*)(_op+JIT_OBJ_CLASSID_OFF)==JIT_CLASS_ARRAY\n"
                        "                   &&_ai<(uint32_t)*(int*)(_op+JIT_ARR_COUNT_OFF))){\n"
                        "          JSValue *_vp=*(JSValue**)(_op+JIT_ARR_VALUES_OFF);\n"
                        "          JSValue _old=_vp[_ai]; _vp[_ai]=_v; JS_FreeValue(ctx,_old);\n"
                        "          _FREE(_o);_FREE(_idx); goto _aok%d;}}\n"
                        "      { int _ret=_RT->set_array_el(ctx,_o,_idx,_v);\n"
                        "        _FREE(_o);_FREE(_idx); if(_ret<0) goto _ex; }\n"
                        "      _aok%d:; }\n",
                        d-1, d-2, d-3, d-3, pc, pc);
                } else {
                    /* Cold/JSVAL: observe write-value tag. */
                    jit_buf_printf(cb,
                        "    { JSValue _v=_tsv%d,_idx=_tsv%d,_o=_tsv%d; _sp=%d;\n"
                        "      __jit_vt_%016llx[%d]=(uint8_t)JS_VALUE_GET_TAG(_v);\n"
                        "      if(js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT"
                                       "&&JS_VALUE_GET_TAG(_idx)==JS_TAG_INT)){\n"
                        "        char *_op=(char*)JS_VALUE_GET_PTR(_o);\n"
                        "        uint32_t _ai=(uint32_t)JS_VALUE_GET_INT(_idx);\n"
                        "        if(js_likely(*(uint16_t*)(_op+JIT_OBJ_CLASSID_OFF)==JIT_CLASS_ARRAY\n"
                        "                   &&_ai<(uint32_t)*(int*)(_op+JIT_ARR_COUNT_OFF))){\n"
                        "          JSValue *_vp=*(JSValue**)(_op+JIT_ARR_VALUES_OFF);\n"
                        "          JSValue _old=_vp[_ai]; _vp[_ai]=_v; JS_FreeValue(ctx,_old);\n"
                        "          _FREE(_o);_FREE(_idx); goto _aok%d;}}\n"
                        "      { int _ret=_RT->set_array_el(ctx,_o,_idx,_v);\n"
                        "        _FREE(_o);_FREE(_idx); if(_ret<0) goto _ex; }\n"
                        "      _aok%d:; }\n",
                        d-1, d-2, d-3, d-3,
                        (unsigned long long)bc_hash, _pa_base+pa_idx, /* vt observe */
                        pc, pc);
                }
            }
            break;
        }
        /* P38.3: get_length — inline fast path for JS Arrays; fall back to get_prop.
         * Result is always stored in typed int slot _ti{d-1} (no change from P11.8).
         * Fast path: class_id == JIT_CLASS_ARRAY → read prop[0].u.value (= length).
         *   NOTE: u.array.count is the internal allocated-element count, NOT the JS
         *   length property.  new Array(n) sets length=n but count=0.  We must read
         *   prop[0] (the length property value stored by set_array_length) instead.
         * Slow path: any other object → _RT->get_prop (string, proxy, etc.). */
        case OP_get_length:
            _P94_ENSURE(d-1); /* box typed obj slot before JSValue use */
            _borrowed_depth = -1;
            jit_buf_printf(cb,
                "    { JSValue _o=_tsv%d;\n"
                "      if(js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT)){\n"
                "        char *_op=(char*)JS_VALUE_GET_PTR(_o);\n"
                "        if(js_likely(*(uint16_t*)(_op+JIT_OBJ_CLASSID_OFF)==JIT_CLASS_ARRAY)){\n"
                "          { JSValue *_lpp=*(JSValue**)(_op+JIT_OBJ_PROP_OFF);\n"
                "            _ti%d=(JS_VALUE_GET_TAG(_lpp[0])==JS_TAG_INT)\n"
                "                 ?(int64_t)(uint32_t)JS_VALUE_GET_INT(_lpp[0])\n"
                "                 :(int64_t)JS_VALUE_GET_FLOAT64(_lpp[0]); }\n"
                "          _FREE(_o); _sp=%d; goto _lenok%d; }}\n"
                "      { JSValue _r=_RT->get_prop(ctx,_o,(JSAtom)%uu);\n"
                "        _sp=%d; _CHK(_r); _FREE(_o);\n"
                "        _ti%d=(JS_VALUE_GET_TAG(_r)==JS_TAG_INT)\n"
                "             ?(int64_t)JS_VALUE_GET_INT(_r)\n"
                "             :(int64_t)JS_VALUE_GET_FLOAT64(_r);\n"
                "        _FREE(_r); }\n"
                "      _lenok%d:; _sp=%d; }\n",
                d-1,                          /* _o=_tsv{d-1} */
                d-1,                          /* _ti{d-1} = length from prop[0] (fast path) */
                d, pc,                        /* _sp after fast; goto label */
                (unsigned)JS_ATOM_length,     /* atom for slow path */
                d-1,                          /* _sp before _CHK */
                d-1,                          /* _ti{d-1} (slow path) */
                pc, d);                       /* label; _sp after slow path */
            break;

        /* ---- Function calls ---- */
        /* P9.2: calls use named _tsv{} slots.
         * stack layout at depth d:
         *   OP_call N:          func=_tsv{d-N-1}, args=_tsv{d-N}.._tsv{d-1}
         *   OP_call_method N:   this=_tsv{d-N-2}, func=_tsv{d-N-1}, args=_tsv{d-N}.._tsv{d-1}
         *   OP_call_constructor N: ctor=_tsv{d-N-2}, new_target=_tsv{d-N-1}, args=_tsv{d-N}.._tsv{d-1}
         * Since _tsv are separate C variables (not an array), we build a temp array
         * in the generated C for passing args to the call. */
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
            /* func at _tsv{d-nargs-1}, args at _tsv{d-nargs}.._tsv{d-1}
             * result at _tsv{d-nargs-1}, new depth = d-nargs */
            {
                int func_slot = gen_sp - 1 - nargs;
                int is_self = (func_slot >= 0 && gen_st[func_slot] == JIT_T_SELF_FUNC);
                /* P10.3: guarded direct call to known JIT callee */
                int is_jit  = (func_slot >= 0 && gen_st[func_slot] == JIT_T_JIT_FUNC && gen_hsh);
                uint64_t jit_callee_hash = is_jit ? gen_hsh[func_slot] : 0;
                int fslot = d - nargs - 1; /* absolute slot index of func */
                /* P9.4: box any typed arg slots before building the args array */
                for (int _aj = 0; _aj < nargs; _aj++)
                    _P94_ENSURE(d-nargs+_aj);
                /* P10.3: emit file-scope extern declarations BEFORE the inner
                 * { } block so they remain visible to subsequent call sites
                 * within the same function body. */
                if (is_jit) {
                    int _p103_already = 0;
                    for (int _pi = 0; _pi < p103_nexterns; _pi++)
                        if (p103_externs[_pi] == jit_callee_hash) { _p103_already = 1; break; }
                    if (!_p103_already && p103_nexterns < 16) {
                        jit_buf_printf(cb,
                            "extern JSValue __jit_f_%016llx"
                            "(JSContext*,JSValue,int,JSValue*,JSValue*,JSVarRef**);\n",
                            (unsigned long long)jit_callee_hash);
                        p103_externs[p103_nexterns++] = jit_callee_hash;
                    }
                    if (!p103_cae_declared) {
                        jit_buf_str(cb,
                            "extern int js_jit_check_and_extract"
                            "(JSValue,JSJITFunc,JSValue**,JSVarRef***);\n");
                        p103_cae_declared = 1;
                    }
                }
                jit_buf_printf(cb, "    { JSValue _f=_tsv%d;\n", fslot);
                /* Build args array */
                if (nargs > 0) {
                    jit_buf_printf(cb, "      JSValue _ca%d[%d]={", pc, nargs);
                    for (int _aj = 0; _aj < nargs; _aj++) {
                        if (_aj) jit_buf_str(cb, ",");
                        jit_buf_printf(cb, "_tsv%d", d-nargs+_aj);
                    }
                    jit_buf_str(cb, "};\n");
                }
                if (is_self) {
                    /* P8.2: self-recursive direct call */
                    jit_buf_str(cb, "      if(_RT->poll_interrupts(ctx)) goto _ex;\n");
                    if (nargs > 0)
                        jit_buf_printf(cb,
                            "      JSValue _r=%s(ctx,JS_UNDEFINED,%d,_ca%d,cpool,var_refs);\n",
                            self_jit_sym, nargs, pc);
                    else
                        jit_buf_printf(cb,
                            "      JSValue _r=%s(ctx,JS_UNDEFINED,0,NULL,cpool,var_refs);\n",
                            self_jit_sym);
                } else if (is_jit) {
                    jit_buf_str(cb, "      JSValue *_dc; JSVarRef **_dv; JSValue _r;\n");
                    jit_buf_printf(cb,
                        "      if(js_jit_check_and_extract(_f,(JSJITFunc)__jit_f_%016llx,&_dc,&_dv)){\n"
                        "        if(_RT->poll_interrupts(ctx)) goto _ex;\n",
                        (unsigned long long)jit_callee_hash);
                    if (nargs > 0)
                        jit_buf_printf(cb,
                            "        _r=__jit_f_%016llx(ctx,JS_UNDEFINED,%d,_ca%d,_dc,_dv);\n"
                            "      } else {\n"
                            "        _r=_RT->call(ctx,_f,JS_UNDEFINED,%d,_ca%d);\n      }\n",
                            (unsigned long long)jit_callee_hash, nargs, pc,
                            nargs, pc);
                    else
                        jit_buf_printf(cb,
                            "        _r=__jit_f_%016llx(ctx,JS_UNDEFINED,0,NULL,_dc,_dv);\n"
                            "      } else {\n"
                            "        _r=_RT->call(ctx,_f,JS_UNDEFINED,0,NULL);\n      }\n",
                            (unsigned long long)jit_callee_hash);
                } else {
                    /* P11.3: monomorphic call IC.
                     * Check: _fo == cached_func && func->function_bytecode == cached_bc.
                     * Hit + direct_jit: call JIT function via js_jit_ic_direct_call which
                     *   pads args to callee_arg_count (required because the callee's JIT
                     *   code may write-back to argv[i] for i < arg_count even when the
                     *   caller passes fewer args; also prevents double-free of caller args).
                     * Hit + no jit:    call through vtable (avoid identity re-check).
                     * Miss:            vtable call + fill IC for future hits.
                     * Note: void* instead of JSObject* — JSObject not in public header. */
                    if (!p103_cae_declared) {
                        jit_buf_str(cb,
                            "extern int js_jit_check_and_extract"
                            "(JSValue,JSJITFunc,JSValue**,JSVarRef***);\n");
                        p103_cae_declared = 1;
                    }
                    jit_buf_str(cb,
                        "extern JSValue js_jit_ic_direct_call"
                        "(JSContext*,JSValue,int,JSValue*,JSJITCallICEntry*,JSVarRef**);\n"
                        "extern JSValue js_jit_ic_fast_call"
                        "(JSContext*,JSValue,JSJITCallICEntry*);\n");
                    jit_buf_printf(cb,
                        /* P50: rt field added — cross-runtime ABA guard (NULL = cold).
                         * The IC fires only when _cic.rt matches the current runtime. */
                        "      static JSJITCallICEntry _cic%d={NULL,NULL,NULL,NULL,NULL,0,0,0,NULL};\n"
                        "      void *_fo=(JS_VALUE_GET_TAG(_f)==JS_TAG_OBJECT)"
                              "?JS_VALUE_GET_PTR(_f):NULL;\n"
                        "      void *_jrt_=*(void**)((char*)ctx+JIT_CTX_RT_OFF);\n"
                        "      JSValue _r;\n"
                        "      if(js_likely(_fo&&_fo==_cic%d.expected_func&&\n"
                        "                   _cic%d.rt==_jrt_&&\n"
                        "                   *(void**)((char*)_fo+JIT_FUNC_BC_OFF)"
                                            "==(void*)_cic%d.expected_bc)){\n"
                        /* Full ABA guard: check bc_hash to detect both single-ABA
                         * (new closure at same address, same bytecode) and double-ABA
                         * (bytecode AND jit_func pointers reused). bc_hash is set when
                         * the bytecode is compiled and is unique per bytecode function.
                         * A different bytecode at the same address will have a different
                         * hash, preventing wrong-callee direct calls in all cases. */
                        "        if(_cic%d.direct_jit&&\n"
                        "           *(uint64_t*)((char*)_cic%d.expected_bc"
                                                "+JIT_BC_BCHASH_OFF)==_cic%d.callee_bc_hash){\n"
                        "          if(_RT->poll_interrupts(ctx)) goto _ex;\n",
                        pc, pc, pc, pc, pc, pc, pc);
                    /* Use js_jit_ic_direct_call to pad args to callee_arg_count and dup them.
                     * This matches js_jit_call's padding so the callee's put_arg write-backs
                     * operate on a private copy, not the caller's stack slots.
                     *
                     * P41.2: for zero-arg calls where callee_is_fast == 1 (NORMAL closure,
                     * no HOME_OBJECT, no var_refs, arg_count==0), bypass all SF mutation
                     * and arg padding by calling direct_jit() via js_jit_ic_fast_call. */
                    if (nargs > 0)
                        jit_buf_printf(cb,
                            "          _r=js_jit_ic_direct_call(ctx,JS_UNDEFINED,%d,_ca%d,"
                                       "&_cic%d,"
                                       "*(JSVarRef***)((char*)_fo+JIT_FUNC_VARREFS_OFF));\n",
                            nargs, pc, pc);
                    else
                        jit_buf_printf(cb,
                            "          if(js_likely(_cic%d.callee_is_fast))\n"
                            "            _r=js_jit_ic_fast_call(ctx,JS_UNDEFINED,&_cic%d);\n"
                            "          else\n"
                            "            _r=js_jit_ic_direct_call(ctx,JS_UNDEFINED,0,NULL,"
                                         "&_cic%d,"
                                         "*(JSVarRef***)((char*)_fo+JIT_FUNC_VARREFS_OFF));\n",
                            pc, pc, pc);
                    if (nargs > 0)
                        jit_buf_printf(cb,
                            "        } else _r=_RT->call(ctx,_f,JS_UNDEFINED,%d,_ca%d);\n"
                            "      } else {\n"
                            "        _r=_RT->call(ctx,_f,JS_UNDEFINED,%d,_ca%d);\n"
                            /* P50: also refill when rt changed (stale IC from prev runtime) */
                            "        if(!_cic%d.expected_func||_cic%d.rt!=_jrt_)\n"
                            "          js_jit_callIC_fill(ctx,_f,&_cic%d);\n"
                            "      }\n",
                            nargs, pc, nargs, pc, pc, pc, pc);
                    else
                        jit_buf_printf(cb,
                            "        } else _r=_RT->call(ctx,_f,JS_UNDEFINED,0,NULL);\n"
                            "      } else {\n"
                            "        _r=_RT->call(ctx,_f,JS_UNDEFINED,0,NULL);\n"
                            /* P50: also refill when rt changed (stale IC from prev runtime) */
                            "        if(!_cic%d.expected_func||_cic%d.rt!=_jrt_)\n"
                            "          js_jit_callIC_fill(ctx,_f,&_cic%d);\n"
                            "      }\n",
                            pc, pc, pc);
                }
                /* Free args */
                for (int _aj = 0; _aj < nargs; _aj++)
                    jit_buf_printf(cb, "      _FREE(_tsv%d);\n", d-nargs+_aj);
                jit_buf_printf(cb,
                    "      _sp=%d; _FREE(_f);\n"
                    "      _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                    fslot, fslot, fslot+1);
            }
            break;
        }
        case OP_call_method: {
            int nargs = (int)bc_u16(&bc[pc+1]);
            /* stack: this(_tsv{d-N-2}) func(_tsv{d-N-1}) arg0.._N-1(_tsv{d-N}.._tsv{d-1})
             * result at _tsv{d-N-2}, new depth = d-N-1 */
            {
                int tslot = d - nargs - 2; /* this */
                int fslot = d - nargs - 1; /* func */
                /* P9.4: box any typed arg slots before building the args array */
                for (int _aj = 0; _aj < nargs; _aj++)
                    _P94_ENSURE(d-nargs+_aj);
                jit_buf_printf(cb,
                    "    { JSValue _f=_tsv%d;\n"
                    "      JSValue _t=_tsv%d;\n",
                    fslot, tslot);
                if (nargs > 0) {
                    jit_buf_printf(cb, "      JSValue _ca%d[%d]={", pc, nargs);
                    for (int _aj = 0; _aj < nargs; _aj++) {
                        if (_aj) jit_buf_str(cb, ",");
                        jit_buf_printf(cb, "_tsv%d", d-nargs+_aj);
                    }
                    jit_buf_str(cb, "};\n");
                }
                /* P11.3: method call IC — same structure as OP_call */
                jit_buf_str(cb,
                    "extern JSValue js_jit_ic_direct_call"
                    "(JSContext*,JSValue,int,JSValue*,JSJITCallICEntry*,JSVarRef**);\n"
                    "extern JSValue js_jit_ic_fast_call"
                    "(JSContext*,JSValue,JSJITCallICEntry*);\n");
                jit_buf_printf(cb,
                    /* P50: rt field — cross-runtime ABA guard (NULL = cold). */
                    "      static JSJITCallICEntry _cic%d={NULL,NULL,NULL,NULL,NULL,0,0,0,NULL};\n"
                    "      void *_fo=(JS_VALUE_GET_TAG(_f)==JS_TAG_OBJECT)"
                          "?JS_VALUE_GET_PTR(_f):NULL;\n"
                    "      void *_jrt_=*(void**)((char*)ctx+JIT_CTX_RT_OFF);\n"
                    "      JSValue _r;\n"
                    "      if(js_likely(_fo&&_fo==_cic%d.expected_func&&\n"
                    "                   _cic%d.rt==_jrt_&&\n"
                    "                   *(void**)((char*)_fo+JIT_FUNC_BC_OFF)"
                                        "==(void*)_cic%d.expected_bc)){\n"
                    /* Full ABA guard: same as OP_call — bc_hash check */
                    "        if(_cic%d.direct_jit&&\n"
                    "           *(uint64_t*)((char*)_cic%d.expected_bc"
                                            "+JIT_BC_BCHASH_OFF)==_cic%d.callee_bc_hash){\n"
                    "          if(_RT->poll_interrupts(ctx)) goto _ex;\n",
                    pc, pc, pc, pc, pc, pc, pc);
                /* Use js_jit_ic_direct_call for arg padding (same reason as OP_call).
                 * P41.2: for zero-arg method calls, use fast path when callee_is_fast. */
                if (nargs > 0)
                    jit_buf_printf(cb,
                        "          _r=js_jit_ic_direct_call(ctx,_t,%d,_ca%d,"
                                   "&_cic%d,"
                                   "*(JSVarRef***)((char*)_fo+JIT_FUNC_VARREFS_OFF));\n",
                        nargs, pc, pc);
                else
                    jit_buf_printf(cb,
                        "          if(js_likely(_cic%d.callee_is_fast))\n"
                        "            _r=js_jit_ic_fast_call(ctx,_t,&_cic%d);\n"
                        "          else\n"
                        "            _r=js_jit_ic_direct_call(ctx,_t,0,NULL,"
                                     "&_cic%d,"
                                     "*(JSVarRef***)((char*)_fo+JIT_FUNC_VARREFS_OFF));\n",
                        pc, pc, pc);
                if (nargs > 0)
                    jit_buf_printf(cb,
                        "        } else _r=_RT->call(ctx,_f,_t,%d,_ca%d);\n"
                        "      } else {\n"
                        "        _r=_RT->call(ctx,_f,_t,%d,_ca%d);\n"
                        /* P50: also refill when rt changed (stale IC from prev runtime) */
                        "        if(!_cic%d.expected_func||_cic%d.rt!=_jrt_)\n"
                        "          js_jit_callIC_fill(ctx,_f,&_cic%d);\n"
                        "      }\n",
                        nargs, pc, nargs, pc, pc, pc, pc);
                else
                    jit_buf_printf(cb,
                        "        } else _r=_RT->call(ctx,_f,_t,0,NULL);\n"
                        "      } else {\n"
                        "        _r=_RT->call(ctx,_f,_t,0,NULL);\n"
                        /* P50: also refill when rt changed (stale IC from prev runtime) */
                        "        if(!_cic%d.expected_func||_cic%d.rt!=_jrt_)\n"
                        "          js_jit_callIC_fill(ctx,_f,&_cic%d);\n"
                        "      }\n",
                        pc, pc, pc);
                for (int _aj = 0; _aj < nargs; _aj++)
                    jit_buf_printf(cb, "      _FREE(_tsv%d);\n", d-nargs+_aj);
                jit_buf_printf(cb,
                    "      _sp=%d; _FREE(_f); _FREE(_t);\n"
                    "      _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                    tslot, tslot, tslot+1);
            }
            break;
        }
        case OP_tail_call_method: {
            int nargs = (int)bc_u16(&bc[pc+1]);
            /* tail call: perform call and return directly */
            {
                int tslot = d - nargs - 2;
                int fslot = d - nargs - 1;
                /* P9.4: box any typed arg slots before building the args array */
                for (int _aj = 0; _aj < nargs; _aj++)
                    _P94_ENSURE(d-nargs+_aj);
                jit_buf_printf(cb,
                    "    { JSValue _f=_tsv%d;\n"
                    "      JSValue _t=_tsv%d;\n",
                    fslot, tslot);
                if (nargs > 0) {
                    jit_buf_printf(cb, "      JSValue _ca%d[%d]={", pc, nargs);
                    for (int _aj = 0; _aj < nargs; _aj++) {
                        if (_aj) jit_buf_str(cb, ",");
                        jit_buf_printf(cb, "_tsv%d", d-nargs+_aj);
                    }
                    jit_buf_str(cb, "};\n");
                    jit_buf_printf(cb,
                        "      JSValue _r=_RT->call(ctx,_f,_t,%d,_ca%d);\n",
                        nargs, pc);
                } else {
                    jit_buf_str(cb,
                        "      JSValue _r=_RT->call(ctx,_f,_t,0,NULL);\n");
                }
                for (int _aj = 0; _aj < nargs; _aj++)
                    jit_buf_printf(cb, "      _FREE(_tsv%d);\n", d-nargs+_aj);
                jit_buf_printf(cb,
                    "      _sp=%d; _FREE(_f); _FREE(_t);\n"
                    "      if(JS_VALUE_GET_TAG(_r)==JS_TAG_EXCEPTION) goto _ex;\n",
                    tslot);
            }
            /* P13.7: heap-promote var_refs before _cap_buf goes out of scope */
            if (sr->has_fclosure) {
                if (var_ref_count > 0)
                    jit_buf_printf(cb, "      js_jit_close_caps(ctx,_sf_vrefs,%d);\n", var_ref_count);
                for (int _cf = 0; _cf < var_count && _cf < 64; _cf++)
                    if ((sr->captured_local_mask >> _cf) & 1)
                        jit_buf_printf(cb, "      _FREE(_cap_buf[%d]);\n", _cf);
            }
            { uint64_t _eff_am = sr->captured_arg_mask |
                                 (sr->has_yield ? 0ULL : sr->mutated_arg_mask);
              for (int _cf = 0; _cf < arg_count && _cf < 64; _cf++)
                if ((_eff_am >> _cf) & 1)
                    jit_buf_printf(cb, "      _FREE(_arg_cap_buf[%d]);\n", _cf); }
            /* Free remaining stack slots and locals, then return */
            { int _jf; for (_jf=0; _jf<var_count; _jf++)
                jit_buf_printf(cb, "      _FREE(_jsv_%s);\n",
                               varnames[arg_count+_jf]); }
            /* Free _tsv{0} down to _tsv{d-nargs-3} (below this/func slots) */
            for (int _j = d-nargs-3; _j >= 0; _j--)
                jit_buf_printf(cb, "      _FREE(_tsv%d);\n", _j);
            jit_buf_str(cb, "      return _r; }\n");
            break;
        }
        case OP_tail_call: {
            int nargs = (int)bc_u16(&bc[pc+1]);
            /* P8.2: self-recursive tail call → direct C call */
            {
                int func_slot = gen_sp - 1 - nargs;
                int is_self = (func_slot >= 0 && gen_st[func_slot] == JIT_T_SELF_FUNC);
                int fslot = d - nargs - 1;
                /* P9.4: box any typed arg slots before building the args array */
                for (int _aj = 0; _aj < nargs; _aj++)
                    _P94_ENSURE(d-nargs+_aj);
                jit_buf_printf(cb, "    { JSValue _f=_tsv%d;\n", fslot);
                if (nargs > 0) {
                    jit_buf_printf(cb, "      JSValue _ca%d[%d]={", pc, nargs);
                    for (int _aj = 0; _aj < nargs; _aj++) {
                        if (_aj) jit_buf_str(cb, ",");
                        jit_buf_printf(cb, "_tsv%d", d-nargs+_aj);
                    }
                    jit_buf_str(cb, "};\n");
                }
                if (is_self) {
                    jit_buf_str(cb, "      if(_RT->poll_interrupts(ctx)) goto _ex;\n");
                    if (nargs > 0)
                        jit_buf_printf(cb,
                            "      JSValue _r=%s(ctx,JS_UNDEFINED,%d,_ca%d,cpool,var_refs);\n",
                            self_jit_sym, nargs, pc);
                    else
                        jit_buf_printf(cb,
                            "      JSValue _r=%s(ctx,JS_UNDEFINED,0,NULL,cpool,var_refs);\n",
                            self_jit_sym);
                } else {
                    if (nargs > 0)
                        jit_buf_printf(cb,
                            "      JSValue _r=_RT->call(ctx,_f,JS_UNDEFINED,%d,_ca%d);\n",
                            nargs, pc);
                    else
                        jit_buf_str(cb,
                            "      JSValue _r=_RT->call(ctx,_f,JS_UNDEFINED,0,NULL);\n");
                }
                for (int _aj = 0; _aj < nargs; _aj++)
                    jit_buf_printf(cb, "      _FREE(_tsv%d);\n", d-nargs+_aj);
                jit_buf_printf(cb,
                    "      _sp=%d; _FREE(_f);\n"
                    "      if(JS_VALUE_GET_TAG(_r)==JS_TAG_EXCEPTION) goto _ex;\n",
                    fslot);
            }
            /* P13.7: heap-promote var_refs before _cap_buf goes out of scope */
            if (sr->has_fclosure) {
                if (var_ref_count > 0)
                    jit_buf_printf(cb, "      js_jit_close_caps(ctx,_sf_vrefs,%d);\n", var_ref_count);
                for (int _cf = 0; _cf < var_count && _cf < 64; _cf++)
                    if ((sr->captured_local_mask >> _cf) & 1)
                        jit_buf_printf(cb, "      _FREE(_cap_buf[%d]);\n", _cf);
            }
            { uint64_t _eff_am = sr->captured_arg_mask |
                                 (sr->has_yield ? 0ULL : sr->mutated_arg_mask);
              for (int _cf = 0; _cf < arg_count && _cf < 64; _cf++)
                if ((_eff_am >> _cf) & 1)
                    jit_buf_printf(cb, "      _FREE(_arg_cap_buf[%d]);\n", _cf); }
            { int _jf; for (_jf=0; _jf<var_count; _jf++)
                jit_buf_printf(cb, "      _FREE(_jsv_%s);\n",
                               varnames[arg_count+_jf]); }
            /* Free any remaining stack below the func slot */
            for (int _j = d-nargs-2; _j >= 0; _j--)
                jit_buf_printf(cb, "      _FREE(_tsv%d);\n", _j);
            jit_buf_str(cb, "      return _r; }\n");
            break;
        }
        case OP_call_constructor: {
            int nargs = (int)bc_u16(&bc[pc+1]);
            /* stack: ctor(_tsv{d-N-2}) new_target(_tsv{d-N-1}) arg0.._N-1(_tsv{d-N}.._tsv{d-1}) */
            {
                int cslot = d - nargs - 2; /* ctor */
                int nslot = d - nargs - 1; /* new_target */
                /* P9.4: box any typed arg slots before building the args array */
                for (int _aj = 0; _aj < nargs; _aj++)
                    _P94_ENSURE(d-nargs+_aj);
                jit_buf_printf(cb,
                    "    { JSValue _nt=_tsv%d;\n"
                    "      JSValue _ctor=_tsv%d;\n",
                    nslot, cslot);
                if (nargs > 0) {
                    jit_buf_printf(cb, "      JSValue _ca%d[%d]={", pc, nargs);
                    for (int _aj = 0; _aj < nargs; _aj++) {
                        if (_aj) jit_buf_str(cb, ",");
                        jit_buf_printf(cb, "_tsv%d", d-nargs+_aj);
                    }
                    jit_buf_str(cb, "};\n");
                    jit_buf_printf(cb,
                        "      JSValue _r=_RT->call_constructor(ctx,_ctor,_nt,%d,_ca%d);\n",
                        nargs, pc);
                } else {
                    jit_buf_str(cb,
                        "      JSValue _r=_RT->call_constructor(ctx,_ctor,_nt,0,NULL);\n");
                }
                for (int _aj = 0; _aj < nargs; _aj++)
                    jit_buf_printf(cb, "      _FREE(_tsv%d);\n", d-nargs+_aj);
                jit_buf_printf(cb,
                    "      _sp=%d; _FREE(_ctor); _FREE(_nt);\n"
                    "      _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                    cslot, cslot, cslot+1);
            }
            break;
        }
        /* OP_new does not exist; call_constructor handles 'new' expressions */

        /* ---- Return ---- */
        /* P9.2/P9.4: OP_return: box typed slot, pop return value, free remaining stack */
        case OP_return: {
            _borrowed_depth = -1; /* P38.1: borrow cannot survive a return */
            _P94_ENSURE(d-1); /* P9.4: box typed slot before reading as JSValue */
            jit_buf_printf(cb, "    { JSValue _r=_tsv%d; _sp=%d;\n", d-1, d-1);
            /* P13.7: heap-promote var_refs before _cap_buf goes out of scope */
            if (sr->has_fclosure) {
                if (var_ref_count > 0)
                    jit_buf_printf(cb, "      js_jit_close_caps(ctx,_sf_vrefs,%d);\n", var_ref_count);
                for (int _cf = 0; _cf < var_count && _cf < 64; _cf++)
                    if ((sr->captured_local_mask >> _cf) & 1)
                        jit_buf_printf(cb, "      _FREE(_cap_buf[%d]);\n", _cf);
            }
            { uint64_t _eff_am = sr->captured_arg_mask |
                                 (sr->has_yield ? 0ULL : sr->mutated_arg_mask);
              for (int _cf = 0; _cf < arg_count && _cf < 64; _cf++)
                if ((_eff_am >> _cf) & 1)
                    jit_buf_printf(cb, "      _FREE(_arg_cap_buf[%d]);\n", _cf); }
            { int _jf; for (_jf=0; _jf<var_count; _jf++)
                jit_buf_printf(cb, "      _FREE(_jsv_%s);\n",
                               varnames[arg_count+_jf]); }
            for (int _j = d-2; _j >= 0; _j--)
                jit_buf_printf(cb, "      _FREE(_tsv%d);\n", _j);
            jit_buf_str(cb, "      return _r; }\n");
            break;
        }
        /* P9.2: OP_return_undef: free _tsv{0}.._tsv{d-1} and all locals */
        case OP_return_undef: {
            _borrowed_depth = -1; /* P38.1: borrow cannot survive a return */
            jit_buf_str(cb, "    {");
            /* P13.7: heap-promote var_refs before _cap_buf goes out of scope */
            if (sr->has_fclosure) {
                if (var_ref_count > 0)
                    jit_buf_printf(cb, " js_jit_close_caps(ctx,_sf_vrefs,%d);", var_ref_count);
                for (int _cf = 0; _cf < var_count && _cf < 64; _cf++)
                    if ((sr->captured_local_mask >> _cf) & 1)
                        jit_buf_printf(cb, " _FREE(_cap_buf[%d]);", _cf);
            }
            { uint64_t _eff_am = sr->captured_arg_mask |
                                 (sr->has_yield ? 0ULL : sr->mutated_arg_mask);
              for (int _cf = 0; _cf < arg_count && _cf < 64; _cf++)
                if ((_eff_am >> _cf) & 1)
                    jit_buf_printf(cb, " _FREE(_arg_cap_buf[%d]);", _cf); }
            { int _jf; for (_jf=0; _jf<var_count; _jf++)
                jit_buf_printf(cb, " _FREE(_jsv_%s);",
                               varnames[arg_count+_jf]); }
            for (int _j = d-1; _j >= 0; _j--)
                jit_buf_printf(cb, " _FREE(_tsv%d);", _j);
            jit_buf_str(cb, " return JS_UNDEFINED; }\n");
            break;
        }

        /* ---- Throw ---- */
        /* P9.2/P9.4: pop value at _tsv{d-1}, update _sp, then throw */
        case OP_throw:
            _borrowed_depth = -1; /* P38.1: borrow cannot survive a throw */
            _P94_ENSURE(d-1); /* P9.4: box typed slot before throw */
            jit_buf_printf(cb,
                "    { JSValue _v=_tsv%d; _sp=%d; _RT->throw_val(ctx,_v);"
                " goto _ex; }\n",
                d-1, d-1);
            break;

        /* OP_typeof_undef does not exist; OP_typeof handles typeof */

        /* ---- Object/Array creation ---- */
        /* P9.2: push new object at _tsv{d}; depth d -> d+1 */
        case OP_object:
            jit_buf_printf(cb,
                "    { JSValue _r=JS_NewObject(ctx);\n"
                "      _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                d, d, d+1);
            break;
        /* OP_set_name: set function .name property; stack unchanged (1-in 1-out) */
        case OP_set_name: {
            uint32_t _atom = bc_u32(&bc[pc+1]);
            _P94_ENSURE(d-1); /* ensure top is a boxed JSValue */
            jit_buf_printf(cb,
                "    if(JS_IsObject(_tsv%d)) {\n"
                "      int _r=JS_DefinePropertyValue(ctx,_tsv%d,(JSAtom)%uu,"
                "JS_AtomToString(ctx,(JSAtom)%uu),JS_PROP_CONFIGURABLE);\n"
                "      if(_r<0) goto _ex;\n"
                "    }\n",
                d-1, d-1, (unsigned)JS_ATOM_name, _atom);
            break;
        }
        /* OP_define_field: obj(_tsv{d-2}) val(_tsv{d-1}) -> obj stays at _tsv{d-2}; depth d -> d-1 */
        case OP_define_field: {
            uint32_t atom = bc_u32(&bc[pc+1]);
            _P94_ENSURE(d-1); /* P9.4: box typed val slot before use as JSValue */
            jit_buf_printf(cb,
                "    { JSValue _v=_tsv%d; _sp=%d;\n"
                "      int _r=JS_DefinePropertyValue(ctx,_tsv%d,(JSAtom)%uu,_v,"
                "JS_PROP_C_W_E|JS_PROP_THROW);\n"
                "      if(_r<0) goto _ex; }\n",
                d-1, d-1, d-2, atom);
            break;
        }
        /* P9.2: array_from N: pop N items, push array; result at _tsv{d-N}, depth d -> d-N+1 */
        case OP_array_from: {
            int nargs = (int)bc_u16(&bc[pc+1]);
            /* P9.4: box any typed slots used as array elements */
            for (int _aj = 0; _aj < nargs; _aj++)
                _P94_ENSURE(d-nargs+_aj);
            jit_buf_printf(cb,
                "    { JSValue _r=JS_NewArray(ctx);\n"
                "      _sp=%d; _CHK(_r);\n",
                d-nargs);
            /* JS_SetPropertyUint32 takes ownership of the value */
            for (int _aj = 0; _aj < nargs; _aj++)
                jit_buf_printf(cb,
                    "      JS_SetPropertyUint32(ctx,_r,(uint32_t)%d,_tsv%d);\n",
                    _aj, d-nargs+_aj);
            jit_buf_printf(cb,
                "      _tsv%d=_r; _sp=%d; }\n",
                d-nargs, d-nargs+1);
            break;
        }

        /* ---- P16: property deletion ---- */
        /* OP_delete: obj(_tsv{d-2}) key(_tsv{d-1}) -> bool(_tsv{d-2}); depth d -> d-1 */
        case OP_delete:
            _P94_ENSURE(d-2); /* P9.4: box typed obj slot */
            _P94_ENSURE(d-1); /* P9.4: box typed key slot */
            jit_buf_printf(cb,
                "    { JSValue _obj=_tsv%d,_key=_tsv%d; _sp=0;\n"
                "      JSAtom _at=JS_ValueToAtom(ctx,_key);\n"
                "      _FREE(_key);\n"
                "      if(_at==JS_ATOM_NULL){ _FREE(_obj); goto _ex; }\n"
                "      int _ret=JS_DeleteProperty(ctx,_obj,_at,JS_PROP_THROW_STRICT);\n"
                "      JS_FreeAtom(ctx,_at); _FREE(_obj);\n"
                "      if(_ret<0) goto _ex;\n"
                "      _tsv%d=JS_NewBool(ctx,_ret); _sp=%d; }\n",
                d-2, d-1, d-2, d-1);
            break;
        /* OP_delete_var <atom>: push bool; depth d -> d+1 */
        case OP_delete_var: {
            uint32_t atom = bc_u32(&bc[pc+1]);
            jit_buf_printf(cb,
                "    { int _ret=_RT->delete_global_var(ctx,(JSAtom)%uu);\n"
                "      if(_ret<0) goto _ex;\n"
                "      _tsv%d=JS_NewBool(ctx,_ret); _sp=%d; }\n",
                atom, d, d+1);
            break;
        }

        /* ---- P17: spread / apply ---- */
        /* OP_apply <magic u16>: func(_tsv{d-3}) this(_tsv{d-2}) args(_tsv{d-1})
         * -> result(_tsv{d-3}); depth d -> d-2 */
        case OP_apply: {
            int magic = (int)bc_u16(&bc[pc+1]);
            _P94_ENSURE(d-3); /* P9.4: box typed func slot */
            _P94_ENSURE(d-2); /* P9.4: box typed this slot */
            _P94_ENSURE(d-1); /* P9.4: box typed args slot */
            jit_buf_printf(cb,
                "    { JSValue _func=_tsv%d,_this=_tsv%d,_args=_tsv%d; _sp=0;\n"
                "      JSValue _r=_RT->apply(ctx,_func,_this,_args,%d);\n"
                "      _FREE(_func); _FREE(_this); _FREE(_args);\n"
                "      _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                d-3, d-2, d-1, magic, d-3, d-3, d-2);
            break;
        }
        /* OP_apply_eval <scope_idx u16>: func(_tsv{d-2}) args(_tsv{d-1})
         * -> result(_tsv{d-2}); depth d -> d-1 */
        case OP_apply_eval: {
            /* scope_idx in bytecode = s->scopes[scope].first - ARG_SCOPE_END;
             * at runtime: scope_idx = stored + ARG_SCOPE_END.
             * ARG_SCOPE_END = -2 (from quickjs.c). */
            int raw = (int)(int16_t)bc_u16(&bc[pc+1]);
            int scope_idx = raw + (-2); /* ARG_SCOPE_END */
            _P94_ENSURE(d-2); /* P9.4: box typed func slot */
            _P94_ENSURE(d-1); /* P9.4: box typed args slot */
            jit_buf_printf(cb,
                "    { JSValue _func=_tsv%d,_args=_tsv%d; _sp=0;\n"
                "      JSValue _r=_RT->apply_eval(ctx,_func,_args,%d);\n"
                "      _FREE(_func); _FREE(_args);\n"
                "      _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                d-2, d-1, scope_idx, d-2, d-2, d-1);
            break;
        }

        /* ---- P12: generator yield/resume opcodes ---- */
        case OP_initial_yield: {
            /* Suspend the generator before any yields have been made.
             * Stack is empty at this point (d == 0).
             * Emits: yield_setup(JS_UNDEFINED, resume_idx=0) → return FUNC_RET_INITIAL_YIELD
             * Then _Lresume_0: — entered by dispatch table on next .next() call.
             * The first .next(v) argument is ignored per spec; we consume and free it. */
            jit_buf_str(cb,
                "    js_jit_gen_yield_setup(ctx,JS_UNDEFINED,0,_gf);\n"
                "    return JS_NewInt32(ctx,3);\n" /* FUNC_RET_INITIAL_YIELD */
                "    _Lresume_0:;\n"
                "    { JSValue _nv0=js_jit_gen_get_next_val(ctx); JS_FreeValue(ctx,_nv0); }\n");
            break;
        }

        case OP_yield: {
            /* Suspend the generator, yielding _tsv{d-1} to the caller.
             *
             * On the resume path (_Lresume_N:):
             *   _tsv{d-1} = .next(v) value    (JSVAL, owned by JIT)
             *   _tsv{d}   = magic int (0=next, 1=return, 2=throw)  (JS_NewInt32)
             *   gen_sp updated to d+1
             *
             * The next bytecode opcode is always OP_if_false which checks the magic. */
            int j;
            int yresi = yield_site_counter++;
            _P94_ENSURE(d-1); /* box the yield value if it is typed */
            /* Spill all local JSValues into saved_lv (transfer ownership) */
            for (j = 0; j < var_count; j++) {
                jit_buf_printf(cb,
                    "    _gf->saved_lv[%d]=_jsv_%s; _jsv_%s=JS_UNDEFINED;\n",
                    j, LNAME(j), LNAME(j));
            }
            /* Save live stack temporaries BELOW the yield value into saved_lv.
             * These slots (_tsv0..._tsv{d-2}) survive as C locals but the C stack
             * frame is destroyed when we return.  Transfer ownership to the frame. */
            for (j = 0; j < d-1; j++) {
                jit_buf_printf(cb,
                    "    _gf->saved_lv[%d]=_tsv%d; _tsv%d=JS_UNDEFINED;\n",
                    var_count + j, j, j);
            }
            /* P12.2: heap-promote captured locals/args and save vrefs to frame */
            if (sr->has_fclosure && var_ref_count > 0) {
                jit_buf_printf(cb,
                    "    js_jit_close_caps(ctx,_sf_vrefs,%d);\n"
                    "    js_jit_gen_save_vrefs(ctx,_sf_vrefs,%d,_gf);\n",
                    var_ref_count, var_ref_count);
            }
            /* P12.1: save catch state so try/catch works across yield */
            if (sr->has_try && stack_size > 0) {
                jit_buf_str(cb,
                    "    _gf->catch_depth=_catch_depth;\n"
                    "    if(_catch_depth>0){\n"
                    "        memcpy(_gf->catch_sp,_catch_sp,(size_t)_catch_depth*sizeof(int));\n"
                    "        memcpy(_gf->catch_h,_catch_h,(size_t)_catch_depth*sizeof(int));\n"
                    "    }\n");
            }
            jit_buf_printf(cb,
                "    { JSValue _yv%d=_tsv%d; _sp=0;\n"
                "      js_jit_gen_yield_setup(ctx,_yv%d,%d,_gf);\n"
                "    }\n"
                "    return JS_NewInt32(ctx,1);\n" /* FUNC_RET_YIELD */
                /* Stack-slot restoration (tsv0..d-2) is done in preamble dispatch
                 * before 'goto _Lresume_N'.  Here we only pick up the .next(v) value
                 * and the magic flag from the two stack-buffer slots. */
                "    _Lresume_%d:;\n"
                "    { JSValue _nv%d=js_jit_gen_get_next_val(ctx);\n"
                "      int _mg%d=js_jit_gen_get_magic_int(ctx);\n"
                "      _tsv%d=_nv%d; _tsv%d=JS_NewInt32(ctx,_mg%d);\n"
                "      _sp=%d; }\n",
                yresi, d-1,
                yresi, yresi,
                yresi,
                yresi, yresi,
                d-1, yresi, d, yresi, d+1);
            /* Update gen-time type stack: d-1 = JSVAL (next_val), d = JSVAL (magic) */
            if (gen_sp > 0) gen_sp--;          /* pop yield value */
            if (gen_sp < gen_stk_cap) { gen_st[gen_sp] = JIT_T_JSVAL; gen_hsh[gen_sp] = 0; gen_sp++; } /* next_val */
            if (gen_sp < gen_stk_cap) { gen_st[gen_sp] = JIT_T_JSVAL; gen_hsh[gen_sp] = 0; gen_sp++; } /* magic */
            break;
        }

        case OP_await: {
            /* P12.3: Async function await — suspends execution, yielding the
             * awaited value (_tsv{d-1}) to the Promise machinery.
             *
             * On resume (_Lresume_N:): only the resolved value is pushed back
             * (no magic int).  A throw resume sets throw_flag and is handled by
             * the throw check at the top of every entry (preamble).
             *
             * Returns FUNC_RET_AWAIT=0 to async_func_resume.
             */
            int j;
            int yresi = yield_site_counter++;
            _P94_ENSURE(d-1); /* box the awaited value if it is typed */
            /* Spill all local JSValues into saved_lv (transfer ownership) */
            for (j = 0; j < var_count; j++) {
                jit_buf_printf(cb,
                    "    _gf->saved_lv[%d]=_jsv_%s; _jsv_%s=JS_UNDEFINED;\n",
                    j, LNAME(j), LNAME(j));
            }
            /* Save live stack temporaries BELOW the awaited value into saved_lv.
             * These slots (_tsv0..._tsv{d-2}) survive as C locals but the C stack
             * frame is destroyed when we return.  Transfer ownership to the frame. */
            for (j = 0; j < d-1; j++) {
                jit_buf_printf(cb,
                    "    _gf->saved_lv[%d]=_tsv%d; _tsv%d=JS_UNDEFINED;\n",
                    var_count + j, j, j);
            }
            /* P12.2: heap-promote captured locals/args and save vrefs to frame */
            if (sr->has_fclosure && var_ref_count > 0) {
                jit_buf_printf(cb,
                    "    js_jit_close_caps(ctx,_sf_vrefs,%d);\n"
                    "    js_jit_gen_save_vrefs(ctx,_sf_vrefs,%d,_gf);\n",
                    var_ref_count, var_ref_count);
            }
            /* P12.1: save catch state */
            if (sr->has_try && stack_size > 0) {
                jit_buf_str(cb,
                    "    _gf->catch_depth=_catch_depth;\n"
                    "    if(_catch_depth>0){\n"
                    "        memcpy(_gf->catch_sp,_catch_sp,(size_t)_catch_depth*sizeof(int));\n"
                    "        memcpy(_gf->catch_h,_catch_h,(size_t)_catch_depth*sizeof(int));\n"
                    "    }\n");
            }
            jit_buf_printf(cb,
                "    { JSValue _yv%d=_tsv%d; _sp=0;\n"
                "      js_jit_gen_yield_setup(ctx,_yv%d,%d,_gf);\n"
                "    }\n"
                "    return JS_NewInt32(ctx,0);\n" /* FUNC_RET_AWAIT */
                /* Stack-slot restoration (tsv0..d-2) is done in preamble dispatch
                 * before 'goto _Lresume_N'.  The throw check here catches rejected
                 * awaits that land after the preamble's catch-state restoration. */
                "    _Lresume_%d:;\n"
                "    if(js_jit_gen_get_throw(ctx)){goto _ex;}\n"
                "    { JSValue _rv%d=js_jit_gen_get_next_val(ctx);\n"
                "      _tsv%d=_rv%d; _sp=%d; }\n",
                yresi, d-1,
                yresi, yresi,
                yresi,
                yresi,
                d-1, yresi, d);
            /* Update gen-time type stack: slot d-1 = resolved JSVAL (net 0 change) */
            if (gen_sp > 0) { gen_st[gen_sp-1] = JIT_T_JSVAL; gen_hsh[gen_sp-1] = 0; }
            break;
        }

        case OP_yield_star:
        case OP_async_yield_star: {
            /* P29: Delegate yield — same suspend/resume machinery as OP_yield but
             * returns FUNC_RET_YIELD_STAR (2) so the generator runtime knows to drive
             * the full iterator delegation protocol on the outer side.
             *
             * On resume (_Lresume_N:): exactly like OP_yield — two values pushed:
             *   _tsv{d-1} = .next(v) value    (JSVAL)
             *   _tsv{d}   = magic int (0=next, 1=return, 2=throw)
             */
            int j;
            int yresi = yield_site_counter++;
            _P94_ENSURE(d-1); /* box the yield* value if it is typed */
            /* Spill all local JSValues into saved_lv (transfer ownership) */
            for (j = 0; j < var_count; j++) {
                jit_buf_printf(cb,
                    "    _gf->saved_lv[%d]=_jsv_%s; _jsv_%s=JS_UNDEFINED;\n",
                    j, LNAME(j), LNAME(j));
            }
            /* Save live stack temporaries BELOW the yield* value into saved_lv */
            for (j = 0; j < d-1; j++) {
                jit_buf_printf(cb,
                    "    _gf->saved_lv[%d]=_tsv%d; _tsv%d=JS_UNDEFINED;\n",
                    var_count + j, j, j);
            }
            /* P12.2: heap-promote captured locals/args and save vrefs to frame */
            if (sr->has_fclosure && var_ref_count > 0) {
                jit_buf_printf(cb,
                    "    js_jit_close_caps(ctx,_sf_vrefs,%d);\n"
                    "    js_jit_gen_save_vrefs(ctx,_sf_vrefs,%d,_gf);\n",
                    var_ref_count, var_ref_count);
            }
            /* P12.1: save catch state so try/catch works across yield* */
            if (sr->has_try && stack_size > 0) {
                jit_buf_str(cb,
                    "    _gf->catch_depth=_catch_depth;\n"
                    "    if(_catch_depth>0){\n"
                    "        memcpy(_gf->catch_sp,_catch_sp,(size_t)_catch_depth*sizeof(int));\n"
                    "        memcpy(_gf->catch_h,_catch_h,(size_t)_catch_depth*sizeof(int));\n"
                    "    }\n");
            }
            jit_buf_printf(cb,
                "    { JSValue _yv%d=_tsv%d; _sp=0;\n"
                "      js_jit_gen_yield_setup(ctx,_yv%d,%d,_gf);\n"
                "    }\n"
                "    return JS_NewInt32(ctx,2);\n" /* FUNC_RET_YIELD_STAR */
                "    _Lresume_%d:;\n"
                "    { JSValue _nv%d=js_jit_gen_get_next_val(ctx);\n"
                "      int _mg%d=js_jit_gen_get_magic_int(ctx);\n"
                "      _tsv%d=_nv%d; _tsv%d=JS_NewInt32(ctx,_mg%d);\n"
                "      _sp=%d; }\n",
                yresi, d-1,
                yresi, yresi,
                yresi,
                yresi, yresi,
                d-1, yresi, d, yresi, d+1);
            /* Update gen-time type stack: d-1 = JSVAL (next_val), d = JSVAL (magic) */
            if (gen_sp > 0) gen_sp--;          /* pop yield* value */
            if (gen_sp < gen_stk_cap) { gen_st[gen_sp] = JIT_T_JSVAL; gen_hsh[gen_sp] = 0; gen_sp++; } /* next_val */
            if (gen_sp < gen_stk_cap) { gen_st[gen_sp] = JIT_T_JSVAL; gen_hsh[gen_sp] = 0; gen_sp++; } /* magic */
            break;
        }

        case OP_return_async: {
            /* Generator/async function return (normal end or via .return(v) / .throw()).
             *
             * _tsv{d-1} holds the return value (the value passed to .return(), or
             * the generator's `return expr` value, or the .next(v) value discarded
             * by the bytecode's yield→if_false→return_async pattern).
             *
             * We store it into stack_start[0] (sf->cur_sp[-1] after yield_setup),
             * free all lower stack slots and all locals, then return JS_UNDEFINED.
             * async_func_resume detects JS_UNDEFINED → reads sf->cur_sp[-1] →
             * frees the frame → delivers the value to the caller.           */
            int j;
            _P94_ENSURE(d-1);
            /* Free stack slots below the return value */
            for (j = 0; j < d-1; j++)
                jit_buf_printf(cb, "    _FREE(_tsv%d);\n", j);
            /* Store return value in stack_start[0] and free JIT locals */
            jit_buf_printf(cb,
                "    { JSValue _rv=_tsv%d; _sp=%d;\n", d-1, d-1);
            /* P12.2: heap-promote caps before the C-local arrays go out of scope */
            if (sr->has_fclosure) {
                if (var_ref_count > 0)
                    jit_buf_printf(cb, "      js_jit_close_caps(ctx,_sf_vrefs,%d);\n", var_ref_count);
                for (j = 0; j < var_count && j < 64; j++)
                    if ((sr->captured_local_mask >> j) & 1)
                        jit_buf_printf(cb, "      _FREE(_cap_buf[%d]);\n", j);
            }
            { uint64_t _eff_am = sr->captured_arg_mask |
                                 (sr->has_yield ? 0ULL : sr->mutated_arg_mask);
              for (j = 0; j < arg_count && j < 64; j++)
                if ((_eff_am >> j) & 1)
                    jit_buf_printf(cb, "      _FREE(_arg_cap_buf[%d]);\n", j); }
            for (j = 0; j < var_count; j++)
                jit_buf_printf(cb, "      _FREE(_jsv_%s);\n", LNAME(j));
            jit_buf_str(cb,
                "      js_jit_gen_yield_setup(ctx,_rv,-2,_gf);\n"
                "    }\n"
                "    return JS_UNDEFINED;\n");
            gen_sp = 0;
            break;
        }

        /* ---- P19: utility ops ---- */

        /* OP_get_var_undef: like OP_get_var but passes FALSE to GetPropertyInternal.
         * Operand: u16 index into closure var_refs. */
        case OP_get_var_undef: {
            int idx = (int)bc_u16(&bc[pc+1]);
            JSAtom cv_atom   = js_jit_fb_get_closure_var_atom(b, idx);
            int    cv_is_lex = js_jit_fb_get_closure_var_is_lexical(b, idx);
            /* P40.2: use _vrp{idx} (pvalue cached in preamble). */
            jit_buf_printf(cb,
                "    { if(JS_VALUE_GET_TAG(*_vrp%d)==JS_TAG_UNINITIALIZED){\n"
                "        JSValue _r=_RT->get_var_undef(ctx,%uu,%d);\n"
                "        _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d;\n"
                "      } else { _tsv%d=_DUP(*_vrp%d); _sp=%d; } }\n",
                idx, (unsigned)cv_atom, cv_is_lex, d, d, d+1, d, idx, d+1);
            break;
        }

        /* OP_throw_error: bake atom+type into generated C, always jumps to _ex.
         * Operand: atom u32 + type u8. */
        case OP_throw_error: {
            uint32_t atom = bc_u32(&bc[pc+1]);
            int type = (int)bc[pc+5];
            jit_buf_printf(cb,
                "    _RT->throw_error(ctx,(JSAtom)%uu,%d); goto _ex;\n",
                (unsigned)atom, type);
            break;
        }

        /* OP_to_object: borrows top-of-stack, replaces with wrapped object. */
        case OP_to_object: {
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { JSValue _v=_tsv%d;\n"
                "      JSValue _r=_RT->to_object(ctx,_v);\n"
                "      _FREE(_v); _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                d-1, d-1, d-1, d);
            break;
        }

        /* OP_to_propkey: borrows top-of-stack, replaces with propkey-coerced value. */
        case OP_to_propkey: {
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { JSValue _v=_tsv%d;\n"
                "      JSValue _r=_RT->to_propkey(ctx,_v);\n"
                "      _FREE(_v); _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                d-1, d-1, d-1, d);
            break;
        }

        /* OP_regexp: pops pattern and bc_val (both consumed), pushes RegExp object. */
        case OP_regexp: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    { JSValue _pat=_tsv%d, _bc2=_tsv%d; _sp=%d;\n"
                "      JSValue _r=_RT->regexp(ctx,_pat,_bc2);\n"
                "      _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                d-2, d-1, d-2, d-2, d-2, d-1);
            break;
        }

        /* OP_set_name_computed: name_src(_tsv{d-2}) func(_tsv{d-1}) — no depth change.
         * Interpreter: JS_DefineObjectNameComputed(ctx, sp[-1]=func, sp[-2]=name_src).
         * Borrows both, may throw. */
        case OP_set_name_computed: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    { int _r=_RT->set_name_computed(ctx,_tsv%d,_tsv%d);\n"
                "      if(_r<0) goto _ex; }\n",
                d-1, d-2);  /* func=d-1, name_src=d-2 */
            break;
        }

        /* OP_set_proto: obj(_tsv{d-2}) proto(_tsv{d-1}) → obj; pops proto. */
        case OP_set_proto: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    { JSValue _proto=_tsv%d; _sp=%d;\n"
                "      int _r=_RT->set_proto(ctx,_tsv%d,_proto);\n"
                "      _FREE(_proto);\n"
                "      if(_r<0) goto _ex; }\n",
                d-1, d-1, d-2);
            break;
        }

        /* OP_set_home_object: home(_tsv{d-2}) func(_tsv{d-1}) — no depth change.
         * Interpreter: js_method_set_home_object(ctx, sp[-1]=func, sp[-2]=home).
         * Borrows both, never throws. */
        case OP_set_home_object: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    _RT->set_home_object(ctx,_tsv%d,_tsv%d);\n",
                d-1, d-2);  /* func=d-1, home=d-2 */
            break;
        }

        /* OP_get_array_el2: obj(_tsv{d-2}) prop(_tsv{d-1}) → obj val.
         * Borrows obj, CONSUMES prop; val replaces prop at d-1. Depth unchanged. */
        case OP_get_array_el2: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    { JSValue _prop=_tsv%d; _sp=%d;\n"
                "      JSValue _r=_RT->get_array_el2(ctx,_tsv%d,_prop);\n"
                "      _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                d-1, d-1, d-2, d-1, d-1, d);
            break;
        }

        /* OP_get_array_el3: arr(_tsv{d-2}) idx(_tsv{d-1}) → arr idx result.
         * Borrows both; result pushed at _tsv{d}. Depth +1. */
        case OP_get_array_el3: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    { JSValue _r=_RT->get_array_el2(ctx,_tsv%d,_DUP(_tsv%d));\n"
                "      _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                d-2, d-1, d, d, d+1);
            break;
        }

        /* OP_define_array_el: arr(_tsv{d-3}) key(_tsv{d-2}) val(_tsv{d-1}) → arr key.
         * Borrows arr, DUPs key (helper consumes DUP), consumes val. Depth -1. */
        case OP_define_array_el: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            _P94_ENSURE(d-3);
            jit_buf_printf(cb,
                "    { JSValue _val=_tsv%d; _sp=%d;\n"
                "      int _r=_RT->define_array_el(ctx,_tsv%d,_DUP(_tsv%d),_val);\n"
                "      if(_r<0) goto _ex; }\n",
                d-1, d-1, d-3, d-2);
            break;
        }

        /* OP_push_bigint_i32: push BigInt from i32 literal. Operand: i32. */
        case OP_push_bigint_i32: {
            int32_t v = (int32_t)bc_u32(&bc[pc+1]);
            jit_buf_printf(cb,
                "    { JSValue _r=_RT->push_bigint_i32(ctx,%d);\n"
                "      _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                v, d, d, d+1);
            break;
        }

        /* OP_close_loc: detach one captured local's JSVarRef.
         * Operand: u16 local var idx → mapped through vardefs to get _sf_vrefs index. */
        case OP_close_loc: {
            int var_idx = (int)bc_u16(&bc[pc+1]);
            int vri = js_jit_fb_get_local_var_ref_idx(b, var_idx);
            if (vri < 0) {
                /* var has no associated var_ref — close_loc is a no-op here */
                break;
            }
            jit_buf_printf(cb,
                "    _RT->close_loc(ctx,_sf_vrefs[%d]);\n", vri);
            break;
        }

        /* ---- P20: ref-slot ops ---- */

        /* OP_make_loc_ref / OP_make_arg_ref: capture a local/arg into a ref-pair.
         * Operands: atom u32 + idx u16 (local/arg index → mapped to _sf_vrefs index).
         * Pushes (obj, atom_val) at d, d+1. */
        case OP_make_loc_ref:
        case OP_make_arg_ref: {
            uint32_t atom = bc_u32(&bc[pc+1]);
            int idx = (int)bc_u16(&bc[pc+5]);
            int vri = (op == OP_make_arg_ref)
                      ? js_jit_fb_get_arg_var_ref_idx(b, idx)
                      : js_jit_fb_get_local_var_ref_idx(b, idx);
            if (vri < 0) {
                /* No var_ref available — generate a runtime error path */
                jit_buf_printf(cb,
                    "    JS_ThrowInternalError(ctx,\"make_ref: no var_ref for idx %d\"); goto _ex;\n",
                    idx);
                break;
            }
            jit_buf_printf(cb,
                "    { JSValue _obj, _atv;\n"
                "      if(_RT->make_ref_pair(ctx,_sf_vrefs[%d],(JSAtom)%uu,&_obj,&_atv)<0) goto _ex;\n"
                "      _sp=%d; _tsv%d=_obj; _sp=%d; _tsv%d=_atv; _sp=%d; }\n",
                vri, (unsigned)atom, d, d, d+1, d+1, d+2);
            break;
        }

        /* OP_make_var_ref_ref: re-use an existing closure var_ref (outer capture).
         * Operands: atom u32 + var_ref_idx u16. Pushes (obj, atom_val). */
        case OP_make_var_ref_ref: {
            uint32_t atom = bc_u32(&bc[pc+1]);
            int idx = (int)bc_u16(&bc[pc+5]);
            jit_buf_printf(cb,
                "    { JSValue _obj, _atv;\n"
                "      if(_RT->make_ref_pair(ctx,var_refs[%d],(JSAtom)%uu,&_obj,&_atv)<0) goto _ex;\n"
                "      _sp=%d; _tsv%d=_obj; _sp=%d; _tsv%d=_atv; _sp=%d; }\n",
                idx, (unsigned)atom, d, d, d+1, d+1, d+2);
            break;
        }

        /* OP_make_var_ref: create a ref-pair for a global variable.
         * Operand: atom u32. Pushes (obj, atom_val). */
        case OP_make_var_ref: {
            uint32_t atom = bc_u32(&bc[pc+1]);
            jit_buf_printf(cb,
                "    { JSValue _obj, _atv;\n"
                "      if(_RT->make_var_ref(ctx,(JSAtom)%uu,&_obj,&_atv)<0) goto _ex;\n"
                "      _sp=%d; _tsv%d=_obj; _sp=%d; _tsv%d=_atv; _sp=%d; }\n",
                (unsigned)atom, d, d, d+1, d+1, d+2);
            break;
        }

        /* OP_get_ref_value: obj(_tsv{d-2}) atom_val(_tsv{d-1}) → obj atom_val result.
         * Borrows both; result pushed at _tsv{d}. Depth +1. */
        case OP_get_ref_value: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    { JSValue _r=_RT->get_ref_value(ctx,_tsv%d,_tsv%d);\n"
                "      _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                d-2, d-1, d, d, d+1);
            break;
        }

        /* OP_put_ref_value: obj(_tsv{d-3}) atom_val(_tsv{d-2}) val(_tsv{d-1}) → (all consumed).
         * Helper takes ownership of all 3. Depth -3. */
        case OP_put_ref_value: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            _P94_ENSURE(d-3);
            jit_buf_printf(cb,
                "    { JSValue _o=_tsv%d,_av=_tsv%d,_val=_tsv%d; _sp=%d;\n"
                "      if(_RT->put_ref_value(ctx,_o,_av,_val)<0) goto _ex; }\n",
                d-3, d-2, d-1, d-3);
            break;
        }

        /* ---- P21: spread / rest / copy ---- */

        /* OP_rest: push rest-argument array.  Operand: u16 first. */
        case OP_rest: {
            int first = (int)bc_u16(&bc[pc+1]);
            jit_buf_printf(cb,
                "    { JSValue _r=_RT->rest(ctx,%d,argc,argv);\n"
                "      _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                first, d, d, d+1);
            break;
        }

        /* OP_append: array(d-3) pos(d-2) enumobj(d-1) → array(d-3) pos(d-2).
         * Helper frees enumobj and updates array/pos via pointers. */
        case OP_append: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            _P94_ENSURE(d-3);
            jit_buf_printf(cb,
                "    { JSValue _enu=_tsv%d; _sp=%d;\n"
                "      if(_RT->append(ctx,&_tsv%d,&_tsv%d,_enu)<0) goto _ex;\n"
                "      _sp=%d; }\n",
                d-1, d-2, d-3, d-2, d-1);
            break;
        }

        /* OP_copy_data_properties: operand=u8 mask encoding offsets.
         * Borrows target, source, excluded (all stay on stack). Net 0. */
        case OP_copy_data_properties: {
            int mask = (int)bc[pc+1];
            int t_slot = d - 1 - (mask & 3);
            int s_slot = d - 1 - ((mask >> 2) & 7);
            int e_slot = d - 1 - ((mask >> 5) & 7);
            _P94_ENSURE(t_slot);
            _P94_ENSURE(s_slot);
            _P94_ENSURE(e_slot);
            jit_buf_printf(cb,
                "    if(_RT->copy_data_properties(ctx,_tsv%d,_tsv%d,_tsv%d)<0) goto _ex;\n",
                t_slot, s_slot, e_slot);
            break;
        }

        /* ---- P22: private fields ---- */

        /* OP_private_symbol: push new private symbol. Operand: atom u32. */
        case OP_private_symbol: {
            uint32_t atom = bc_u32(&bc[pc+1]);
            jit_buf_printf(cb,
                "    { JSValue _r=_RT->private_symbol(ctx,(JSAtom)%uu);\n"
                "      _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                (unsigned)atom, d, d, d+1);
            break;
        }

        /* OP_get_private_field: obj(d-2) prop(d-1) → val(d-2). Net -1. */
        case OP_get_private_field: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    { JSValue _o=_tsv%d,_p=_tsv%d; _sp=%d;\n"
                "      JSValue _r=_RT->get_private_field(ctx,_o,_p);\n"
                "      _FREE(_o); _FREE(_p);\n"
                "      _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                d-2, d-1, d-2, d-2, d-2, d-1);
            break;
        }

        /* OP_put_private_field: obj(d-3) val(d-2) prop(d-1) → consumed. Net -3.
         * Order: sp[-3]=obj, sp[-2]=val, sp[-1]=prop. */
        case OP_put_private_field: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            _P94_ENSURE(d-3);
            jit_buf_printf(cb,
                "    { JSValue _o=_tsv%d,_v=_tsv%d,_p=_tsv%d; _sp=%d;\n"
                "      if(_RT->put_private_field(ctx,_o,_p,_v)<0) goto _ex; }\n",
                d-3, d-2, d-1, d-3);
            break;
        }

        /* OP_define_private_field: obj(d-3) prop(d-2) val(d-1) → obj(d-3). Net -2. */
        case OP_define_private_field: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    { JSValue _p=_tsv%d,_v=_tsv%d; _sp=%d;\n"
                "      if(_RT->define_private_field(ctx,_tsv%d,_p,_v)<0) goto _ex;\n"
                "      _sp=%d; }\n",
                d-2, d-1, d-2, d-3, d-1);
            break;
        }

        /* OP_private_in: obj(d-2) prop(d-1) → bool(d-2). Net -1.
         * Note interpreter: sp[-2]=object, sp[-1]=name/method. */
        case OP_private_in: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    { JSValue _o=_tsv%d,_p=_tsv%d; _sp=%d;\n"
                "      JSValue _r=_RT->private_in(ctx,_o,_p);\n"
                "      _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                d-2, d-1, d-2, d-2, d-2, d-1);
            break;
        }

        /* ---- P23: OOP / class helpers (feasible subset) ---- */

        /* OP_check_ctor_return: val(d-1) → val(d-1) bool(d). Net +1.
         * If val is not object and not undefined → TypeError.
         * If val is not object → push TRUE (use 'this'), else push FALSE (use val). */
        case OP_check_ctor_return: {
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { if(!JS_IsObject(_tsv%d)) {\n"
                "        if(!JS_IsUndefined(_tsv%d)) {\n"
                "          JS_ThrowTypeError(ctx,\"derived class constructor must return"
                                             " an object or undefined\");\n"
                "          goto _ex;\n"
                "        }\n"
                "        _tsv%d=JS_TRUE; _sp=%d;\n"
                "      } else {\n"
                "        _tsv%d=JS_FALSE; _sp=%d;\n"
                "      } }\n",
                d-1, d-1, d, d+1, d, d+1);
            break;
        }

        /* OP_check_brand: this_obj(d-2) func(d-1) → same (borrows both). Net 0. */
        case OP_check_brand: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    if(_RT->check_brand(ctx,_tsv%d,_tsv%d)<0) goto _ex;\n",
                d-2, d-1);
            break;
        }

        /* OP_add_brand: this_obj(d-2) home_obj(d-1) → (both consumed). Net -2. */
        case OP_add_brand: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    { JSValue _t=_tsv%d,_h=_tsv%d; _sp=%d;\n"
                "      int _r=_RT->add_brand(ctx,_t,_h);\n"
                "      _FREE(_t); _FREE(_h);\n"
                "      if(_r<0) goto _ex; }\n",
                d-2, d-1, d-2);
            break;
        }

        /* OP_get_super: obj(d-1) → proto(d-1). Net 0.
         * JS_GetPrototype is in the public API (quickjs.h). */
        case OP_get_super: {
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { JSValue _o=_tsv%d;\n"
                "      JSValue _r=JS_GetPrototype(ctx,_o);\n"
                "      _FREE(_o); _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                d-1, d-1, d-1, d);
            break;
        }

        /* OP_get_super_value: this(d-3) obj(d-2) prop(d-1) → val(d-3). Net -2. */
        case OP_get_super_value: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            _P94_ENSURE(d-3);
            jit_buf_printf(cb,
                "    { JSValue _tv=_tsv%d,_o=_tsv%d,_p=_tsv%d; _sp=%d;\n"
                "      JSValue _r=_RT->get_super_value(ctx,_tv,_o,_p);\n"
                "      _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                d-3, d-2, d-1, d-3, d-3, d-3, d-2);
            break;
        }

        /* OP_put_super_value: this(d-4) obj(d-3) prop(d-2) val(d-1) → consumed. Net -4. */
        case OP_put_super_value: {
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            _P94_ENSURE(d-3);
            _P94_ENSURE(d-4);
            jit_buf_printf(cb,
                "    { JSValue _tv=_tsv%d,_o=_tsv%d,_p=_tsv%d,_v=_tsv%d; _sp=%d;\n"
                "      if(_RT->put_super_value(ctx,_tv,_o,_p,_v)<0) goto _ex; }\n",
                d-4, d-3, d-2, d-1, d-4);
            break;
        }

        /* OP_define_method: obj(d-2) func(d-1) → obj(d-2). Net -1.
         * Operands: atom u32 + op_flags u8. */
        case OP_define_method: {
            uint32_t atom = bc_u32(&bc[pc+1]);
            int op_flags = (int)bc[pc+5];
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            jit_buf_printf(cb,
                "    { JSValue _func=_tsv%d; _sp=%d;\n"
                "      if(_RT->define_method(ctx,_tsv%d,_func,(JSAtom)%uu,%d)<0) goto _ex;\n"
                "      _sp=%d; }\n",
                d-1, d-1, d-2, (unsigned)atom, op_flags, d-1);
            break;
        }

        /* OP_define_method_computed: obj(d-3) key(d-2) func(d-1) → obj(d-3). Net -2.
         * Operand: op_flags u8. */
        case OP_define_method_computed: {
            int op_flags = (int)bc[pc+1];
            _P94_ENSURE(d-1);
            _P94_ENSURE(d-2);
            _P94_ENSURE(d-3);
            jit_buf_printf(cb,
                "    { JSValue _k=_tsv%d,_func=_tsv%d; _sp=%d;\n"
                "      if(_RT->define_method_computed(ctx,_tsv%d,_k,_func,%d)<0) goto _ex;\n"
                "      _sp=%d; }\n",
                d-2, d-1, d-2, d-3, op_flags, d-1);
            break;
        }

        /* ---- P26: constructor / class-definition opcodes ---- */

        /* OP_check_ctor: no stack effect.  Reads new_target via current_stack_frame. */
        case OP_check_ctor: {
            jit_buf_printf(cb,
                "    if(_RT->check_ctor(ctx)<0) goto _ex;\n");
            break;
        }

        /* OP_init_ctor: push result of super() on stack.  Reads new_target/func_obj
         * via current_stack_frame; argc/argv are JIT function parameters. */
        case OP_init_ctor: {
            jit_buf_printf(cb,
                "    { JSValue _r=_RT->init_ctor(ctx,argc,argv);\n"
                "      _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                d, d, d+1);
            break;
        }

        /* OP_define_class: parent(d-2) bfunc(d-1) → ctor(d-2) proto(d-1). Net 0.
         * Operands: atom u32, class_flags u8. */
        case OP_define_class: {
            uint32_t atom       = bc_u32(&bc[pc+1]);
            int      class_flags = (int)bc[pc+5];
            _P94_ENSURE(d-2);
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { _sp=%d;\n"
                "      if(_RT->define_class(ctx,&_tsv%d,&_tsv%d,%uu,%d,var_refs)<0) goto _ex;\n"
                "      _sp=%d; }\n",
                d-2, d-2, d-1, atom, class_flags, d);
            break;
        }

        /* OP_define_class_computed: key(d-3) parent(d-2) bfunc(d-1) → key ctor proto. Net 0.
         * Operands: atom u32, class_flags u8. */
        case OP_define_class_computed: {
            uint32_t atom       = bc_u32(&bc[pc+1]);
            int      class_flags = (int)bc[pc+5];
            _P94_ENSURE(d-3);
            _P94_ENSURE(d-2);
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { _sp=%d;\n"
                "      if(_RT->define_class_computed(ctx,&_tsv%d,&_tsv%d,&_tsv%d,%uu,%d,var_refs)<0) goto _ex;\n"
                "      _sp=%d; }\n",
                d-2, d-3, d-2, d-1, atom, class_flags, d);
            break;
        }

        /* ---- P27: dynamic import ---- */

        /* OP_import: specifier(d-2) options(d-1) → promise(d-2). Net -1. */
        case OP_import: {
            _P94_ENSURE(d-2);
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { JSValue _spec=_tsv%d,_opts=_tsv%d,_r;\n"
                "      _r=_RT->import_op(ctx,_spec,_opts);\n"
                "      _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                d-2, d-1, d-2, d-2, d-1);
            break;
        }

        /* ---- P30: with_* — object-environment lookup with conditional PC jump ---- */

        /* Shared codegen pattern for all with_* opcodes:
         *   1. Check HasProperty + optionally @@unscopables (via _RT->with_has).
         *   2. If found: perform per-opcode action, then goto _L<TARGET>.
         *   3. If not found: pop obj, fall through.
         *
         * Bytecode format: op(1) atom(4) diff(4) is_with(1) = 10 bytes.
         * Jump target = pc + 5 + diff (matches interpreter's pc-after-operands + (diff-5)). */

        case OP_with_get_var: {
            /* obj(d-1) → val(d-1) if found (in-place), else pop obj.
             * Jump diff is measured from atom-start byte (pc+1 in JIT). */
            uint32_t atom   = bc_u32(&bc[pc+1]);
            int32_t  diff   = (int32_t)bc_u32(&bc[pc+5]);
            int      is_with = (int)bc[pc+9];
            int      target  = pc + 5 + diff;
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { int _wh=_RT->with_has(ctx,_tsv%d,%uu,%d);\n"
                "      if(_wh<0) goto _ex;\n"
                "      if(_wh){\n"
                "        if(_RT->with_get_var(ctx,&_tsv%d,%uu)<0) goto _ex;\n"
                "        goto _L%d;\n"
                "      }\n"
                "      _FREE(_tsv%d); _sp=%d; }\n",
                d-1, atom, is_with,
                d-1, atom,
                target,
                d-1, d-1);
            break;
        }

        case OP_with_put_var: {
            /* val(d-2) obj(d-1) → if found: set val on obj, pop both (sp=d-2); jump.
             *                     if not found: pop obj only (sp=d-1). */
            uint32_t atom   = bc_u32(&bc[pc+1]);
            int32_t  diff   = (int32_t)bc_u32(&bc[pc+5]);
            int      is_with = (int)bc[pc+9];
            int      target  = pc + 5 + diff;
            _P94_ENSURE(d-2); /* val must be boxed before passing to SetProperty */
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { int _wh=_RT->with_has(ctx,_tsv%d,%uu,%d);\n"
                "      if(_wh<0) goto _ex;\n"
                "      if(_wh){\n"
                "        int _r=_RT->with_put_var(ctx,_tsv%d,%uu,_tsv%d);\n"
                "        _FREE(_tsv%d); _sp=%d;\n"
                "        if(_r<0) goto _ex;\n"
                "        goto _L%d;\n"
                "      }\n"
                "      _FREE(_tsv%d); _sp=%d; }\n",
                d-1, atom, is_with,
                d-1, atom, d-2,
                d-1, d-2,
                target,
                d-1, d-1);
            break;
        }

        case OP_with_delete_var: {
            /* obj(d-1) → bool(d-1) if found; else pop obj. */
            uint32_t atom   = bc_u32(&bc[pc+1]);
            int32_t  diff   = (int32_t)bc_u32(&bc[pc+5]);
            int      is_with = (int)bc[pc+9];
            int      target  = pc + 5 + diff;
            _P94_ENSURE(d-1);
            jit_buf_printf(cb,
                "    { int _wh=_RT->with_has(ctx,_tsv%d,%uu,%d);\n"
                "      if(_wh<0) goto _ex;\n"
                "      if(_wh){\n"
                "        int _r=_RT->with_delete_var(ctx,_tsv%d,%uu);\n"
                "        if(_r<0) goto _ex;\n"
                "        _FREE(_tsv%d); _tsv%d=JS_NewBool(ctx,_r);\n"
                "        goto _L%d;\n"
                "      }\n"
                "      _FREE(_tsv%d); _sp=%d; }\n",
                d-1, atom, is_with,
                d-1, atom,
                d-1, d-1,
                target,
                d-1, d-1);
            break;
        }

        case OP_with_make_ref: {
            /* obj(d-1) → obj(d-1) atom_val(d) if found (+1); else pop obj. */
            uint32_t atom   = bc_u32(&bc[pc+1]);
            int32_t  diff   = (int32_t)bc_u32(&bc[pc+5]);
            int      is_with = (int)bc[pc+9];
            int      target  = pc + 5 + diff;
            _P94_ENSURE(d-1);
            _P94_ENSURE(d);
            jit_buf_printf(cb,
                "    { int _wh=_RT->with_has(ctx,_tsv%d,%uu,%d);\n"
                "      if(_wh<0) goto _ex;\n"
                "      if(_wh){\n"
                "        _tsv%d=_RT->with_make_ref(ctx,%uu); _sp=%d;\n"
                "        goto _L%d;\n"
                "      }\n"
                "      _FREE(_tsv%d); _sp=%d; }\n",
                d-1, atom, is_with,
                d, atom, d+1,
                target,
                d-1, d-1);
            break;
        }

        case OP_with_get_ref: {
            /* obj(d-1) → obj(d-1) method_val(d) if found (+1); else pop obj. */
            uint32_t atom   = bc_u32(&bc[pc+1]);
            int32_t  diff   = (int32_t)bc_u32(&bc[pc+5]);
            int      is_with = (int)bc[pc+9];
            int      target  = pc + 5 + diff;
            _P94_ENSURE(d-1);
            _P94_ENSURE(d);
            jit_buf_printf(cb,
                "    { int _wh=_RT->with_has(ctx,_tsv%d,%uu,%d);\n"
                "      if(_wh<0) goto _ex;\n"
                "      if(_wh){\n"
                "        JSValue _gref=_RT->with_get_ref(ctx,_tsv%d,%uu);\n"
                "        if(JS_IsException(_gref)) goto _ex;\n"
                "        _tsv%d=_gref; _sp=%d;\n"
                "        goto _L%d;\n"
                "      }\n"
                "      _FREE(_tsv%d); _sp=%d; }\n",
                d-1, atom, is_with,
                d-1, atom,
                d, d+1,
                target,
                d-1, d-1);
            break;
        }

        /* ---- Unsupported opcodes (caught in scan, but defensive) ---- */
        default:
            fprintf(stderr, "[JIT] gen_body: unhandled opcode 0x%02x at pc=%d\n", op, pc);
            *unsupported_out = 1;
            free(gen_st);
            free(gen_hsh);
            return -1;
        }

        /* Phase 6.1: update gen-time type stack.
         * Comparison opcodes (OP_lt..OP_strict_neq) already updated gen_st
         * inline above.  All other opcodes are handled here.              */
        {
            uint8_t  _gs_push = 255; /* 255 = no push */
            int      _gs_drop = 0;
            int      _gs_push_n = 1; /* number of times to push _gs_push (1 = normal) */
            uint64_t _gs_hash = 0;   /* P10.3: bc_hash for JIT_T_JIT_FUNC slots */

            switch (op) {
            /* --- Numeric constant pushes → INT (all are integer literals) --- */
            case OP_push_i32: case OP_push_i8: case OP_push_i16:
            case OP_push_0:   case OP_push_1:  case OP_push_2:  case OP_push_3:
            case OP_push_4:   case OP_push_5:  case OP_push_6:  case OP_push_7:
            case OP_push_minus1:
                _gs_push = JIT_T_INT; break;

            /* --- Non-numeric pushes → JSVAL --- */
            case OP_push_false: case OP_push_true: case OP_push_empty_string:
            case OP_undefined:  case OP_null:      case OP_push_this:
            case OP_push_const: case OP_push_const8: case OP_push_atom_value:
                _gs_push = JIT_T_JSVAL; break;

            /* --- get_loc: propagate local's type --- */
            case OP_get_loc: case OP_get_loc_check: case OP_get_loc_checkthis:
                { int _i=(int)bc_u16(&bc[pc+1]);
                  _gs_push=(local_type&&_i<var_count)?local_type[_i]:JIT_T_JSVAL; break; }
            case OP_get_loc8:
                { int _i=(int)bc[pc+1];
                  _gs_push=(local_type&&_i<var_count)?local_type[_i]:JIT_T_JSVAL; break; }
            case OP_get_loc0: _gs_push=(local_type&&var_count>0)?local_type[0]:JIT_T_JSVAL; break;
            case OP_get_loc1: _gs_push=(local_type&&var_count>1)?local_type[1]:JIT_T_JSVAL; break;
            case OP_get_loc2: _gs_push=(local_type&&var_count>2)?local_type[2]:JIT_T_JSVAL; break;
            case OP_get_loc3: _gs_push=(local_type&&var_count>3)?local_type[3]:JIT_T_JSVAL; break;

            /* --- get_arg: always JSVAL (unknown call-site type) --- */
            case OP_get_arg:  case OP_get_arg0: case OP_get_arg1:
            case OP_get_arg2: case OP_get_arg3:
                _gs_push = JIT_T_JSVAL; break;

            /* --- get_var: SELF_FUNC if loading the function's own name,
             * JIT_FUNC if loading a known JIT callee, else JSVAL.
             * P8.2: marks the stack slot so OP_call* can emit a direct C call.
             * P10.3: marks the slot for known JIT callees; hash stored in gen_hsh. --- */
            case OP_get_var: {
                int _vi = (int)bc_u16(&bc[pc+1]);
                JSAtom _va = js_jit_fb_get_closure_var_atom(b, _vi);
                if (self_func_atom != JS_ATOM_NULL && _va == self_func_atom) {
                    _gs_push = JIT_T_SELF_FUNC;
                } else if (var_jit_hash && _vi < p103_cvc_gb && var_jit_hash[_vi]) {
                    _gs_push = JIT_T_JIT_FUNC;
                    _gs_hash = var_jit_hash[_vi];
                } else {
                    _gs_push = JIT_T_JSVAL;
                }
                break;
            }
            /* P49: get_var_ref* — INT hint → gen_st=INT so downstream ops use fast paths.
             * get_var_ref_check is excluded: its main-switch code always writes _tsv (not _ti),
             * so gen_st must be JSVAL regardless of the hint. */
            case OP_get_var_ref:
            case OP_get_var_ref0: case OP_get_var_ref1:
            case OP_get_var_ref2: case OP_get_var_ref3: {
                int _p49_gst = (vt_hints
                                && (n_gf + n_ae + n_pf + vr_idx) < (n_gf + n_ae + n_pf + n_vr)
                                && vt_hints[n_gf + n_ae + n_pf + vr_idx] == 0 /* JS_TAG_INT */)
                               ? JIT_T_INT : JIT_T_JSVAL;
                _gs_push = _p49_gst;
                vr_idx++;
                break;
            }
            case OP_get_var_ref_check:
                /* Always JSVAL: main switch emits TDZ check + _DUP into _tsv, never _ti. */
                _gs_push = JIT_T_JSVAL;
                vr_idx++;
                break;

            /* --- get_length: P11.8 — always INT (stored in _ti, not _tsv/_tsd) --- */
            case OP_get_length: _gs_drop=1; _gs_push=JIT_T_INT; break;

            /* --- pow: always JSVAL result (calls runtime, no typed fast path) --- */
            case OP_pow: _gs_drop=2; _gs_push=JIT_T_JSVAL; break;

            /* --- Arithmetic: INT if both INT (except div), NUMBER if both >=NUMBER, else JSVAL.
             * P44: sub/mul/div/mod half-typed (one NUMBER/INT, one JSVAL) → NUMBER (_tsd).
             * add stays JSVAL when not both numeric (add can produce strings).
             * P51: when both JSVAL but warm hints say both INT → speculative INT result. */
            case OP_add: {
                uint8_t _t2=_GS_TOP2(), _t1=_GS_TOP();
                _gs_drop = 2;
                if (_t2>=JIT_T_NUMBER && _t1>=JIT_T_NUMBER) {
                    _gs_push = (_t2==JIT_T_INT&&_t1==JIT_T_INT) ? JIT_T_INT : JIT_T_NUMBER;
                } else if (_t2==JIT_T_JSVAL && _t1==JIT_T_JSVAL) {
                    /* P51: check warm hints for this OP_add site (2 slots each). */
                    int _ad_base = n_gf + n_ae + n_pf + n_vr + n_pa + ad_idx * 2;
                    int _p51_int = (vt_hints
                                    && _ad_base + 1 < n_gf + n_ae + n_pf + n_vr + n_pa + n_ad * 2
                                    && vt_hints[_ad_base]   == 0 /* JS_TAG_INT */
                                    && vt_hints[_ad_base+1] == 0 /* JS_TAG_INT */);
                    _gs_push = _p51_int ? JIT_T_INT : JIT_T_JSVAL;
                } else {
                    _gs_push = JIT_T_JSVAL;
                }
                ad_idx++;
                break;
            }
            case OP_sub: case OP_mul: {
                uint8_t _t2=_GS_TOP2(), _t1=_GS_TOP();
                _gs_drop = 2;
                /* Half-typed (one JSVAL) → JSVAL result (gen_body writes _tsv). */
                int _t2n=(_t2>=JIT_T_NUMBER&&_t2<=JIT_T_INT), _t1n=(_t1>=JIT_T_NUMBER&&_t1<=JIT_T_INT);
                if (_t2n && _t1n)
                    _gs_push = (_t2==JIT_T_INT&&_t1==JIT_T_INT) ? JIT_T_INT : JIT_T_NUMBER;
                else
                    _gs_push = JIT_T_JSVAL;
                break;
            }
            case OP_mod: {
                uint8_t _t2=_GS_TOP2(), _t1=_GS_TOP();
                _gs_drop = 2;
                /* OP_mod P44 half-typed paths write result to _tsd (NUMBER), not _tsv.
                 * So any numeric input → NUMBER result, matching gen_body behaviour. */
                int _t2n=(_t2>=JIT_T_NUMBER&&_t2<=JIT_T_INT), _t1n=(_t1>=JIT_T_NUMBER&&_t1<=JIT_T_INT);
                if (_t2==JIT_T_INT && _t1==JIT_T_INT)
                    _gs_push = JIT_T_INT;
                else if (_t2n || _t1n)
                    _gs_push = JIT_T_NUMBER;
                else
                    _gs_push = JIT_T_JSVAL;
                break;
            }
            case OP_div: {
                uint8_t _t2=_GS_TOP2(), _t1=_GS_TOP();
                _gs_drop = 2;
                /* P11.6: div result is always NUMBER — _tsd holds the result.
                 * P44: half-typed path also produces NUMBER. */
                int _t2n=(_t2>=JIT_T_NUMBER&&_t2<=JIT_T_INT), _t1n=(_t1>=JIT_T_NUMBER&&_t1<=JIT_T_INT);
                _gs_push = (_t2n || _t1n) ? JIT_T_NUMBER : JIT_T_JSVAL;
                break;
            }
            /* --- Bitwise: and/or/xor/shl/sar/not always produce int32.
             * When both inputs are INT, emit native _ti path → push INT.
             * Otherwise fall back to _tsv tag-check path → push JSVAL.
             * shr (>>>) result may exceed INT32_MAX → always JSVAL. */
            case OP_and: case OP_or: case OP_xor:
            case OP_shl: case OP_sar: {
                uint8_t _t2=_GS_TOP2(), _t1=_GS_TOP();
                _gs_drop=2;
                _gs_push=(_t2==JIT_T_INT&&_t1==JIT_T_INT)?JIT_T_INT:JIT_T_JSVAL;
                break;
            }
            case OP_shr: _gs_drop=2; _gs_push=JIT_T_JSVAL; break;
            case OP_not: {
                _gs_drop=1;
                _gs_push=(_GS_TOP()==JIT_T_INT)?JIT_T_INT:JIT_T_JSVAL;
                break;
            }

            /* --- Unary numeric: INT/NUMBER if operand was numeric, else JSVAL --- */
            case OP_neg: case OP_plus: case OP_inc: case OP_dec:
                { uint8_t _t=_GS_TOP();
                  _gs_drop=1;
                  _gs_push=(_t>=JIT_T_NUMBER)?_t:JIT_T_JSVAL; break; }
            /* post_inc/dec: pop 1 (original), push 2 (original + result).
             * We set _gs_drop=1 and push the result type; the original is
             * re-pushed via a special case in the application block below. */
            case OP_post_inc: case OP_post_dec:
                { uint8_t _t=_GS_TOP();
                  _gs_drop=1;
                  _gs_push=(_t>=JIT_T_NUMBER)?_t:JIT_T_JSVAL; break; }

            /* --- Comparisons: handled inline in main switch above --- */
            case OP_lt:  case OP_lte: case OP_gt:  case OP_gte:
            case OP_eq:  case OP_neq: case OP_strict_eq: case OP_strict_neq:
                break; /* already updated in main switch */

            /* --- Boolean / typeof --- */
            case OP_lnot: case OP_typeof: _gs_drop=1; _gs_push=JIT_T_JSVAL; break;
            case OP_instanceof: case OP_in: _gs_drop=2; _gs_push=JIT_T_JSVAL; break;

            /* --- put_loc: pop 1 --- */
            case OP_put_loc: case OP_put_loc_check: case OP_put_loc_check_init:
            case OP_put_loc8: case OP_put_loc0: case OP_put_loc1:
            case OP_put_loc2: case OP_put_loc3:
                _gs_drop = 1; break;
            /* set_loc: peek, no pop */

            /* --- put_arg: pop 1 --- */
            case OP_put_arg:  case OP_put_arg0: case OP_put_arg1:
            case OP_put_arg2: case OP_put_arg3: _gs_drop=1; break;

            /* --- put_var_ref / put_var: pop 1 --- */
            case OP_put_var_ref:  case OP_put_var_ref_check:
            case OP_put_var_ref_check_init:
            case OP_put_var_ref0: case OP_put_var_ref1:
            case OP_put_var_ref2: case OP_put_var_ref3:
                /* P52: track pv_idx for old-value hint indexing. */
                _gs_drop = 1; pv_idx++; break;
            case OP_set_var_ref:  case OP_set_var_ref0: case OP_set_var_ref1:
            case OP_set_var_ref2: case OP_set_var_ref3:
                /* P52: set_var_ref peeks (no drop) but still uses GEN_SET_VR hint. */
                pv_idx++; break;
            case OP_put_var: case OP_put_var_init:
                _gs_drop = 1; break;

            /* --- dup: propagate the typed type of top slot (P9.4) --- */
            case OP_dup: _gs_push = _GS_TOP(); break;

            /* --- drop: pop 1 --- */
            case OP_drop: _gs_drop=1; break;

            /* --- add_loc: pop 1 (modifies local in-place) --- */
            case OP_add_loc: _gs_drop=1; break;

            /* --- Calls: pop n+1 (or n+2 for method), push JSVAL --- */
            case OP_call: case OP_tail_call:
                { int _n=(int)bc_u16(&bc[pc+1]); _gs_drop=_n+1; _gs_push=JIT_T_JSVAL; break; }
            case OP_call0: _gs_drop=1; _gs_push=JIT_T_JSVAL; break;
            case OP_call1: _gs_drop=2; _gs_push=JIT_T_JSVAL; break;
            case OP_call2: _gs_drop=3; _gs_push=JIT_T_JSVAL; break;
            case OP_call3: _gs_drop=4; _gs_push=JIT_T_JSVAL; break;
            case OP_call_method: case OP_tail_call_method:
                { int _n=(int)bc_u16(&bc[pc+1]); _gs_drop=_n+2; _gs_push=JIT_T_JSVAL; break; }
            case OP_call_constructor:
                { int _n=(int)bc_u16(&bc[pc+1]); _gs_drop=_n+2; _gs_push=JIT_T_JSVAL; break; }

            /* --- P13: fclosure → pushes a closure object (JSVAL) --- */
            case OP_fclosure:  _gs_push=JIT_T_JSVAL; break;
            case OP_fclosure8: _gs_push=JIT_T_JSVAL; break;
            case OP_set_name: break; /* 1-in 1-out, no stack change */

            /* --- P14: try/catch/finally --- */
            case OP_catch:     _gs_push=JIT_T_JSVAL; break; /* push placeholder */
            case OP_gosub:     _gs_push=JIT_T_JSVAL; break; /* push return addr */
            case OP_ret:       gen_sp=0; break;              /* jumps away */
            case OP_nip_catch: gen_sp=0; break;              /* conservative reset */

            /* --- P15: iterators --- */
            case OP_for_in_start: break;                              /* 1-in 1-out */
            case OP_for_in_next:
                _gs_push=JIT_T_JSVAL; _gs_push_n=2; break;           /* +2 */
            case OP_for_of_start:
                _gs_push=JIT_T_JSVAL; _gs_push_n=2; break;           /* +2 */
            case OP_for_of_next:
                _gs_push=JIT_T_JSVAL; _gs_push_n=2; break;           /* +2 */
            /* --- P30: async for-of --- */
            case OP_for_await_of_start:
                _gs_push=JIT_T_JSVAL; _gs_push_n=2; break;           /* +2 */
            case OP_for_await_of_next:
                _gs_push=JIT_T_JSVAL; break;                          /* +1 */
            /* --- P30: with_* — model the fall-through (not-found) path: pop obj --- */
            case OP_with_get_var:
            case OP_with_put_var:
            case OP_with_delete_var:
            case OP_with_make_ref:
            case OP_with_get_ref:
                _gs_drop=1; break;
            case OP_iterator_close:   _gs_drop=3; break;              /* -3 */
            case OP_iterator_check_object: break;                     /* 0 */
            case OP_iterator_get_value_done: _gs_push=JIT_T_JSVAL; break; /* +1 */
            case OP_iterator_next:    break;                          /* 0 */
            case OP_iterator_call:    _gs_push=JIT_T_JSVAL; break;   /* +1 */
            case OP_special_object:   _gs_push=JIT_T_JSVAL; break;   /* +1 */

            /* --- P16: delete / delete_var → bool (JSVAL) --- */
            case OP_delete:     _gs_drop=2; _gs_push=JIT_T_JSVAL; break;
            case OP_delete_var: _gs_push=JIT_T_JSVAL; break;

            /* --- P17: apply pops 3, apply_eval pops 2 → JSVAL --- */
            case OP_apply:      _gs_drop=3; _gs_push=JIT_T_JSVAL; break;
            case OP_apply_eval: _gs_drop=2; _gs_push=JIT_T_JSVAL; break;

            /* --- P18: type-test ops: 1-in, 1-out (JSVAL bool) --- */
            case OP_is_null: case OP_is_undefined: case OP_is_undefined_or_null:
            case OP_typeof_is_undefined: case OP_typeof_is_function:
                _gs_drop=1; _gs_push=JIT_T_JSVAL; break;
            /* P18/P9.4: stack-shuffle ops — must update gen_st[] for existing slots too.
             * P94_ENSURE (called in the codegen first-switch) boxes any INT/NUMBER
             * slot to _tsv before the shuffle.  After the shuffle, all involved
             * slots hold JSValues; mark them all JSVAL so the next opcode's
             * P94_ENSURE does not try to re-box a stale _ti value.             */
            /* --- depth-neutral rotations: mark all involved slots JSVAL --- */
            case OP_swap:   /* a b -> b a */
                if (gen_sp >= 2) {
                    gen_st[gen_sp-2] = JIT_T_JSVAL;
                    gen_st[gen_sp-1] = JIT_T_JSVAL;
                }
                break;
            case OP_rot3l:  /* a b c -> b c a */
            case OP_rot3r:  /* a b c -> c a b */
                if (gen_sp >= 3) {
                    gen_st[gen_sp-3] = JIT_T_JSVAL;
                    gen_st[gen_sp-2] = JIT_T_JSVAL;
                    gen_st[gen_sp-1] = JIT_T_JSVAL;
                }
                break;
            /* --- dup variants: mark source slots JSVAL, push JSVAL for new slots --- */
            case OP_dup1: { /* a b -> a a b (+1): a stays, dup(a) at d-1, b at d */
                if (gen_sp >= 2 && gen_sp < gen_stk_cap) {
                    /* _P94_ENSURE(d-2) and _P94_ENSURE(d-1) in the codegen section
                     * boxed BOTH a and b into their _tsv slots before the shuffle.
                     * After the shuffle, _tsv{d}=_tsv{d-1} holds b's JSValue.
                     * _ti{d} is stale — mark all three slots JSVAL so subsequent
                     * opcodes (sub/add INT fast paths, put_loc INT path, etc.) do
                     * not read the stale _ti{d} register. */
                    gen_st[gen_sp-2] = JIT_T_JSVAL; /* a: boxed by _P94_ENSURE */
                    gen_st[gen_sp-1] = JIT_T_JSVAL; /* dup(a) — always JSVAL */
                    gen_st[gen_sp]   = JIT_T_JSVAL; /* b: boxed by _P94_ENSURE, now in _tsv{d} */
                    gen_sp++;
                }
                break; }
            case OP_dup2:   /* a b -> a b a b (+2): mark originals JSVAL, push 2 */
                if (gen_sp >= 2) {
                    gen_st[gen_sp-2] = JIT_T_JSVAL;
                    gen_st[gen_sp-1] = JIT_T_JSVAL;
                }
                _gs_push=JIT_T_JSVAL; _gs_push_n=2; break;
            case OP_dup3:   /* a b c -> a b c a b c (+3): mark originals JSVAL, push 3 */
                if (gen_sp >= 3) {
                    gen_st[gen_sp-3] = JIT_T_JSVAL;
                    gen_st[gen_sp-2] = JIT_T_JSVAL;
                    gen_st[gen_sp-1] = JIT_T_JSVAL;
                }
                _gs_push=JIT_T_JSVAL; _gs_push_n=3; break;
            /* --- insert2: obj a -> a obj a (+1) --- */
            case OP_insert2: {
                if (gen_sp >= 2 && gen_sp < gen_stk_cap) {
                    /* dup(a) at d-2, obj at d-1, original a at d */
                    gen_st[gen_sp-2] = JIT_T_JSVAL; /* dup(a) — boxed */
                    gen_st[gen_sp-1] = JIT_T_JSVAL; /* obj (moved, boxed by _P94_ENSURE) */
                    gen_st[gen_sp]   = JIT_T_JSVAL; /* original a at top */
                    gen_sp++;
                }
                break; }
            /* --- nip: a b -> b (-1): mark result slot JSVAL, pop 1 --- */
            case OP_nip:
                if (gen_sp >= 2)
                    gen_st[gen_sp-2] = JIT_T_JSVAL; /* b moved down, boxed */
                _gs_drop = 1; break;
            case OP_nip1: { /* a b c -> b c: remove slot gen_sp-3, shift down */
                int s = gen_sp;
                if (s >= 3) {
                    gen_st[s-3] = gen_st[s-2];
                    gen_st[s-2] = gen_st[s-1];
                }
                gen_sp = (s >= 1) ? s-1 : 0;
                break; }
            /* insert3: obj prop a -> a obj prop a (+1). P94_ENSURE forces all to JSVAL. */
            case OP_insert3:
                if (gen_sp >= 3) {
                    gen_st[gen_sp-3] = JIT_T_JSVAL; /* dup(a) at bottom */
                    gen_st[gen_sp-2] = JIT_T_JSVAL; /* obj */
                    gen_st[gen_sp-1] = JIT_T_JSVAL; /* prop */
                    if (gen_sp < gen_stk_cap) {
                        gen_st[gen_sp] = JIT_T_JSVAL; /* original a at top */
                        gen_sp++;
                    }
                }
                break;
            /* insert4: this obj prop a -> a this obj prop a (+1) */
            case OP_insert4:
                if (gen_sp >= 4) {
                    gen_st[gen_sp-4] = JIT_T_JSVAL;
                    gen_st[gen_sp-3] = JIT_T_JSVAL;
                    gen_st[gen_sp-2] = JIT_T_JSVAL;
                    gen_st[gen_sp-1] = JIT_T_JSVAL;
                    if (gen_sp < gen_stk_cap) {
                        gen_st[gen_sp] = JIT_T_JSVAL;
                        gen_sp++;
                    }
                }
                break;
            /* perm3/4/5: cyclic rotation of 3/4/5 slots (depth-neutral). */
            case OP_perm3:
                if (gen_sp >= 3) {
                    gen_st[gen_sp-3] = JIT_T_JSVAL;
                    gen_st[gen_sp-2] = JIT_T_JSVAL;
                }
                break;
            case OP_perm4:
                if (gen_sp >= 4) {
                    gen_st[gen_sp-4] = JIT_T_JSVAL;
                    gen_st[gen_sp-3] = JIT_T_JSVAL;
                    gen_st[gen_sp-2] = JIT_T_JSVAL;
                }
                break;
            case OP_perm5:
                if (gen_sp >= 5) {
                    gen_st[gen_sp-5] = JIT_T_JSVAL;
                    gen_st[gen_sp-4] = JIT_T_JSVAL;
                    gen_st[gen_sp-3] = JIT_T_JSVAL;
                    gen_st[gen_sp-2] = JIT_T_JSVAL;
                }
                break;
            /* rot4l/rot5l: left-rotate 4/5 slots (depth-neutral). */
            case OP_rot4l:
                if (gen_sp >= 4) {
                    gen_st[gen_sp-4] = JIT_T_JSVAL;
                    gen_st[gen_sp-3] = JIT_T_JSVAL;
                    gen_st[gen_sp-2] = JIT_T_JSVAL;
                    gen_st[gen_sp-1] = JIT_T_JSVAL;
                }
                break;
            case OP_rot5l:
                if (gen_sp >= 5) {
                    gen_st[gen_sp-5] = JIT_T_JSVAL;
                    gen_st[gen_sp-4] = JIT_T_JSVAL;
                    gen_st[gen_sp-3] = JIT_T_JSVAL;
                    gen_st[gen_sp-2] = JIT_T_JSVAL;
                    gen_st[gen_sp-1] = JIT_T_JSVAL;
                }
                break;
            /* swap2: a b c d -> c d a b (depth-neutral). */
            case OP_swap2:
                if (gen_sp >= 4) {
                    gen_st[gen_sp-4] = JIT_T_JSVAL;
                    gen_st[gen_sp-3] = JIT_T_JSVAL;
                    gen_st[gen_sp-2] = JIT_T_JSVAL;
                    gen_st[gen_sp-1] = JIT_T_JSVAL;
                }
                break;

            /* --- Property / array access → JSVAL --- */
            case OP_put_field: {
                /* P48: drop 2 (obj + val), push nothing.
                 * Track pf_idx to match __jit_vt_ array indexing in main switch. */
                _gs_drop = 2;
                pf_idx++;
                break;
            }
            case OP_put_array_el: {
                /* P50: drop 3 (obj + idx + val), push nothing.
                 * Track pa_idx to match __jit_vt_ array indexing in main switch. */
                _gs_drop = 3;
                pa_idx++;
                break;
            }
            case OP_get_field: {
                int _p45b_gst = (vt_hints && gf_idx < n_gf && vt_hints[gf_idx] == 0)
                                ? JIT_T_INT : JIT_T_JSVAL;
                _gs_drop = 1; _gs_push = _p45b_gst;
                gf_idx++;
                break;
            }
            case OP_get_field2:              _gs_push=JIT_T_JSVAL; break;
            case OP_get_array_el: {
                int _idx_is_int = (gen_sp >= 1 && gen_st[gen_sp-1] == JIT_T_INT);
                int _p46_gst = (_idx_is_int && vt_hints
                                && (n_gf + ae_idx) < (n_gf + n_ae)
                                && vt_hints[n_gf + ae_idx] == 0)
                               ? JIT_T_INT : JIT_T_JSVAL;
                _gs_drop = 2; _gs_push = _p46_gst;
                ae_idx++;
                break;
            }
            case OP_object:                  _gs_push=JIT_T_JSVAL; break;
            case OP_array_from:
                { int _n=(int)bc_u16(&bc[pc+1]); _gs_drop=_n; _gs_push=JIT_T_JSVAL; break; }

            /* --- if_false/if_true: pop 1 (only reached when NOT fused) --- */
            case OP_if_false:  case OP_if_true:
            case OP_if_false8: case OP_if_true8: _gs_drop=1; break;

            /* --- return: clear stack --- */
            case OP_return: gen_sp=0; break;
            case OP_return_undef: gen_sp=0; break;
            case OP_throw: gen_sp=0; break;

            /* --- P12: generator/async opcodes (gen_st updated inline in main switch) --- */
            case OP_initial_yield: break;   /* no stack change, updated inline */
            case OP_yield: break;           /* updated inline in OP_yield case above */
            case OP_await: break;           /* updated inline in OP_await case above */
            case OP_yield_star: break;      /* updated inline in OP_yield_star case above */
            case OP_async_yield_star: break; /* updated inline in OP_async_yield_star case above */
            case OP_return_async: gen_sp=0; break;

            /* --- P19: utility ops --- */
            case OP_get_var_undef: _gs_push=JIT_T_JSVAL; break;
            case OP_throw_error: gen_sp=0; break; /* always jumps to _ex */
            case OP_to_object: case OP_to_propkey:
                _gs_drop=1; _gs_push=JIT_T_JSVAL; break;
            case OP_regexp: _gs_drop=2; _gs_push=JIT_T_JSVAL; break;
            case OP_set_name_computed: break; /* 2-in 2-out, no net change */
            case OP_set_proto: _gs_drop=1; break; /* 2-in 1-out */
            case OP_set_home_object: break; /* 2-in 2-out, no net change */
            case OP_get_array_el2: _gs_drop=1; _gs_push=JIT_T_JSVAL; break; /* net 0 */
            case OP_get_array_el3: _gs_push=JIT_T_JSVAL; break; /* net +1 */
            case OP_define_array_el: _gs_drop=1; break; /* net -1 */
            case OP_push_bigint_i32: _gs_push=JIT_T_JSVAL; break;
            case OP_close_loc: break; /* no stack effect */

            /* --- P20: ref-slot ops --- */
            /* make_*_ref: push 2 (obj, atom_val) */
            case OP_make_loc_ref: case OP_make_arg_ref:
            case OP_make_var_ref: case OP_make_var_ref_ref:
                _gs_push=JIT_T_JSVAL; _gs_push_n=2; break;
            case OP_get_ref_value: _gs_push=JIT_T_JSVAL; break; /* net +1 */
            case OP_put_ref_value: _gs_drop=3; break; /* net -3 */

            /* --- P21: spread / rest / copy --- */
            case OP_rest: _gs_push=JIT_T_JSVAL; break;
            case OP_append:
                /* Helper writes back updated JSValues to array/pos slots;
                 * invalidate any typed (int/float) tracking on those slots
                 * so _P94_ENSURE won't re-box stale _ti/_tsd values. */
                if (gen_sp >= 3) {
                    gen_st[gen_sp-3] = JIT_T_JSVAL; /* array slot */
                    gen_st[gen_sp-2] = JIT_T_JSVAL; /* pos slot */
                }
                _gs_drop = 1; break;
            case OP_copy_data_properties: break;

            /* --- P22: private fields --- */
            case OP_private_symbol: _gs_push=JIT_T_JSVAL; break;
            case OP_get_private_field: _gs_drop=2; _gs_push=JIT_T_JSVAL; break;
            case OP_put_private_field: _gs_drop=3; break;
            case OP_define_private_field: _gs_drop=2; break;
            case OP_private_in: _gs_drop=2; _gs_push=JIT_T_JSVAL; break;

            /* --- P23: OOP helpers --- */
            case OP_check_ctor_return: _gs_push=JIT_T_JSVAL; break;
            case OP_check_brand: break;
            case OP_add_brand: _gs_drop=2; break;
            case OP_get_super: _gs_drop=1; _gs_push=JIT_T_JSVAL; break;
            case OP_get_super_value: _gs_drop=3; _gs_push=JIT_T_JSVAL; break;
            case OP_put_super_value: _gs_drop=4; break;
            case OP_define_method: _gs_drop=1; break;
            case OP_define_method_computed: _gs_drop=2; break;

            /* --- P26: constructor / class-definition --- */
            case OP_check_ctor: break;                  /* net 0 */
            case OP_init_ctor: _gs_push=JIT_T_JSVAL; break; /* net +1 */
            case OP_define_class: break;                /* net 0 */
            case OP_define_class_computed: break;       /* net 0 */

            /* --- P27: dynamic import --- */
            case OP_import: _gs_drop=2; _gs_push=JIT_T_JSVAL; break;

            /* Everything else: no tracked stack effect (conservative) */
            default: break;
            }

            /* Apply drop then push */
            if (_gs_drop > 0) { gen_sp -= _gs_drop; if (gen_sp < 0) gen_sp = 0; }
            if (_gs_push != 255 && gen_sp < gen_stk_cap) {
                gen_st[gen_sp]  = _gs_push;
                gen_hsh[gen_sp] = _gs_hash;  /* P10.3: store callee hash (0 if not JIT_T_JIT_FUNC) */
                gen_sp++;
            }
            /* post_inc/dec: push a second time (result at bottom, original at top) */
            if ((op == OP_post_inc || op == OP_post_dec) && gen_sp < gen_stk_cap) {
                gen_st[gen_sp] = _gs_push;
                gen_hsh[gen_sp] = 0;
                gen_sp++;
            }
            /* P15: multi-push for iterator opcodes that push 2 JSVAL items */
            for (int _pi = 1; _pi < _gs_push_n && gen_sp < gen_stk_cap; _pi++) {
                gen_st[gen_sp]  = _gs_push;
                gen_hsh[gen_sp] = 0;
                gen_sp++;
            }
        }

        pc += sz;
    }

#undef _GS_PUSH
#undef _GS_POP
#undef _GS_TOP
#undef _GS_TOP2
#undef _GS_DROP
#undef _P94_ENSURE
#undef LNAME
#undef ANAME
#undef _CAP_LOC
#undef _CAP_ARG

    free(gen_st);
    free(gen_hsh);
    return 0;
}

/*
 * js_jit_gen_c() — top-level: run scan, then generate C source for b.
 * Returns 0 and fills cb on success.
 * Returns -1 on failure; if *unsupported!=0 the function is ineligible.
 */
static int js_jit_gen_c(JSFunctionBytecode *b, JSJITCodeBuf *cb,
                        char *fname_out, size_t fname_sz, int *unsupported,
                        const char *js_func_name, uint64_t bc_hash,
                        JSRuntime *rt, const uint64_t *var_jit_hash)
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
    int var_ref_count  = js_jit_fb_get_var_ref_count(b);

    int op_sz_count;
    const uint8_t *op_sz = js_jit_get_opcode_size_table(&op_sz_count);

    /* P12: collect per-yield-site stack depth using the stack_depth_tab.
     * yield_below[k] = number of live stack slots below the yielded value
     * at resume label _Lresume_k.  These must be saved/restored across yield. */
    if (sr.has_yield) {
        const uint16_t *sdt = js_jit_fb_get_stack_depth_tab(b);
        int yi = 0;
        int pc2 = 0;
        memset(sr.yield_below, 0, sizeof(sr.yield_below));
        sr.max_below_yield = 0;
        while (pc2 < bc_len && yi < 64) {
            int op2 = bc[pc2];
            if (op2 == OP_initial_yield) {
                /* resume label 0: no locals below */
                sr.yield_below[yi++] = 0;
            } else if (op2 == OP_yield || op2 == OP_await ||
                       op2 == OP_yield_star || op2 == OP_async_yield_star) {
                int d2 = (sdt && sdt[pc2] != 0xffff) ? (int)sdt[pc2] : 0;
                int below = d2 - 1; /* slots below the yielded value */
                if (below < 0) below = 0;
                sr.yield_below[yi++] = below;
                if (below > sr.max_below_yield) sr.max_below_yield = below;
            }
            pc2 += (op2 < op_sz_count) ? op_sz[op2] : 1;
        }
    }

    /* Phase 5: infer which locals are always numeric → use C double */
    uint8_t *local_type = jit_infer_types(bc, bc_len, op_sz, op_sz_count,
                                           var_count, stack_size);
    /* In generator/async functions, typed int/float locals (JIT_T_INT, JIT_T_NUMBER)
     * are C-stack variables that are NOT saved to JSJITGeneratorFrame.saved_lv[].
     * Only JSValue locals (_jsv_*) survive yield/await.  Force all locals to
     * JSVAL so they are correctly spilled and restored across yield boundaries. */
    if (sr.has_yield && local_type) {
        int _j;
        for (_j = 0; _j < var_count; _j++)
            local_type[_j] = JIT_T_JSVAL;
    }

    /* P12: force all generator locals to JSVAL so spill/restore is a simple
     * JSValue copy — avoids boxing/unboxing complexity for typed locals. */
    if (sr.has_yield && local_type) {
        for (int i = 0; i < var_count; i++)
            local_type[i] = JIT_T_JSVAL;
    }

    /* P13.3: force captured locals to JIT_T_JSVAL — typed opt breaks capture semantics */
    if (sr.has_fclosure && local_type) {
        for (int i = 0; i < var_count && i < 64; i++)
            if ((sr.captured_local_mask >> i) & 1)
                local_type[i] = JIT_T_JSVAL;
    }

    /* P9.1: build named variable table (fallback to numeric on alloc failure) */
    char **varnames = jit_build_varnames(rt, b, arg_count, var_count);
    if (!varnames && (arg_count + var_count) > 0) {
        scan_result_free(&sr);
        free(local_type);
        return -1;
    }

    if (jit_buf_init(cb) < 0) {
        scan_result_free(&sr);
        free(local_type);
        jit_free_varnames(varnames, arg_count + var_count);
        return -1;
    }

    /* P45b/P46/P48/P49/P50/P51/P52: count tracked opcodes for __jit_vt_ array. */
    int n_gf = 0, n_ae = 0, n_pf = 0, n_vr = 0, n_pa = 0, n_ad = 0, n_pv = 0;
    for (int _pc = 0; _pc < bc_len; ) {
        int _op = bc[_pc];
        if (_op == OP_get_field)    n_gf++;
        if (_op == OP_get_array_el) n_ae++;
        if (_op == OP_put_field)    n_pf++; /* P48 */
        /* P49: all get_var_ref variants */
        if (_op == OP_get_var_ref || _op == OP_get_var_ref_check ||
            _op == OP_get_var_ref0 || _op == OP_get_var_ref1 ||
            _op == OP_get_var_ref2 || _op == OP_get_var_ref3)
            n_vr++;
        if (_op == OP_put_array_el) n_pa++; /* P50 */
        if (_op == OP_add)          n_ad++; /* P51 */
        /* P52: all put/set_var_ref* variants (same as GEN_PUT_VR/GEN_SET_VR coverage) */
        if (_op == OP_put_var_ref  || _op == OP_put_var_ref_check ||
            _op == OP_put_var_ref_check_init ||
            _op == OP_put_var_ref0 || _op == OP_put_var_ref1 ||
            _op == OP_put_var_ref2 || _op == OP_put_var_ref3 ||
            _op == OP_set_var_ref  || _op == OP_set_var_ref0 ||
            _op == OP_set_var_ref1 || _op == OP_set_var_ref2 ||
            _op == OP_set_var_ref3)
            n_pv++;
        _pc += (_op < op_sz_count) ? op_sz[_op] : 1;
    }
    if (n_gf > 0 && n_gf <= 0xFFFF)
        js_jit_fb_set_n_gf(b, (uint16_t)n_gf);
    if (n_ae > 0 && n_ae <= 0xFF)
        js_jit_fb_set_n_ae(b, (uint8_t)n_ae);
    if (n_pf > 0 && n_pf <= 0xFFFF)
        js_jit_fb_set_n_pf(b, (uint16_t)n_pf); /* P48 */
    if (n_vr > 0 && n_vr <= 0xFFFF)
        js_jit_fb_set_n_vr(b, (uint16_t)n_vr); /* P49 */
    if (n_pa > 0 && n_pa <= 0xFFFF)
        js_jit_fb_set_n_pa(b, (uint16_t)n_pa); /* P50 */
    if (n_ad > 0 && n_ad <= 0xFFFF)
        js_jit_fb_set_n_ad(b, (uint16_t)n_ad); /* P51 */
    if (n_pv > 0 && n_pv <= 0xFFFF)
        js_jit_fb_set_n_pv(b, (uint16_t)n_pv); /* P52 */
    /* P45b: read val_tag hints set by js_jit_schedule_warm_recompile(). */
    const uint8_t *vt_hints = js_jit_fb_get_vt_hints(b);

    gen_preamble(cb, bc_hash, var_count, arg_count, stack_size,
                 closure_var_count, cpool_count, fname_out, fname_sz,
                 local_type, js_func_name, varnames, &sr, var_ref_count, b,
                 n_gf, n_ae, n_pf, n_vr, n_pa, n_ad, n_pv);

    int unsup = 0;
    if (gen_body(cb, bc, bc_len, &sr, op_sz, op_sz_count,
                 var_count, arg_count, stack_size, &unsup, local_type, b,
                 bc_hash, varnames, var_jit_hash, var_ref_count,
                 vt_hints, n_gf, n_ae, n_pf, n_vr, n_pa, n_ad, n_pv) < 0) {
        *unsupported = unsup;
        jit_buf_free(cb);
        scan_result_free(&sr);
        free(local_type);
        jit_free_varnames(varnames, arg_count + var_count);
        return -1;
    }

    gen_footer(cb, var_count, arg_count, varnames, stack_size, &sr, var_ref_count);

    scan_result_free(&sr);
    free(local_type);
    jit_free_varnames(varnames, arg_count + var_count);

    if (cb->error) {
        jit_buf_free(cb);
        return -1;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * P10.5 — LTO IC object: compile quickjs.c once with -flto -fvisibility=hidden
 *
 * When added to the --jit-link GCC command, GCC LTO can inline js_jit_ic_check
 * (and js_jit_ic_read/write, js_jit_array_get/set) directly into the JIT
 * functions, eliminating the function-call overhead on every property access.
 *
 * The compiled .o is cached in <cache_dir>/qjs_ic_<mtime>.o so it is only
 * rebuilt when quickjs.c changes.  Returns malloc'd path or NULL if
 * quickjs.c is not found / compilation fails (P10.4 behaviour is preserved).
 * ----------------------------------------------------------------------- */
#ifdef JIT_INCLUDE_DIR
static char *jit_ensure_lto_obj(void)
{
    char src[512];
    snprintf(src, sizeof(src), "%s/quickjs.c", JIT_INCLUDE_DIR);
    struct stat st;
    if (stat(src, &st) != 0) return NULL;  /* no source — skip P10.5 */

    /* Cache key: quickjs.c last-modified time (seconds) */
    char obj[620];
    snprintf(obj, sizeof(obj), "%s/qjs_ic_%016llx.o",
             jit_cache_dir, (unsigned long long)(uint64_t)st.st_mtime);
    if (access(obj, R_OK) == 0) return strdup(obj);  /* cache hit */

    fprintf(stderr, "[JIT] P10.5: compiling quickjs.c for LTO IC inlining...\n");

    /* Compile quickjs.c with -flto -fvisibility=hidden so GCC can inline all
     * IC helpers into the JIT functions at --jit-link time.  Hidden visibility
     * keeps quickjs symbols local to combined.so (no conflict with main binary). */
    char def_ver[]    = "-DCONFIG_VERSION=\"" CONFIG_VERSION "\"";
    char def_thresh[] = "-DJIT_THRESHOLD_GCC=" STRINGIFY(JIT_THRESHOLD_GCC);
    char *av[] = {
        "gcc",
        "-O2", "-flto", "-fPIC", "-fvisibility=hidden",
        "-Wno-array-bounds", "-Wno-format-truncation", "-fwrapv",
        "-D_GNU_SOURCE", "-DHAVE_CLOSEFROM", "-DCONFIG_JIT",
        def_ver, def_thresh,
        "-I", JIT_INCLUDE_DIR,
        "-c", "-o", obj, src,
        NULL
    };

    pid_t pid = fork();
    if (pid < 0) return NULL;
    if (pid == 0) {
        int fd = open("/tmp/qjs_jit_ic.log", O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        execvp("gcc", av);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[JIT] P10.5: quickjs.c LTO compile failed "
                "(see /tmp/qjs_jit_ic.log); continuing without IC inlining\n");
        return NULL;
    }
    fprintf(stderr, "[JIT] P10.5: LTO IC object ready → %s\n", obj);
    return strdup(obj);
}
#endif /* JIT_INCLUDE_DIR */

/* -----------------------------------------------------------------------
 * P10.2 — js_jit_link(): combine all per-function .c files into one .so
 *
 * Deduplicates the recorded hash list, resolves .c paths from cache, and
 * invokes GCC once with -O2 -flto -shared.  The output is written to
 * <cache_dir>/combined.so.
 *
 * Returns the number of functions linked, or -1 on error.
 * ----------------------------------------------------------------------- */
int js_jit_link(void)
{
    if (!jit_cache_enabled) {
        fprintf(stderr, "[JIT] --jit-link: cache not enabled\n");
        return -1;
    }

    /* Deduplicate hashes (simple O(n²) for typical sizes ≤ few thousand) */
    for (int i = 0; i < jit_link_hash_count; i++) {
        for (int j = i + 1; j < jit_link_hash_count; j++) {
            if (jit_link_hashes[j] == jit_link_hashes[i]) {
                /* Remove j by swapping with last; keep bytecodes in sync */
                --jit_link_hash_count;
                jit_link_hashes[j]    = jit_link_hashes[jit_link_hash_count];
                jit_link_bytecodes[j] = jit_link_bytecodes[jit_link_hash_count];
                j--;
            }
        }
    }

    /* Resolve .c paths for all hashes that have a cached source file.
     * P10.4: track c_hashes[] in parallel for manifest generation. */
    uint64_t *c_hashes = NULL;
    char **c_paths = NULL;
    int n_paths = 0;
    for (int i = 0; i < jit_link_hash_count; i++) {
        char *path = jit_cache_get_c_src(jit_link_hashes[i]);
        if (!path) continue;
        char **parr = realloc(c_paths, (size_t)(n_paths + 1) * sizeof(*parr));
        uint64_t *harr = realloc(c_hashes, (size_t)(n_paths + 1) * sizeof(*harr));
        if (!parr || !harr) {
            free(path);
            if (parr) c_paths  = parr;
            if (harr) c_hashes = harr;
            break;
        }
        c_paths  = parr;
        c_hashes = harr;
        c_paths[n_paths]  = path;
        c_hashes[n_paths] = jit_link_hashes[i];
        n_paths++;
    }

    if (n_paths == 0) {
        fprintf(stderr, "[JIT] --jit-link: no .c files found — "
                "run --jit-warmup first\n");
        free(c_hashes);
        free(c_paths);
        return 0;
    }

    /* P10.4: Generate manifest .c — extern decls + array + count.
     * This file is compiled together with the per-function .c files so the
     * linker can resolve the __jit_f_<hash> symbol references at link time. */
    JSJITCodeBuf mfst;
    char *mfst_path = NULL;
    if (jit_buf_init(&mfst) == 0) {
        /* Use absolute path so /tmp/ doesn't shadow the real header. */
#ifdef JIT_INCLUDE_DIR
        jit_buf_printf(&mfst, "#include \"%s/quickjs-jit.h\"\n", JIT_INCLUDE_DIR);
#else
        jit_buf_printf(&mfst, "#include \"quickjs-jit.h\"\n");
#endif
        /* Compatibility shim: old cached .c files call js_unlikely() as a
         * function (quickjs.h #undef's it at end-of-header, so the macro
         * is gone by the time those TUs emit code for the call site).
         * js_unlikely is placed AFTER the include because quickjs.h undefines
         * it there; js_likely is still a macro after the include so we must
         * NOT define a function with that name after the include. */
        jit_buf_printf(&mfst,
            "__attribute__((weak)) int js_unlikely(int x) { return x; }\n");
        /* Compatibility shim: JS_OrdinaryIsInstanceOf is static in quickjs.c
         * and therefore not exported from the qjs binary. Old cached .c files
         * (generated before P10.4) call it without a declaration, producing
         * `U JS_OrdinaryIsInstanceOf` in each function's .so which prevents
         * combined.so from loading. Provide a weak definition here so
         * combined.so is self-contained; it routes through the exported
         * js_jit_ordinary_instanceof wrapper.
         * New .c files generated by the current gen_body call
         * js_jit_ordinary_instanceof directly and do not need this shim. */
        jit_buf_printf(&mfst,
            "__attribute__((weak)) int JS_OrdinaryIsInstanceOf("
            "JSContext *ctx, JSValueConst val, JSValueConst obj)"
            " { return js_jit_ordinary_instanceof(ctx, val, obj); }\n");
        for (int i = 0; i < n_paths; i++) {
            jit_buf_printf(&mfst,
                "JSValue __jit_f_%016llx"
                "(JSContext*,JSValue,int,JSValue*,JSValue*,JSVarRef**);\n",
                (unsigned long long)c_hashes[i]);
        }
        jit_buf_printf(&mfst, "JSJITManifestEntry __jit_manifest[] = {\n");
        for (int i = 0; i < n_paths; i++) {
            jit_buf_printf(&mfst,
                "    { 0x%016llxULL, __jit_f_%016llx },\n",
                (unsigned long long)c_hashes[i],
                (unsigned long long)c_hashes[i]);
        }
        jit_buf_printf(&mfst, "};\n");
        jit_buf_printf(&mfst, "int __jit_manifest_count = %d;\n", n_paths);
        if (!mfst.error)
            mfst_path = jit_write_tmp(mfst.buf, ".c");
        jit_buf_free(&mfst);
    }

    char out_path[620];
    snprintf(out_path, sizeof(out_path), "%s/combined.so", jit_cache_dir);

    /* P10.5: compile quickjs.c to LTO object for IC inlining (cached) */
#ifdef JIT_INCLUDE_DIR
    char *lto_obj = jit_ensure_lto_obj();
#else
    char *lto_obj = NULL;
#endif

    /* Build argv for GCC (+1 for optional manifest file, +1 for lto_obj, +16 for fixed args) */
    int max_argc = n_paths + 24;
    char **argv = malloc((size_t)max_argc * sizeof(char *));
    if (!argv) goto oom;

    int argc = 0;
    argv[argc++] = "gcc";
    argv[argc++] = lto_obj ? "-O3" : "-O2";  /* P10.5: -O3 enables aggressive inlining */
    argv[argc++] = "-flto";
    argv[argc++] = "-shared";
    argv[argc++] = "-fPIC";
    argv[argc++] = "-DCONFIG_JIT";
#ifdef JIT_INCLUDE_DIR
    argv[argc++] = "-I";
    argv[argc++] = JIT_INCLUDE_DIR;
#endif
    for (int i = 0; i < n_paths; i++)
        argv[argc++] = c_paths[i];
    if (mfst_path)
        argv[argc++] = mfst_path;
    if (lto_obj)
        argv[argc++] = lto_obj;    /* P10.5: quickjs.c LTO object for IC inlining */
    argv[argc++] = "-o";
    argv[argc++] = out_path;
    argv[argc]   = NULL;

    fprintf(stderr, "[JIT] --jit-link: combining %d functions → %s\n",
            n_paths, out_path);

    pid_t pid = fork();
    if (pid < 0) goto fail;
    if (pid == 0) {
        int logfd = open("/tmp/qjs_jit_link.log",
                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd >= 0) { dup2(logfd, 1); dup2(logfd, 2); close(logfd); }
        execvp("gcc", argv);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

    int had_lto = (lto_obj != NULL);
    free(argv);
    free(lto_obj);
    if (mfst_path) { unlink(mfst_path); free(mfst_path); }
    free(c_hashes);
    for (int i = 0; i < n_paths; i++) free(c_paths[i]);
    free(c_paths);

    if (!ok) {
        fprintf(stderr, "[JIT] --jit-link: GCC failed "
                "(see /tmp/qjs_jit_link.log)\n");
        return -1;
    }
    fprintf(stderr, "[JIT] --jit-link: done (%d functions combined%s)\n",
            n_paths, had_lto ? ", with LTO IC inlining" : "");
    return n_paths;

oom:
fail:
    free(argv);
    free(lto_obj);
    if (mfst_path) { unlink(mfst_path); free(mfst_path); }
    free(c_hashes);
    for (int i = 0; i < n_paths; i++) free(c_paths[i]);
    free(c_paths);
    return -1;
}

/* -----------------------------------------------------------------------
 * P10.4 — combined.so installer
 *
 * After --jit-link produces combined.so, subsequent --jit-aot runs call
 * js_jit_install_combined_if_exists() to dlopen combined.so, read its
 * manifest, and patch each bytecode's jit_func pointer atomically.
 *
 * Handle management:
 *   - Per-bytecode .so handles (tier==2, handle!=NULL) are closed here.
 *   - The combined.so handle is stored in jit_combined_handle (global).
 *   - Per-bytecode handle is set to NULL so js_jit_free_bytecode won't
 *     double-close when the bytecode is eventually freed.
 *
 * Incrementality: js_jit_install_combined_if_exists() is called once per
 * script file load (eval_buf + quickjs-libc load()).  Scripts loaded via
 * load() compile their bytecodes AFTER the top-level script already called
 * install_combined.  We must re-scan on every call and patch any newly
 * discovered bytecodes.  The combined.so is opened once; the manifest is
 * cached in jit_combined_manifest/jit_combined_count; subsequent calls
 * skip bytecodes whose handle already equals jit_combined_handle.
 * ----------------------------------------------------------------------- */

static JSFunctionBytecode *jit_find_bytecode_by_hash(uint64_t hash)
{
    for (int i = 0; i < jit_link_hash_count; i++) {
        if (jit_link_hashes[i] == hash)
            return jit_link_bytecodes ? jit_link_bytecodes[i] : NULL;
    }
    return NULL;
}

/* Scan the manifest and patch any bytecodes not yet using combined.so.
 * Always returns the number of functions patched this call (≥ 0). */
static int jit_install_combined_pass(void)
{
    int installed = 0;
    for (int i = 0; i < jit_combined_count; i++) {
        JSFunctionBytecode *b = jit_find_bytecode_by_hash(jit_combined_manifest[i].bc_hash);
        if (!b) continue;
        void *old_handle = js_jit_fb_get_handle(b);
        if (old_handle == jit_combined_handle) continue;  /* already patched */
        uint8_t old_tier = js_jit_fb_get_tier(b);
        /* Install with handle=NULL so js_jit_free_bytecode skips it */
        js_jit_fb_set_bc_hash(b, jit_combined_manifest[i].bc_hash);
        /* P10.3: combined.so is always freshly compiled with current codegen */
        js_jit_fb_set_p103_safe(b, 1);
        js_jit_fb_set_func(b, jit_combined_manifest[i].func_ptr, NULL, 2);
        /* P36.1: register address for sampling profiler (combined-pass path) */
        jit_registry_add((uintptr_t)jit_combined_manifest[i].func_ptr,
                         jit_combined_manifest[i].bc_hash, "");
        if (old_tier == 2 && old_handle)
            dlclose(old_handle);
        installed++;
    }
    return installed;
}

/* Open combined.so and cache the manifest pointer WITHOUT installing anything.
 * Safe to call before js_jit_compile_all: enables the fast-path in
 * js_jit_queue_gcc that skips loading individual .so files for functions
 * already present in combined.so. */
int js_jit_preload_combined(void)
{
    if (!jit_cache_enabled || jit_combined_handle) return 0;
    char path[620];
    snprintf(path, sizeof(path), "%s/combined.so", jit_cache_dir);
    if (access(path, R_OK) != 0) return 0;
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) return -1;
    JSJITManifestEntry *manifest =
        (JSJITManifestEntry *)dlsym(handle, "__jit_manifest");
    int *count_ptr = (int *)dlsym(handle, "__jit_manifest_count");
    if (!manifest || !count_ptr) { dlclose(handle); return -1; }
    jit_combined_handle   = handle;
    jit_combined_manifest = manifest;
    jit_combined_count    = *count_ptr;
    return jit_combined_count;
}

int js_jit_install_combined_if_exists(void)
{
    if (!jit_cache_enabled) return 0;
    /* Open combined.so once (may already be open from js_jit_preload_combined) */
    if (js_jit_preload_combined() < 0) return -1;
    if (!jit_combined_handle) return 0;  /* no combined.so */

    int installed = jit_install_combined_pass();
    if (installed > 0)
        fprintf(stderr, "[JIT] install_combined: installed %d/%d functions\n",
                installed, jit_combined_count);
    return installed;
}

/* -----------------------------------------------------------------------
 * P36.1: JIT address range registry
 *
 * Maps installed JIT function addresses → bc_hash → sample_count.
 * Used by the P36.2 SIGPROF sampler: the signal handler binary-searches
 * this sorted array to identify which JIT function is executing.
 *
 * Synchronization: seqlock.
 *   Writers (install/remove, main thread): increment seqlock to odd,
 *   modify, increment to even.
 *   Readers (signal handler): retry if seqlock is odd or changes.
 *   Sample counters are incremented with __ATOMIC_RELAXED — no lock
 *   needed since each slot is owned by one entry and entries are never
 *   recycled while in the registry.
 * ----------------------------------------------------------------------- */

#define JIT_ADDR_REGISTRY_MAX 4096

typedef struct {
    uintptr_t  func_ptr;   /* start address of compiled JIT function      */
    uint64_t   bc_hash;    /* FNV-1a hash — key for profile output         */
    uint32_t   samples;    /* atomic sample counter; SIGPROF increments it */
    char       name[80];   /* JS function name for profile output          */
} JITAddrEntry;

static JITAddrEntry      jit_addr_registry[JIT_ADDR_REGISTRY_MAX];
static volatile int      jit_addr_count  = 0;
static volatile uint32_t jit_addr_seqlock = 0; /* even = stable, odd = writing */

static inline void jit_seqlock_write_begin(void)
{
    __atomic_add_fetch(&jit_addr_seqlock, 1, __ATOMIC_SEQ_CST);
}
static inline void jit_seqlock_write_end(void)
{
    __atomic_add_fetch(&jit_addr_seqlock, 1, __ATOMIC_SEQ_CST);
}
/* Returns the seq value at the start of a read window. */
static inline uint32_t jit_seqlock_read_begin(void)
{
    uint32_t s;
    do {
        s = __atomic_load_n(&jit_addr_seqlock, __ATOMIC_SEQ_CST);
    } while (s & 1); /* spin while a write is in progress */
    return s;
}
/* Returns 1 if the read window is stale and must be retried. */
static inline int jit_seqlock_retry(uint32_t s)
{
    return s != __atomic_load_n(&jit_addr_seqlock, __ATOMIC_SEQ_CST);
}

/* Add a newly installed JIT function to the registry.
 * Keeps the array sorted by func_ptr for binary search in the signal handler.
 * Called from js_jit_install_results() (main thread only). */
void jit_registry_add(uintptr_t func_ptr, uint64_t bc_hash, const char *name)
{
    int cnt = jit_addr_count;
    if (cnt >= JIT_ADDR_REGISTRY_MAX) {
        /* Registry full — this function will not be sampled. */
        return;
    }

    jit_seqlock_write_begin();

    /* Insertion sort: find position to keep array sorted by func_ptr. */
    int i = cnt;
    while (i > 0 && jit_addr_registry[i - 1].func_ptr > func_ptr) {
        jit_addr_registry[i] = jit_addr_registry[i - 1];
        i--;
    }
    jit_addr_registry[i].func_ptr = func_ptr;
    jit_addr_registry[i].bc_hash  = bc_hash;
    jit_addr_registry[i].samples  = 0;
    strncpy(jit_addr_registry[i].name, name ? name : "",
            sizeof(jit_addr_registry[i].name) - 1);
    jit_addr_registry[i].name[sizeof(jit_addr_registry[i].name) - 1] = '\0';
    jit_addr_count = cnt + 1;

    jit_seqlock_write_end();
}

/* Remove a JIT function from the registry (called before dlclose).
 * Called from js_jit_free_bytecode() (main thread only). */
void jit_registry_remove(uintptr_t func_ptr)
{
    jit_seqlock_write_begin();

    int cnt = jit_addr_count;
    for (int i = 0; i < cnt; i++) {
        if (jit_addr_registry[i].func_ptr == func_ptr) {
            for (int j = i; j < cnt - 1; j++)
                jit_addr_registry[j] = jit_addr_registry[j + 1];
            jit_addr_count = cnt - 1;
            break;
        }
    }

    jit_seqlock_write_end();
}

/* Look up sample count for a bc_hash.
 * Called from profile writer (main thread, after sampler stopped). */
uint32_t jit_registry_lookup_samples(uint64_t bc_hash)
{
    int cnt = jit_addr_count;
    for (int i = 0; i < cnt; i++)
        if (jit_addr_registry[i].bc_hash == bc_hash)
            return jit_addr_registry[i].samples;
    return 0;
}

/* Public accessors used by jit-tests. */
int js_jit_registry_count(void) { return jit_addr_count; }

/* Returns the maximum sample count across all registry entries.
 * Used by tests to verify the sampler accumulated at least one sample. */
uint32_t js_jit_registry_max_samples(void)
{
    uint32_t mx = 0;
    int cnt = jit_addr_count;
    for (int i = 0; i < cnt; i++) {
        uint32_t s = __atomic_load_n(&jit_addr_registry[i].samples, __ATOMIC_RELAXED);
        if (s > mx) mx = s;
    }
    return mx;
}

/* -----------------------------------------------------------------------
 * P36.2: SIGPROF sampling profiler
 *
 * Fires SIGPROF at <hz> Hz using ITIMER_PROF (CPU time: user+kernel).
 * The signal handler reads %rip / equivalent PC from ucontext_t, binary-
 * searches jit_addr_registry[], and atomically increments the sample counter
 * for the enclosing JIT function.
 *
 * The signal handler must be async-signal-safe:
 *   - no malloc, no stdio, no mutex
 *   - seqlock for registry read (spin + retry)
 *   - __ATOMIC_RELAXED increment of the sample counter
 * ----------------------------------------------------------------------- */

static int              jit_sampler_hz = 0;      /* 0 = stopped */
static struct sigaction jit_old_sigaction;
static struct itimerval jit_old_itimer;

static void jit_sigprof_handler(int sig, siginfo_t *si, void *ctx_raw)
{
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)ctx_raw;

    /* Extract program counter — platform-specific */
#if defined(__x86_64__) && defined(__linux__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__x86_64__) && defined(__APPLE__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext->__ss.__rip;
#elif defined(__aarch64__) && defined(__linux__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
#else
    (void)uc;
    return; /* unsupported platform: no-op */
#endif

    /* Binary-search registry for the largest func_ptr <= pc.
     * Use seqlock retry loop so we never read a partially-modified array. */
    int found = -1;
    uint32_t seq;
    do {
        seq = jit_seqlock_read_begin();
        int lo = 0, hi = jit_addr_count - 1;
        found = -1;
        while (lo <= hi) {
            int mid = (lo + hi) >> 1;
            if (jit_addr_registry[mid].func_ptr <= pc) {
                found = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
    } while (jit_seqlock_retry(seq));

    if (found >= 0) {
        __atomic_fetch_add(&jit_addr_registry[found].samples, 1,
                           __ATOMIC_RELAXED);
    }
}

void js_jit_sampler_start(int hz)
{
    if (jit_sampler_hz > 0)
        return; /* already running */
    if (hz <= 0)
        hz = 100;
    jit_sampler_hz = hz;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = jit_sigprof_handler;
    sa.sa_flags     = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, &jit_old_sigaction);

    long usec = 1000000L / hz;
    struct itimerval it;
    it.it_interval.tv_sec  = 0;
    it.it_interval.tv_usec = usec;
    it.it_value.tv_sec     = 0;
    it.it_value.tv_usec    = usec;
    setitimer(ITIMER_PROF, &it, &jit_old_itimer);
}

void js_jit_sampler_stop(void)
{
    if (jit_sampler_hz == 0)
        return; /* not running */
    /* Disable the timer first, then restore the old handler */
    struct itimerval zero = {{0,0},{0,0}};
    setitimer(ITIMER_PROF, &zero, NULL);
    sigaction(SIGPROF, &jit_old_sigaction, NULL);
    jit_sampler_hz = 0;
}

int js_jit_sampler_hz(void) { return jit_sampler_hz; }

/* -----------------------------------------------------------------------
 * P35.5: call-count profile writer
 * ----------------------------------------------------------------------- */

typedef struct {
    FILE       *f;
    int         first;
    JSRuntime  *rt;
    int         hz;        /* P36.3: sampler hz; 0 = no timing data */
    /* P36.4: track hashes output by module walk for registry dedup pass */
    uint64_t   *seen;
    int         seen_count;
    int         seen_cap;
} ProfileWalkState;

static void profile_walk_cb(JSFunctionBytecode *b, void *opaque)
{
    ProfileWalkState *st = (ProfileWalkState *)opaque;
    int      calls   = js_jit_fb_get_call_count(b);
    uint64_t hash    = js_jit_hash_bytecode_pub(b);
    uint32_t samples = 0;
    uint32_t time_ms = 0;

    if (st->hz > 0) {
        /* P36.3: timed profile — look up sample count */
        samples = jit_registry_lookup_samples(hash);
        time_ms = (samples * 1000u) / (uint32_t)st->hz;
    }

    if (calls <= 0 && samples == 0)
        return; /* nothing recorded */

    /* P36.4: track hashes output by module walk so registry pass can skip them */
    if (st->seen) {
        if (st->seen_count == st->seen_cap) {
            int nc = st->seen_cap ? st->seen_cap * 2 : 16;
            uint64_t *na = realloc(st->seen, (size_t)nc * sizeof(*na));
            if (na) { st->seen = na; st->seen_cap = nc; }
        }
        if (st->seen_count < st->seen_cap)
            st->seen[st->seen_count++] = hash;
    }

    const char *name = js_jit_fb_get_func_name(st->rt, b);

    /* Escape the name: replace '"' and '\' (names are identifiers, rare) */
    char safe_name[256];
    if (name) {
        int i = 0;
        while (name[i] && i < (int)(sizeof(safe_name) - 1)) {
            safe_name[i] = (name[i] == '"' || name[i] == '\\') ? '_' : name[i];
            i++;
        }
        safe_name[i] = '\0';
    } else {
        safe_name[0] = '\0';
    }

    if (!st->first)
        fprintf(st->f, ",\n");

    if (st->hz > 0) {
        fprintf(st->f,
            "  {\"hash\":\"%016llx\",\"calls\":%d,"
            "\"time_ms\":%u,\"name\":\"%s\"}",
            (unsigned long long)hash, calls, time_ms, safe_name);
    } else {
        fprintf(st->f,
            "  {\"hash\":\"%016llx\",\"calls\":%d,\"name\":\"%s\"}",
            (unsigned long long)hash, calls, safe_name);
    }
    st->first = 0;
}

int js_jit_write_profile(JSContext *ctx, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "{\"functions\":[\n");
    ProfileWalkState st = { f, 1, JS_GetRuntime(ctx), 0, NULL, 0, 0 };
    js_jit_walk_all_modules(ctx, profile_walk_cb, &st);
    fprintf(f, "\n]}\n");
    fclose(f);
    return 0;
}

int js_jit_write_profile_timed(JSContext *ctx, const char *path, int hz)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "{\"functions\":[\n");

    /* Allocate initial seen-hash tracking buffer (grows on demand in walk_cb) */
    uint64_t *seen_buf = malloc(16 * sizeof(uint64_t));
    ProfileWalkState st = { f, 1, JS_GetRuntime(ctx), hz,
                            seen_buf, 0, seen_buf ? 16 : 0 };
    js_jit_walk_all_modules(ctx, profile_walk_cb, &st);

    /* P36.4: second pass — emit global-script functions found in the registry
     * but NOT in any loaded module (they have samples > 0 but no module entry). */
    int cnt = jit_addr_count;
    for (int i = 0; i < cnt; i++) {
        uint32_t s = __atomic_load_n(&jit_addr_registry[i].samples, __ATOMIC_RELAXED);
        if (s == 0) continue;
        uint64_t h = jit_addr_registry[i].bc_hash;
        /* Skip hashes already output by the module walk */
        int already = 0;
        for (int j = 0; j < st.seen_count; j++) {
            if (st.seen[j] == h) { already = 1; break; }
        }
        if (already) continue;
        uint32_t tm = hz > 0 ? (s * 1000u) / (uint32_t)hz : 0;
        if (!st.first) fprintf(f, ",\n");
        const char *n = jit_addr_registry[i].name;
        char safe_name[sizeof(jit_addr_registry[i].name)];
        int ni = 0;
        while (n[ni] && ni < (int)(sizeof(safe_name) - 1)) {
            safe_name[ni] = (n[ni] == '"' || n[ni] == '\\') ? '_' : n[ni];
            ni++;
        }
        safe_name[ni] = '\0';
        fprintf(f, "  {\"hash\":\"%016llx\",\"calls\":0,\"time_ms\":%u,\"name\":\"%s\"}",
                (unsigned long long)h, tm,
                safe_name[0] ? safe_name : "<anon>");
        st.first = 0;
    }
    free(st.seen);

    fprintf(f, "\n]}\n");
    fclose(f);
    return 0;
}

/* -----------------------------------------------------------------------
 * Cleanup hook called from free_function_bytecode()
 * ----------------------------------------------------------------------- */

void js_jit_free_bytecode(JSFunctionBytecode *b)
{
    /* Mark this bytecode dead in the session map so js_jit_install_results()
     * does not try to write into freed memory if a GCC result arrives later. */
    jit_session_remove(b);

    uint8_t tier       = js_jit_fb_get_tier(b);
    void   *handle     = js_jit_fb_get_handle(b);
    void   *warm_handle = js_jit_fb_get_warm_handle(b);

    /* P36.1: remove from sampling registry before releasing .so */
    JSJITFunc func = js_jit_fb_get_func(b);
    js_jit_fb_clear_handles(b);
    js_jit_fb_set_warm_handle(b, NULL); /* clear warm handle field */

    if (tier == 2 && handle) {
        if (func)
            jit_registry_remove((uintptr_t)func);
        dlclose(handle);
    }
    /* P45b: close warm .so (different handle from cold .so).
     * The cold .so was already closed above; warm .so is independent. */
    if (warm_handle && warm_handle != handle)
        dlclose(warm_handle);
    /* P45b: free val_tag hints buffer (malloc'd in js_jit_schedule_warm_recompile) */
    uint8_t *vt_hints = js_jit_fb_get_vt_hints(b);
    if (vt_hints) {
        js_jit_fb_set_vt_hints(b, NULL);
        free(vt_hints);
    }
}

#endif /* CONFIG_JIT */
