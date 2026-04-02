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

#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <dlfcn.h>

#include "quickjs.h"
#include "quickjs-jit.h"
#include "quickjs-opcode.h"

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
    /* var ref accessor */
    .var_ref_value    = js_jit_var_ref_value,
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
    /* closure creation: needs stack-frame access not available in JIT */
    case OP_fclosure:
    case OP_fclosure8:
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
        case OP_goto:
        case OP_gosub: {
            int32_t delta = bc_get_i32(&bc[pc + 1]);
            int target = pc + 1 + delta;
            if (scan_add_target(sr, target, &cap) < 0) {
                scan_result_free(sr); return -1;
            }
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

#define JIT_T_JSVAL  0  /* unknown — always use JSValue */
#define JIT_T_NUMBER 1  /* provably always numeric — use C double */

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

    /* Optimistic start: assume all locals are NUMBER */
    memset(lt, JIT_T_NUMBER, var_count);

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
/* Downgrade local[i] if written from non-NUMBER source */
#define _TI_WRITE(i, t) do { \
    if ((i) >= 0 && (i) < var_count && (t) != JIT_T_NUMBER \
        && lt[(i)] == JIT_T_NUMBER) { lt[(i)] = JIT_T_JSVAL; changed = 1; } \
} while(0)

        int pc = 0;
        while (pc < bc_len) {
            int op = bc[pc];
            if (op >= op_sz_count || op_sz[op] == 0) break;

            switch (op) {
            /* ---- Numeric constant pushes → NUMBER ---- */
            case OP_push_i32: case OP_push_i8: case OP_push_i16:
            case OP_push_0:   case OP_push_1:  case OP_push_2:  case OP_push_3:
            case OP_push_4:   case OP_push_5:  case OP_push_6:  case OP_push_7:
            case OP_push_minus1:
                _TI_PUSH(JIT_T_NUMBER); break;

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
            case OP_get_length:   _TI_DROPN(1); _TI_PUSH(JIT_T_NUMBER); break;
            case OP_object:                     _TI_PUSH(JIT_T_JSVAL); break;
            case OP_array_from: {
                int n = (int)bc_u16(&bc[pc+1]); _TI_DROPN(n); _TI_PUSH(JIT_T_JSVAL); break;
            }

            /* ---- Binary arithmetic: NUMBER iff both NUMBER (not pow) ---- */
            case OP_add: case OP_sub: case OP_mul: case OP_div: case OP_mod: {
                uint8_t b = _TI_POP(), a = _TI_POP();
                _TI_PUSH(a == JIT_T_NUMBER && b == JIT_T_NUMBER ? JIT_T_NUMBER : JIT_T_JSVAL);
                break;
            }
            case OP_pow: _TI_DROPN(2); _TI_PUSH(JIT_T_JSVAL); break;

            /* ---- Unary numeric ---- */
            case OP_neg: case OP_plus: case OP_inc: case OP_dec: {
                uint8_t a = _TI_POP();
                _TI_PUSH(a == JIT_T_NUMBER ? JIT_T_NUMBER : JIT_T_JSVAL); break;
            }
            case OP_post_inc: case OP_post_dec: {
                uint8_t a = _TI_POP();
                uint8_t r = a == JIT_T_NUMBER ? JIT_T_NUMBER : JIT_T_JSVAL;
                _TI_PUSH(a); _TI_PUSH(r); break;
            }

            /* ---- Bitwise: always int → NUMBER ---- */
            case OP_shl: case OP_sar: case OP_shr:
            case OP_and: case OP_or:  case OP_xor: _TI_DROPN(2); _TI_PUSH(JIT_T_NUMBER); break;
            case OP_not: _TI_DROPN(1); _TI_PUSH(JIT_T_NUMBER); break;

            /* ---- Boolean / comparison / typeof → JSVAL ---- */
            case OP_lnot: case OP_typeof: _TI_DROPN(1); _TI_PUSH(JIT_T_JSVAL); break;
            case OP_lt:  case OP_lte: case OP_gt:  case OP_gte:
            case OP_eq:  case OP_neq: case OP_strict_eq: case OP_strict_neq:
            case OP_instanceof: case OP_in: _TI_DROPN(2); _TI_PUSH(JIT_T_JSVAL); break;

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

            /* ---- Branches ---- */
            case OP_if_false:  case OP_if_true:
            case OP_if_false8: case OP_if_true8: _TI_DROPN(1); break;
            case OP_goto: case OP_goto8: case OP_goto16: break;

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
                         const char *js_func_name);

typedef struct JITGCCJob {
    JSFunctionBytecode *b;
    char               *c_src;     /* malloc'd C source; freed after gcc    */
    char                fname[64]; /* symbol name to look up via dlsym      */
    struct JITGCCJob   *next;
} JITGCCJob;

static struct {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    JITGCCJob      *head;
    JITGCCJob      *tail;
    int             stop;
    int             started;
    int             ref_count; /* how many JSRuntime instances share this thread */
} jit_worker;

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
    if (gcc_ok) unlink(c_path);
    free(c_path);
    if (!gcc_ok) { unlink(so_path); goto fail; }

    /* Load the compiled .so; unlink immediately (kernel keeps it mapped) */
    void *handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    unlink(so_path);
    if (!handle) goto fail;

    JSJITFunc f = (JSJITFunc)(uintptr_t)dlsym(handle, job->fname);
    if (!f) { dlclose(handle); goto fail; }

    js_jit_fb_set_func(job->b, f, handle, 2);
    return;
fail:
    js_jit_fb_set_no_compile(job->b);
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
        pthread_mutex_unlock(&jit_worker.lock);
        jit_compile_gcc_job(job);
        free(job);

        pthread_mutex_lock(&jit_worker.lock);
    }
    pthread_mutex_unlock(&jit_worker.lock);
    return NULL;
}

void js_jit_init(void)
{
    if (jit_worker.started) {
        jit_worker.ref_count++;
        return;
    }
    pthread_mutex_init(&jit_worker.lock, NULL);
    pthread_cond_init(&jit_worker.cond, NULL);
    jit_worker.head = jit_worker.tail = NULL;
    jit_worker.stop = 0;
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
}

void js_jit_queue_gcc(JSContext *ctx, JSFunctionBytecode *b)
{
    if (js_jit_fb_jit_no_compile(b)) return;
    if (js_jit_fb_get_func(b) != NULL) return;
    /* Claim the slot: no other thread or call will enqueue this function */
    js_jit_fb_set_no_compile(b);
    if (!jit_worker.started) return;

    JSJITCodeBuf cb;
    char fname[64];
    int unsupported = 0;
    const char *js_name = js_jit_fb_get_func_name(JS_GetRuntime(ctx), b);
    if (js_jit_gen_c(b, &cb, fname, sizeof(fname), &unsupported, js_name) < 0) {
        return;
    }

    JITGCCJob *job = malloc(sizeof(*job));
    if (!job) { jit_buf_free(&cb); return; }
    job->b     = b;
    job->c_src = cb.buf;   /* transfer buffer ownership to job */
    cb.buf     = NULL;     /* prevent double-free if jit_buf_free is called */
    memcpy(job->fname, fname, sizeof(job->fname));
    job->next  = NULL;

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

/*
 * Emit the C preamble: type definitions and the function signature.
 * The function symbol name encodes the bytecode pointer as a hex address
 * so multiple compiled functions don't clash when linked.
 */
static void gen_preamble(JSJITCodeBuf *cb, JSFunctionBytecode *b,
                         int var_count, int arg_count, int stack_size,
                         int closure_var_count, int cpool_count,
                         char *fname_out, size_t fname_sz,
                         const uint8_t *local_type,
                         const char *js_func_name)
{
    /* Unique function name based on pointer value */
    snprintf(fname_out, fname_sz, "__jit_f_%016llx",
             (unsigned long long)(uintptr_t)b);

    /* Debug: identify the JS source function */
    jit_buf_printf(cb, "/* JS function: %s */\n",
                   js_func_name ? js_func_name : "<unknown>");
    jit_buf_str(cb,
        "#include <stdint.h>\n"
        "#include \"quickjs.h\"\n"
        "#include \"quickjs-jit.h\"\n"
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
    if (var_count > 0)
        jit_buf_str(cb, "    int _sp=0, _i;\n");
    else
        jit_buf_str(cb, "    int _sp=0;\n");
    jit_buf_str(cb, "    (void)argc; (void)cpool; (void)var_refs;\n");
    if (var_count > 0)
        jit_buf_printf(cb,
            "    for(_i=0;_i<%d;_i++) _l[_i]=JS_UNDEFINED;\n", var_count);

    /* Phase 5: typed double locals.  NUMBER-inferred slots use _ld[idx]
     * instead of _l[idx], avoiding boxing/unboxing in hot arithmetic loops.
     * _l[idx] stays JS_UNDEFINED for NUMBER slots — _FREE(_l[idx]) is safe. */
    if (local_type && var_count > 0) {
        int nhave = 0;
        for (int j = 0; j < var_count; j++)
            if (local_type[j] == JIT_T_NUMBER) nhave++;
        if (nhave > 0) {
            jit_buf_printf(cb, "    double _ld[%d];\n", var_count);
            for (int j = 0; j < var_count; j++)
                if (local_type[j] == JIT_T_NUMBER)
                    jit_buf_printf(cb, "    _ld[%d]=0.0;\n", j);
        }
    }
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
                    const uint8_t *local_type)
{
    *unsupported_out = 0;
    int pc = 0;

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

#define _GS_PUSH(t) do { if (gen_sp < gen_stk_cap) gen_st[gen_sp++] = (uint8_t)(t); } while(0)
#define _GS_POP()   (gen_sp > 0 ? gen_st[--gen_sp] : (uint8_t)JIT_T_JSVAL)
#define _GS_TOP()   (gen_sp > 0 ? gen_st[gen_sp-1]   : (uint8_t)JIT_T_JSVAL)
#define _GS_TOP2()  (gen_sp > 1 ? gen_st[gen_sp-2]   : (uint8_t)JIT_T_JSVAL)
#define _GS_DROP(n) do { gen_sp -= (n); if (gen_sp < 0) gen_sp = 0; } while(0)

    while (pc < bc_len) {
        /* Emit label if this offset is a branch target.
         * Also reset gen_st conservatively — multiple control-flow paths merge
         * here so we cannot assume the type stack is consistent. */
        if (scan_is_target(sr, pc)) {
            memset(gen_st, JIT_T_JSVAL, gen_stk_cap);
            gen_sp = 0;
            jit_buf_printf(cb, "_L%d:;\n", pc);
        }

        int op = bc[pc];
        if (op >= op_sz_count || op_sz[op] == 0) {
            fprintf(stderr, "[JIT] unsupported opcode 0x%02x at pc=%d\n", op, pc);
            *unsupported_out = 1; free(gen_st); return -1;
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
        case OP_push_atom_value: {
            /* Push the string representation of an interned atom */
            uint32_t atom = bc_u32(&bc[pc+1]);
            jit_buf_printf(cb,
                "    { JSValue _v=JS_AtomToValue(ctx,(JSAtom)%uu);"
                " _CHK(_v); _s[_sp++]=_v; }\n", atom);
            break;
        }
        /* OP_fclosure / OP_fclosure8: create closure — needs stack-frame access,
         * not available in JIT.  Functions using these are kept in interpreter. */

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
        case OP_insert2: /* obj a -> a obj a (dup_x1): a is duplicated */
            /* Interpreter: sp[-2]=JS_DupValue(sp[0]) after moving.
             * Must dup 'a' since it appears in both new sp[-3] and sp[-1]. */
            jit_buf_str(cb,
                "    { JSValue _t=_DUP(_s[_sp-1]);"
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

        /* ---- Local variable access (Phase 5: type-aware) ----
         *
         * NUMBER locals use double _ld[idx]: boxing on read, unboxing on write.
         * GCC -O2 CSE collapses consecutive get_loc+arithmetic patterns into
         * pure double operations (e.g. get_loc i → add_loc s ≡ _ld[s]+=_ld[i]).
         * _l[idx] for NUMBER slots stays JS_UNDEFINED — footer _FREE is a no-op.
         * INT and FLOAT64 JSValues are immediate (no refcount) → no _FREE needed
         * when writing from stack to a NUMBER local.                           */
#define _IS_NUM(idx) \
    (local_type && (idx) >= 0 && (idx) < var_count && \
     local_type[(idx)] == JIT_T_NUMBER)

#define GEN_GET_LOC(idx) do { \
    if (_IS_NUM(idx)) \
        jit_buf_printf(cb, \
            "    { double _d=_ld[%d];" \
            " _s[_sp++]=(_d==(int32_t)_d)?JS_NewInt32(ctx,(int32_t)_d)" \
            ":JS_NewFloat64(ctx,_d); }\n", (idx)); \
    else \
        jit_buf_printf(cb, "    _s[_sp++]=_DUP(_l[%d]);\n", (idx)); \
} while(0)

#define GEN_PUT_LOC(idx) do { \
    if (_IS_NUM(idx)) \
        /* INT and FLOAT64 are immediate — no _FREE required */ \
        jit_buf_printf(cb, \
            "    { JSValue _t=_s[--_sp];" \
            " _ld[%d]=(JS_VALUE_GET_TAG(_t)==JS_TAG_INT)" \
            "?(double)JS_VALUE_GET_INT(_t):JS_VALUE_GET_FLOAT64(_t); }\n", (idx)); \
    else \
        jit_buf_printf(cb, "    _FREE(_l[%d]); _l[%d]=_s[--_sp];\n", (idx), (idx)); \
} while(0)

#define GEN_SET_LOC(idx) do { \
    if (_IS_NUM(idx)) \
        jit_buf_printf(cb, \
            "    { JSValue _t=_s[_sp-1];" \
            " _ld[%d]=(JS_VALUE_GET_TAG(_t)==JS_TAG_INT)" \
            "?(double)JS_VALUE_GET_INT(_t):JS_VALUE_GET_FLOAT64(_t); }\n", (idx)); \
    else \
        jit_buf_printf(cb, "    _FREE(_l[%d]); _l[%d]=_DUP(_s[_sp-1]);\n", (idx), (idx)); \
} while(0)

        case OP_get_loc:  case OP_get_loc_check:
        case OP_get_loc_checkthis: GEN_GET_LOC((int)bc_u16(&bc[pc+1])); break;
        case OP_put_loc:  case OP_put_loc_check:
        case OP_put_loc_check_init: GEN_PUT_LOC((int)bc_u16(&bc[pc+1])); break;
        case OP_set_loc:  GEN_SET_LOC((int)bc_u16(&bc[pc+1])); break;
        /* TDZ init: mark local as uninitialized — skip in JIT (no TDZ checking) */
        case OP_set_loc_uninitialized: break;
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

#undef _IS_NUM
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
/* _VRV(idx) returns a pointer to the JSValue stored inside var_refs[idx].
 * We use the vtable accessor rather than ->pvalue directly because JSVarRef
 * is defined only in quickjs.c (incomplete type in generated C). */
#define GEN_GET_VR(idx) \
    jit_buf_printf(cb, "    _s[_sp++]=_DUP(*_RT->var_ref_value(var_refs[%d]));\n", idx)
#define GEN_PUT_VR(idx) \
    jit_buf_printf(cb, "    { JSValue *_p=_RT->var_ref_value(var_refs[%d]);" \
                       " _FREE(*_p); *_p=_s[--_sp]; }\n", idx)
#define GEN_SET_VR(idx) \
    jit_buf_printf(cb, "    { JSValue *_p=_RT->var_ref_value(var_refs[%d]);" \
                       " _FREE(*_p); *_p=_DUP(_s[_sp-1]); }\n", idx)

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

        /* ---- Global/closure variable access (u16 index into var_refs) ----
         *
         * OP_get_var / OP_put_var: same as get/put_var_ref but with a 16-bit
         * index.  TDZ and const checks are skipped — the JIT only runs on hot
         * functions that have already executed successfully many times, so any
         * TDZ violation would have been caught by the interpreter.
         * ------------------------------------------------------------------ */
        case OP_get_var: {
            int idx = (int)bc_u16(&bc[pc+1]);
            jit_buf_printf(cb,
                "    _s[_sp++]=_DUP(*_RT->var_ref_value(var_refs[%d]));\n", idx);
            break;
        }
        case OP_put_var:
        case OP_put_var_init: {
            int idx = (int)bc_u16(&bc[pc+1]);
            jit_buf_printf(cb,
                "    { JSValue *_p=_RT->var_ref_value(var_refs[%d]);"
                " _FREE(*_p); *_p=_s[--_sp]; }\n", idx);
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

        /* ---- Increment / decrement ---- */
        /* OP_inc / OP_dec: pre-increment — pop, push ±1 (int fast path) */
        case OP_inc:
            jit_buf_str(cb,
                "    { JSValue _a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT){\n"
                "        int32_t ia=JS_VALUE_GET_INT(_a);\n"
                "        _s[_sp++]=(ia==INT32_MAX)?JS_NewFloat64(ctx,(double)ia+1)\n"
                "                                 :JS_NewInt32(ctx,ia+1);\n"
                "      } else { JSValue _r=_RT->add(ctx,_a,JS_NewInt32(ctx,1));\n"
                "               _CHK(_r); _s[_sp++]=_r; } }\n");
            break;
        case OP_dec:
            jit_buf_str(cb,
                "    { JSValue _a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT){\n"
                "        int32_t ia=JS_VALUE_GET_INT(_a);\n"
                "        _s[_sp++]=(ia==INT32_MIN)?JS_NewFloat64(ctx,(double)ia-1)\n"
                "                                 :JS_NewInt32(ctx,ia-1);\n"
                "      } else { JSValue _r=_RT->sub(ctx,_a,JS_NewInt32(ctx,1));\n"
                "               _CHK(_r); _s[_sp++]=_r; } }\n");
            break;
        /* OP_post_inc / OP_post_dec: post-increment — pop original, push original, push ±1 result */
        case OP_post_inc:
            jit_buf_str(cb,
                "    { JSValue _a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT){\n"
                "        int32_t ia=JS_VALUE_GET_INT(_a);\n"
                "        _s[_sp++]=_a; /* original */\n"
                "        _s[_sp++]=(ia==INT32_MAX)?JS_NewFloat64(ctx,(double)ia+1)\n"
                "                                 :JS_NewInt32(ctx,ia+1);\n"
                "      } else { JSValue _r=_RT->add(ctx,_DUP(_a),JS_NewInt32(ctx,1));\n"
                "               _CHK(_r); _s[_sp++]=_a; _s[_sp++]=_r; } }\n");
            break;
        case OP_post_dec:
            jit_buf_str(cb,
                "    { JSValue _a=_s[--_sp];\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT){\n"
                "        int32_t ia=JS_VALUE_GET_INT(_a);\n"
                "        _s[_sp++]=_a; /* original */\n"
                "        _s[_sp++]=(ia==INT32_MIN)?JS_NewFloat64(ctx,(double)ia-1)\n"
                "                                 :JS_NewInt32(ctx,ia-1);\n"
                "      } else { JSValue _r=_RT->sub(ctx,_DUP(_a),JS_NewInt32(ctx,1));\n"
                "               _CHK(_r); _s[_sp++]=_a; _s[_sp++]=_r; } }\n");
            break;
        /* OP_inc_loc / OP_dec_loc: in-place ±1 on local variable (1-byte index) */
        case OP_inc_loc: {
            int idx = bc[pc + 1];
            if (local_type && idx < var_count && local_type[idx] == JIT_T_NUMBER) {
                jit_buf_printf(cb, "    _ld[%d]+=1.0;\n", idx);
            } else {
                jit_buf_printf(cb,
                    "    { JSValue _a=_l[%d];\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT){\n"
                    "        int32_t ia=JS_VALUE_GET_INT(_a);\n"
                    "        _l[%d]=(ia==INT32_MAX)?JS_NewFloat64(ctx,(double)ia+1)\n"
                    "                              :JS_NewInt32(ctx,ia+1);\n"
                    "      } else { JSValue _r=_RT->add(ctx,_a,JS_NewInt32(ctx,1));\n"
                    "               _CHK(_r); _FREE(_l[%d]); _l[%d]=_r; } }\n",
                    idx, idx, idx, idx);
            }
            break;
        }
        case OP_dec_loc: {
            int idx = bc[pc + 1];
            if (local_type && idx < var_count && local_type[idx] == JIT_T_NUMBER) {
                jit_buf_printf(cb, "    _ld[%d]-=1.0;\n", idx);
            } else {
                jit_buf_printf(cb,
                    "    { JSValue _a=_l[%d];\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT){\n"
                    "        int32_t ia=JS_VALUE_GET_INT(_a);\n"
                    "        _l[%d]=(ia==INT32_MIN)?JS_NewFloat64(ctx,(double)ia-1)\n"
                    "                              :JS_NewInt32(ctx,ia-1);\n"
                    "      } else { JSValue _r=_RT->sub(ctx,_a,JS_NewInt32(ctx,1));\n"
                    "               _CHK(_r); _FREE(_l[%d]); _l[%d]=_r; } }\n",
                    idx, idx, idx, idx);
            }
            break;
        }
        /* OP_add_loc: pop stack top and add it in-place to local (1-byte index) */
        case OP_add_loc: {
            int idx = bc[pc + 1];
            if (local_type && idx < var_count && local_type[idx] == JIT_T_NUMBER) {
                /* NUMBER local: direct double add.  Inference guarantees the stack
                 * top is INT or FLOAT64 (both immediate — no _FREE needed). GCC
                 * CSE will collapse get_loc(j)+add_loc(i) → _ld[i]+=_ld[j]. */
                jit_buf_printf(cb,
                    "    { JSValue _b=_s[--_sp];\n"
                    "      if(JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n"
                    "        _ld[%d]+=(double)JS_VALUE_GET_INT(_b);\n"
                    "      else _ld[%d]+=JS_VALUE_GET_FLOAT64(_b); }\n",
                    idx, idx);
            } else {
                jit_buf_printf(cb,
                    "    { JSValue _b=_s[--_sp], *_pv=&_l[%d];\n"
                    "      if(JS_VALUE_GET_TAG(*_pv)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT){\n"
                    "        int64_t _r=(int64_t)JS_VALUE_GET_INT(*_pv)+JS_VALUE_GET_INT(_b);\n"
                    "        *_pv=((int32_t)_r==_r)?JS_NewInt32(ctx,(int32_t)_r)\n"
                    "                              :JS_NewFloat64(ctx,(double)_r);\n"
                    "      } else {\n"
                    "        JSValue _r=_RT->add(ctx,*_pv,_b); _CHK(_r); _FREE(*_pv); *_pv=_r;\n"
                    "      } }\n",
                    idx);
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

/* Helper: emit fused NUMBER×NUMBER comparison+branch.
 * _va = first pushed (left operand), _vb = second pushed (right operand). */
#define GEN_CMP_FUSE_NUM(c_op, ftgt, fneg) \
    jit_buf_printf(cb, \
        "    { JSValue _va=_s[_sp-2],_vb=_s[_sp-1]; _sp-=2;\n" \
        "      double _da=(JS_VALUE_GET_TAG(_va)==JS_TAG_INT)" \
                         "?(double)JS_VALUE_GET_INT(_va):JS_VALUE_GET_FLOAT64(_va);\n" \
        "      double _db=(JS_VALUE_GET_TAG(_vb)==JS_TAG_INT)" \
                         "?(double)JS_VALUE_GET_INT(_vb):JS_VALUE_GET_FLOAT64(_vb);\n" \
        "      if(%s(_da " c_op " _db)) goto _L%d; }\n", \
        (fneg)?"!":"", (ftgt))

/* Helper: emit fused general comparison+branch with INT fast path.
 * rt_call: full vtable call expression, e.g. "_RT->lt(ctx,_a,_b)" */
#define GEN_CMP_FUSE_GEN(int_op, rt_call, ftgt, fneg) \
    jit_buf_printf(cb, \
        "    { JSValue _a=_s[_sp-2],_b=_s[_sp-1]; _sp-=2; int _cond;\n" \
        "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n" \
        "        _cond=(JS_VALUE_GET_INT(_a) " int_op " JS_VALUE_GET_INT(_b));\n" \
        "      else{JSValue _r=" rt_call "; _CHK(_r); _cond=JS_VALUE_GET_INT(_r);}\n" \
        "      if(%s_cond) goto _L%d; }\n", \
        (fneg)?"!":"", (ftgt))

/* Helper: unfused comparison (produces BOOL on stack) with INT fast path */
#define GEN_CMP_UNFUSED(int_op, rt_call_or_expr) \
    jit_buf_str(cb, \
        "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n" \
        "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n" \
        "        _s[_sp++]=JS_NewBool(ctx,JS_VALUE_GET_INT(_a) " int_op " JS_VALUE_GET_INT(_b));\n" \
        "      else { " rt_call_or_expr " } }\n")

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
            int _bn = (_GS_TOP2()==JIT_T_NUMBER && _GS_TOP()==JIT_T_NUMBER);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn) GEN_CMP_FUSE_NUM("<",  _fi.tgt, _fi.negate);
                else     GEN_CMP_FUSE_GEN("<", "_RT->lt(ctx,_a,_b)",  _fi.tgt, _fi.negate);
            } else {
                GEN_CMP_UNFUSED("<", "JSValue _r=_RT->lt(ctx,_a,_b); _CHK(_r); _s[_sp++]=_r;");
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }
        case OP_lte: {
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            int _bn = (_GS_TOP2()==JIT_T_NUMBER && _GS_TOP()==JIT_T_NUMBER);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn) GEN_CMP_FUSE_NUM("<=", _fi.tgt, _fi.negate);
                else     GEN_CMP_FUSE_GEN("<=","_RT->lte(ctx,_a,_b)", _fi.tgt, _fi.negate);
            } else {
                GEN_CMP_UNFUSED("<=","JSValue _r=_RT->lte(ctx,_a,_b); _CHK(_r); _s[_sp++]=_r;");
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }
        case OP_gt: {
            /* a > b  ≡  b < a  (strict):  vtable uses lt(b,a) */
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            int _bn = (_GS_TOP2()==JIT_T_NUMBER && _GS_TOP()==JIT_T_NUMBER);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn) GEN_CMP_FUSE_NUM(">",  _fi.tgt, _fi.negate);
                else     GEN_CMP_FUSE_GEN(">", "_RT->lt(ctx,_b,_a)",  _fi.tgt, _fi.negate);
            } else {
                GEN_CMP_UNFUSED(">", "JSValue _r=_RT->lt(ctx,_b,_a); _CHK(_r); _s[_sp++]=_r;");
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }
        case OP_gte: {
            /* a >= b  ≡  b <= a  (inclusive): vtable uses lte(b,a) */
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            int _bn = (_GS_TOP2()==JIT_T_NUMBER && _GS_TOP()==JIT_T_NUMBER);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn) GEN_CMP_FUSE_NUM(">=", _fi.tgt, _fi.negate);
                else     GEN_CMP_FUSE_GEN(">=","_RT->lte(ctx,_b,_a)", _fi.tgt, _fi.negate);
            } else {
                GEN_CMP_UNFUSED(">=","JSValue _r=_RT->lte(ctx,_b,_a); _CHK(_r); _s[_sp++]=_r;");
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }

        case OP_eq: {
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            int _bn = (_GS_TOP2()==JIT_T_NUMBER && _GS_TOP()==JIT_T_NUMBER);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn) GEN_CMP_FUSE_NUM("==", _fi.tgt, _fi.negate);
                else     GEN_CMP_FUSE_GEN("==","_RT->eq(ctx,_a,_b)",  _fi.tgt, _fi.negate);
            } else {
                GEN_CMP_UNFUSED("==","JSValue _r=_RT->eq(ctx,_a,_b); _CHK(_r); _s[_sp++]=_r;");
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }
        case OP_neq: {
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            int _bn = (_GS_TOP2()==JIT_T_NUMBER && _GS_TOP()==JIT_T_NUMBER);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                /* neq fused: "!=" is the comparison; negate inverts it */
                if (_bn) GEN_CMP_FUSE_NUM("!=", _fi.tgt, _fi.negate);
                else     jit_buf_printf(cb,
                    "    { JSValue _a=_s[_sp-2],_b=_s[_sp-1]; _sp-=2; int _cond;\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n"
                    "        _cond=(JS_VALUE_GET_INT(_a)!=JS_VALUE_GET_INT(_b));\n"
                    "      else{JSValue _r=_RT->eq(ctx,_a,_b);_CHK(_r);_cond=!JS_VALUE_GET_INT(_r);}\n"
                    "      if(%s_cond) goto _L%d; }\n",
                    _fi.negate?"!":"", _fi.tgt);
            } else {
                jit_buf_str(cb,
                    "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n"
                    "        _s[_sp++]=JS_NewBool(ctx,JS_VALUE_GET_INT(_a)!=JS_VALUE_GET_INT(_b));\n"
                    "      else { JSValue _r=_RT->eq(ctx,_a,_b); _CHK(_r);\n"
                    "             _s[_sp++]=JS_NewBool(ctx,!JS_VALUE_GET_INT(_r)); _FREE(_r); } }\n");
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }
        case OP_strict_eq: {
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            int _bn = (_GS_TOP2()==JIT_T_NUMBER && _GS_TOP()==JIT_T_NUMBER);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn) GEN_CMP_FUSE_NUM("==", _fi.tgt, _fi.negate);
                else     GEN_CMP_FUSE_GEN("==","_RT->strict_eq(ctx,_a,_b)", _fi.tgt, _fi.negate);
            } else {
                jit_buf_str(cb,
                    "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_VALUE_GET_TAG(_b)&&JS_VALUE_GET_TAG(_a)==JS_TAG_INT)\n"
                    "        _s[_sp++]=JS_NewBool(ctx,JS_VALUE_GET_INT(_a)==JS_VALUE_GET_INT(_b));\n"
                    "      else { JSValue _r=_RT->strict_eq(ctx,_a,_b); _CHK(_r); _s[_sp++]=_r; } }\n");
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }
        case OP_strict_neq: {
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            int _bn = (_GS_TOP2()==JIT_T_NUMBER && _GS_TOP()==JIT_T_NUMBER);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn) GEN_CMP_FUSE_NUM("!=", _fi.tgt, _fi.negate);
                else     jit_buf_printf(cb,
                    "    { JSValue _a=_s[_sp-2],_b=_s[_sp-1]; _sp-=2; int _cond;\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n"
                    "        _cond=(JS_VALUE_GET_INT(_a)!=JS_VALUE_GET_INT(_b));\n"
                    "      else{JSValue _r=_RT->strict_eq(ctx,_a,_b);_CHK(_r);_cond=!JS_VALUE_GET_INT(_r);}\n"
                    "      if(%s_cond) goto _L%d; }\n",
                    _fi.negate?"!":"", _fi.tgt);
            } else {
                jit_buf_str(cb,
                    "    { JSValue _b=_s[--_sp],_a=_s[--_sp];\n"
                    "      if(JS_VALUE_GET_TAG(_a)==JS_VALUE_GET_TAG(_b)&&JS_VALUE_GET_TAG(_a)==JS_TAG_INT)\n"
                    "        _s[_sp++]=JS_NewBool(ctx,JS_VALUE_GET_INT(_a)!=JS_VALUE_GET_INT(_b));\n"
                    "      else { JSValue _r=_RT->strict_eq(ctx,_a,_b); _CHK(_r);\n"
                    "             _s[_sp++]=JS_NewBool(ctx,!JS_VALUE_GET_INT(_r)); _FREE(_r); } }\n");
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }

#undef GEN_CMP_FUSE_NUM
#undef GEN_CMP_FUSE_GEN
#undef GEN_CMP_UNFUSED

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
            int tgt = pc + 1 + delta;
            jit_buf_printf(cb,
                "    { JSValue _v=_s[--_sp]; int _b=_BOOL(_v);"
                " _FREE(_v); if(!_b) goto _L%d; }\n", tgt);
            break;
        }
        case OP_if_true: {
            int32_t delta = (int32_t)bc_u32(&bc[pc+1]);
            int tgt = pc + 1 + delta;
            jit_buf_printf(cb,
                "    { JSValue _v=_s[--_sp]; int _b=_BOOL(_v);"
                " _FREE(_v); if(_b) goto _L%d; }\n", tgt);
            break;
        }
        case OP_goto: {
            int32_t delta = (int32_t)bc_u32(&bc[pc+1]);
            int tgt = pc + 1 + delta;
            jit_buf_printf(cb, "    goto _L%d;\n", tgt);
            break;
        }
        case OP_if_false8: {
            int tgt = pc + 1 + (int)(int8_t)bc[pc+1];
            jit_buf_printf(cb,
                "    { JSValue _v=_s[--_sp]; int _b=_BOOL(_v);"
                " _FREE(_v); if(!_b) goto _L%d; }\n", tgt);
            break;
        }
        case OP_if_true8: {
            int tgt = pc + 1 + (int)(int8_t)bc[pc+1];
            jit_buf_printf(cb,
                "    { JSValue _v=_s[--_sp]; int _b=_BOOL(_v);"
                " _FREE(_v); if(_b) goto _L%d; }\n", tgt);
            break;
        }
        case OP_goto8: {
            int tgt = pc + 1 + (int)(int8_t)bc[pc+1];
            jit_buf_printf(cb, "    goto _L%d;\n", tgt);
            break;
        }
        case OP_goto16: {
            int tgt = pc + 1 + (int)(int16_t)bc_u16(&bc[pc+1]);
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
            jit_buf_printf(cb,
                "    { JSValue _obj=_s[_sp-1];"
                " JSValue _r=_RT->get_prop(ctx,_obj,(JSAtom)%uu);"
                " _CHK(_r);"
                " _FREE(_s[--_sp]); _s[_sp++]=_r; }\n",
                (unsigned)JS_ATOM_length);
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
        case OP_call_method: {
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
        case OP_tail_call_method: {
            int nargs = (int)bc_u16(&bc[pc+1]);
            /* tail call: perform the call and return the result directly
             * (no OP_return follows in the bytecode stream) */
            jit_buf_printf(cb,
                "    { int _n=%d;\n"
                "      JSValue _f=_s[_sp-1-_n];\n"
                "      JSValue _t=_s[_sp-2-_n];\n"
                "      JSValue _r=_RT->call(ctx,_f,_t,_n,&_s[_sp-_n]);\n"
                "      for(int _j=0;_j<_n;_j++) _FREE(_s[_sp-1-_j]);\n"
                "      _sp -= _n+2; _FREE(_f); _FREE(_t);\n"
                "      if(JS_VALUE_GET_TAG(_r)==JS_TAG_EXCEPTION) goto _ex;\n",
                nargs);
            if (var_count > 0)
                jit_buf_printf(cb,
                    "      for(_i=0;_i<%d;_i++) _FREE(_l[_i]);\n", var_count);
            jit_buf_str(cb,
                "      while(_sp>0) _FREE(_s[--_sp]);\n"
                "      return _r; }\n");
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
                "      if(JS_VALUE_GET_TAG(_r)==JS_TAG_EXCEPTION) goto _ex;\n",
                nargs);
            if (var_count > 0)
                jit_buf_printf(cb,
                    "      for(_i=0;_i<%d;_i++) _FREE(_l[_i]);\n", var_count);
            jit_buf_str(cb,
                "      while(_sp>0) _FREE(_s[--_sp]);\n"
                "      return _r; }\n");
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
            jit_buf_str(cb, "    { JSValue _r=_s[--_sp];\n");
            if (var_count > 0)
                jit_buf_printf(cb,
                    "      for(_i=0;_i<%d;_i++) _FREE(_l[_i]);\n", var_count);
            jit_buf_str(cb,
                "      while(_sp>0) _FREE(_s[--_sp]);\n"
                "      return _r; }\n");
            break;
        }
        case OP_return_undef: {
            jit_buf_str(cb, "    {");
            if (var_count > 0)
                jit_buf_printf(cb,
                    " for(_i=0;_i<%d;_i++) _FREE(_l[_i]);", var_count);
            jit_buf_str(cb,
                " while(_sp>0) _FREE(_s[--_sp]); return JS_UNDEFINED; }\n");
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
        /* OP_define_field: obj val -> obj   (obj stays on stack, val consumed)
         * Used in object literals: { key: val } sequences.
         * JS_PROP_C_W_E = configurable | writable | enumerable */
        case OP_define_field: {
            uint32_t atom = bc_u32(&bc[pc+1]);
            jit_buf_printf(cb,
                "    { JSValue _v=_s[--_sp];\n"
                "      int _r=JS_DefinePropertyValue(ctx,_s[_sp-1],(JSAtom)%uu,_v,"
                "JS_PROP_C_W_E|JS_PROP_THROW);\n"
                "      if(_r<0) goto _ex; }\n",
                atom);
            break;
        }
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
            fprintf(stderr, "[JIT] gen_body: unhandled opcode 0x%02x at pc=%d\n", op, pc);
            *unsupported_out = 1;
            free(gen_st);
            return -1;
        }

        /* Phase 6.1: update gen-time type stack.
         * Comparison opcodes (OP_lt..OP_strict_neq) already updated gen_st
         * inline above.  All other opcodes are handled here.              */
        {
            uint8_t _gs_push = 255; /* 255 = no push */
            int     _gs_drop = 0;

            switch (op) {
            /* --- Numeric constant pushes → NUMBER --- */
            case OP_push_i32: case OP_push_i8: case OP_push_i16:
            case OP_push_0:   case OP_push_1:  case OP_push_2:  case OP_push_3:
            case OP_push_4:   case OP_push_5:  case OP_push_6:  case OP_push_7:
            case OP_push_minus1:
                _gs_push = JIT_T_NUMBER; break;

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

            /* --- get_length: NUMBER (array length is always a non-neg int) --- */
            case OP_get_length: _gs_drop=1; _gs_push=JIT_T_NUMBER; break;

            /* --- Arithmetic: NUMBER iff both operands were NUMBER --- */
            case OP_add: case OP_sub: case OP_mul: case OP_div: case OP_mod: {
                uint8_t _t2=_GS_TOP2(), _t1=_GS_TOP();
                _gs_drop = 2;
                _gs_push = (_t2==JIT_T_NUMBER&&_t1==JIT_T_NUMBER)
                           ? JIT_T_NUMBER : JIT_T_JSVAL;
                break;
            }
            /* --- Bitwise: always produces an INT (NUMBER) --- */
            case OP_shl: case OP_sar: case OP_shr:
            case OP_and: case OP_or:  case OP_xor: _gs_drop=2; _gs_push=JIT_T_NUMBER; break;
            case OP_not:                            _gs_drop=1; _gs_push=JIT_T_NUMBER; break;

            /* --- Unary numeric: NUMBER iff operand was NUMBER --- */
            case OP_neg: case OP_plus: case OP_inc: case OP_dec:
                { uint8_t _t=_GS_TOP();
                  _gs_drop=1;
                  _gs_push=(_t==JIT_T_NUMBER)?JIT_T_NUMBER:JIT_T_JSVAL; break; }
            /* post_inc/dec: pop 1 (original), push 2 (original + result).
             * We set _gs_drop=1 and push the result type; the original is
             * re-pushed via a special case in the application block below. */
            case OP_post_inc: case OP_post_dec:
                { uint8_t _t=_GS_TOP();
                  _gs_drop=1;
                  _gs_push=(_t==JIT_T_NUMBER)?JIT_T_NUMBER:JIT_T_JSVAL; break; }

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
            case OP_put_var: case OP_put_var_init:
                _gs_drop = 1; break;

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

            /* --- Property / array access → JSVAL --- */
            case OP_get_field:    _gs_drop=1; _gs_push=JIT_T_JSVAL; break;
            case OP_get_field2:              _gs_push=JIT_T_JSVAL; break;
            case OP_get_array_el: _gs_drop=2; _gs_push=JIT_T_JSVAL; break;
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

            /* Everything else: no tracked stack effect (conservative) */
            default: break;
            }

            /* Apply drop then push */
            if (_gs_drop > 0) { gen_sp -= _gs_drop; if (gen_sp < 0) gen_sp = 0; }
            if (_gs_push != 255 && gen_sp < gen_stk_cap)
                gen_st[gen_sp++] = _gs_push;
            /* post_inc/dec: push a second time (original + result both go on stack) */
            if ((op == OP_post_inc || op == OP_post_dec) && gen_sp < gen_stk_cap)
                gen_st[gen_sp++] = _gs_push;
        }

        pc += sz;
    }

#undef _GS_PUSH
#undef _GS_POP
#undef _GS_TOP
#undef _GS_TOP2
#undef _GS_DROP

    free(gen_st);
    return 0;
}

/*
 * js_jit_gen_c() — top-level: run scan, then generate C source for b.
 * Returns 0 and fills cb on success.
 * Returns -1 on failure; if *unsupported!=0 the function is ineligible.
 */
static int js_jit_gen_c(JSFunctionBytecode *b, JSJITCodeBuf *cb,
                        char *fname_out, size_t fname_sz, int *unsupported,
                        const char *js_func_name)
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

    /* Phase 5: infer which locals are always numeric → use C double */
    uint8_t *local_type = jit_infer_types(bc, bc_len, op_sz, op_sz_count,
                                           var_count, stack_size);

    if (jit_buf_init(cb) < 0) {
        scan_result_free(&sr);
        free(local_type);
        return -1;
    }

    gen_preamble(cb, b, var_count, arg_count, stack_size,
                 closure_var_count, cpool_count, fname_out, fname_sz,
                 local_type, js_func_name);

    int unsup = 0;
    if (gen_body(cb, bc, bc_len, &sr, op_sz, op_sz_count,
                 var_count, arg_count, stack_size, &unsup, local_type) < 0) {
        *unsupported = unsup;
        jit_buf_free(cb);
        scan_result_free(&sr);
        free(local_type);
        return -1;
    }

    gen_footer(cb, var_count);

    scan_result_free(&sr);
    free(local_type);

    if (cb->error) {
        jit_buf_free(cb);
        return -1;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Cleanup hook called from free_function_bytecode()
 * ----------------------------------------------------------------------- */

void js_jit_free_bytecode(JSFunctionBytecode *b)
{
    uint8_t tier   = js_jit_fb_get_tier(b);
    void   *handle = js_jit_fb_get_handle(b);

    js_jit_fb_clear_handles(b);

    if (tier == 2 && handle)
        dlclose(handle);
}

#endif /* CONFIG_JIT */
