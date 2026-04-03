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
    /* calls — P8.3: js_jit_call checks jit_func before falling to JS_Call */
    .call             = js_jit_call,
    .call_constructor = jit_rt_call_constructor,
    /* exceptions */
    .throw_type_error = jit_rt_throw_type_error,
    .throw_val        = jit_rt_throw_val,
    /* P8.2: interrupt poll for direct self-recursive calls */
    .poll_interrupts  = js_jit_poll_interrupts,
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

#define JIT_T_JSVAL    0  /* unknown — always use JSValue */
#define JIT_T_NUMBER   1  /* provably always numeric — use C double */
#define JIT_T_INT      2  /* provably always integral — use int64_t _li[] */
/* P8.2 gen_st marker: the value in this stack slot came from a self-recursive
 * OP_get_var.  Used to detect self-recursive call patterns at codegen time so
 * the call can be emitted as a direct C function call instead of _RT->call. */
#define JIT_T_SELF_FUNC 3

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
            case OP_get_length:   _TI_DROPN(1); _TI_PUSH(JIT_T_NUMBER); break;
            case OP_object:                     _TI_PUSH(JIT_T_JSVAL); break;
            case OP_array_from: {
                int n = (int)bc_u16(&bc[pc+1]); _TI_DROPN(n); _TI_PUSH(JIT_T_JSVAL); break;
            }

            /* ---- Binary arithmetic: propagate most specific numeric type ---- */
            case OP_add: case OP_sub: case OP_mul: case OP_div: case OP_mod: {
                uint8_t b = _TI_POP(), a = _TI_POP();
                uint8_t r = (a >= JIT_T_NUMBER && b >= JIT_T_NUMBER)
                            ? (a == JIT_T_INT && b == JIT_T_INT ? JIT_T_INT : JIT_T_NUMBER)
                            : JIT_T_JSVAL;
                _TI_PUSH(r);
                break;
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
                         const char *js_func_name, uint64_t bc_hash,
                         JSRuntime *rt);

typedef struct JITGCCJob {
    JSFunctionBytecode *b;
    char               *c_src;     /* malloc'd C source; freed after gcc    */
    char                fname[64]; /* symbol name to look up via dlsym      */
    uint64_t            bc_hash;   /* FNV-1a hash of bytecode + build stamp */
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

static char jit_cache_dir[512];
static int  jit_cache_enabled;

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
    if (!getenv("QJS_JIT_KEEP_C")) unlink(c_path);
    free(c_path);
    if (!gcc_ok) { unlink(so_path); goto fail; }

    /* Cache the compiled .so before unlinking (Phase 7.3) */
    jit_cache_put(so_path, job->bc_hash);

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

void js_jit_queue_gcc(JSContext *ctx, JSFunctionBytecode *b)
{
    if (js_jit_fb_jit_no_compile(b)) return;
    if (js_jit_fb_get_func(b) != NULL) return;
    /* Claim the slot: no other thread or call will enqueue this function */
    js_jit_fb_set_no_compile(b);
    if (!jit_worker.started) return;

    /* Compute stable bytecode hash for cache lookup and symbol naming */
    int bc_len;
    const uint8_t *bc = js_jit_fb_get_bytecode(b, &bc_len);
    uint64_t bc_hash = jit_hash_bytecode(bc, bc_len);

    /* Phase 7.4: cache hit — load pre-compiled .so without running GCC */
    char *cache_path = jit_cache_get(bc_hash);
    if (cache_path) {
        char fname[64];
        snprintf(fname, sizeof(fname), "__jit_f_%016llx",
                 (unsigned long long)bc_hash);
        void *handle = dlopen(cache_path, RTLD_NOW | RTLD_LOCAL);
        free(cache_path);
        if (handle) {
            JSJITFunc f = (JSJITFunc)(uintptr_t)dlsym(handle, fname);
            if (f) {
                js_jit_fb_set_func(b, f, handle, 2);
                return;
            }
            dlclose(handle);
            /* Corrupted cache entry — fall through to recompile */
        }
    }

    JSJITCodeBuf cb;
    char fname[64];
    int unsupported = 0;
    const char *js_name = js_jit_fb_get_func_name(JS_GetRuntime(ctx), b);
    if (js_jit_gen_c(b, &cb, fname, sizeof(fname), &unsupported,
                     js_name, bc_hash, JS_GetRuntime(ctx)) < 0) {
        return;
    }

    JITGCCJob *job = malloc(sizeof(*job));
    if (!job) { jit_buf_free(&cb); return; }
    job->b       = b;
    job->c_src   = cb.buf;   /* transfer buffer ownership to job */
    cb.buf       = NULL;     /* prevent double-free if jit_buf_free is called */
    job->bc_hash = bc_hash;
    memcpy(job->fname, fname, sizeof(job->fname));
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
                         char **varnames)
{
    /* Stable symbol name derived from bytecode hash */
    snprintf(fname_out, fname_sz, "__jit_f_%016llx",
             (unsigned long long)bc_hash);

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

    /* P9.2: named temp stack slots — declare _tsv0.._tsv{stack_size-1}.
     * Each initialized to JS_UNDEFINED so the _ex cleanup path can safely
     * _FREE them even if they were never written. */
    for (int j = 0; j < stack_size; j++)
        jit_buf_printf(cb, "    JSValue _tsv%d=JS_UNDEFINED;\n", j);
    /* P9.4: raw double temporaries for typed stack slots.
     * When gen_st[slot] >= JIT_T_NUMBER the value lives here, not in _tsv{}. */
    for (int j = 0; j < stack_size; j++)
        jit_buf_printf(cb, "    double _tsd%d=0.0;\n", j);
    /* _sp is still needed: updated at throw/exception sites so the _ex
     * cleanup knows which _tsv{} slots are live. */
    jit_buf_str(cb, "    int _sp=0;\n");
    jit_buf_str(cb, "    (void)argc; (void)cpool; (void)var_refs;\n");

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
}

/*
 * Emit the exception-cleanup footer and closing brace.
 * P9.2: stack cleanup is unrolled using _tsv{} names, guarded by _sp.
 * _sp is updated at every throw/exception site so this correctly frees
 * only live slots.
 */
static void gen_footer(JSJITCodeBuf *cb, int var_count,
                       int arg_count, char **varnames, int stack_size)
{
    jit_buf_str(cb, "_ex:\n");
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
                    char **varnames)
{
    *unsupported_out = 0;
    int pc = 0;

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
 * object), not a typed double.  Only JIT_T_NUMBER (1) and JIT_T_INT (2) hold
 * their value in _tsd and need to be boxed into _tsv. */
#define _P94_ENSURE(slot) do { \
    if ((slot) < d && gen_sp > (slot) && \
        gen_st[(slot)] >= JIT_T_NUMBER && gen_st[(slot)] <= JIT_T_INT) \
        jit_buf_printf(cb, \
            "    { double _dv=_tsd%d; " \
            "_tsv%d=((double)(int32_t)_dv==_dv)?JS_NewInt32(ctx,(int32_t)_dv)" \
            ":JS_NewFloat64(ctx,_dv); }\n", (slot), (slot)); \
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

    while (pc < bc_len) {
        /* P9.2: stack depth BEFORE this opcode.  sdt[pc]==0xffff means unreachable.
         * Computed early so _P94_ENSURE (which uses d) works in the label block. */
        int d = (sdt && sdt[pc] != 0xffff) ? (int)sdt[pc] : 0;

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
            gen_sp = (sdt != NULL) ? (int)sdt[pc] : 0;
            if (gen_sp < 0 || gen_sp > gen_stk_cap) gen_sp = 0;
            jit_buf_printf(cb, "_L%d:;\n", pc);
            /* P9.3: if this PC is a while/do-while loop header, open while(1){ */
            if (n_cf > 0 && p93_depth < 16) {
                for (int _ci = 0; _ci < n_cf; _ci++) {
                    if ((cf_annots[_ci].kind == JIT_CF_WHILE_LOOP ||
                         cf_annots[_ci].kind == JIT_CF_DOWHILE_LOOP) &&
                        (uint32_t)pc == cf_annots[_ci].header_pc) {
                        jit_buf_str(cb, "while(1) {\n");
                        p93_active[p93_depth].header_pc = cf_annots[_ci].header_pc;
                        p93_active[p93_depth].exit_pc   = cf_annots[_ci].exit_pc;
                        p93_depth++;
                        break;
                    }
                }
            }
        }

        /* P9.4: if gen_sp has drifted above d (due to pop-ops missing from gen_st),
         * reset conservatively so stale INT entries can't cause false _bn=true or
         * spurious boxing in _P94_ENSURE. */
        if (gen_sp > d) {
            memset(gen_st, JIT_T_JSVAL, gen_stk_cap);
            gen_sp = d;
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
        /* P9.4: push into _tsd{d} (raw double), skip boxing; _sp = d+1.
         * d = sdt[pc] = depth before push. */
        case OP_push_i32:
            jit_buf_printf(cb,
                "    _tsd%d=(double)(int32_t)%uu; _sp=%d;\n",
                d, bc_u32(&bc[pc+1]), d+1);
            break;
        case OP_push_i8:
            jit_buf_printf(cb,
                "    _tsd%d=(double)%d; _sp=%d;\n",
                d, (int)(int8_t)bc[pc+1], d+1);
            break;
        case OP_push_i16:
            jit_buf_printf(cb,
                "    _tsd%d=(double)%d; _sp=%d;\n",
                d, (int)(int16_t)bc_u16(&bc[pc+1]), d+1);
            break;
        case OP_push_minus1:
            jit_buf_printf(cb, "    _tsd%d=-1.0; _sp=%d;\n", d, d+1); break;
        case OP_push_0:
            jit_buf_printf(cb, "    _tsd%d=0.0; _sp=%d;\n", d, d+1); break;
        case OP_push_1:
            jit_buf_printf(cb, "    _tsd%d=1.0; _sp=%d;\n", d, d+1); break;
        case OP_push_2:
            jit_buf_printf(cb, "    _tsd%d=2.0; _sp=%d;\n", d, d+1); break;
        case OP_push_3:
            jit_buf_printf(cb, "    _tsd%d=3.0; _sp=%d;\n", d, d+1); break;
        case OP_push_4:
            jit_buf_printf(cb, "    _tsd%d=4.0; _sp=%d;\n", d, d+1); break;
        case OP_push_5:
            jit_buf_printf(cb, "    _tsd%d=5.0; _sp=%d;\n", d, d+1); break;
        case OP_push_6:
            jit_buf_printf(cb, "    _tsd%d=6.0; _sp=%d;\n", d, d+1); break;
        case OP_push_7:
            jit_buf_printf(cb, "    _tsd%d=7.0; _sp=%d;\n", d, d+1); break;
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
        /* OP_fclosure / OP_fclosure8: create closure — needs stack-frame access,
         * not available in JIT.  Functions using these are kept in interpreter. */

        /* ---- Stack manipulation ---- */
        /* P9.2: use named slots _tsv{d-1}, _tsv{d}, etc. */
        case OP_drop: /* pop top: depth d -> d-1 */
            /* P9.4: typed slot lives in _tsd — no refcount, no _FREE needed */
            if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER)
                jit_buf_printf(cb, "    _sp=%d;\n", d-1);
            else
                jit_buf_printf(cb, "    _FREE(_tsv%d); _sp=%d;\n", d-1, d-1);
            break;
        case OP_dup: /* peek top, push copy: depth d -> d+1 */
            /* P9.4: typed slot — copy _tsd directly, no _DUP */
            if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER)
                jit_buf_printf(cb, "    _tsd%d=_tsd%d; _sp=%d;\n", d, d-1, d+1);
            else
                jit_buf_printf(cb, "    _tsv%d=_DUP(_tsv%d); _sp=%d;\n", d, d-1, d+1);
            break;
        case OP_dup1: /* a b -> a a b (insert dup of a below b): depth d -> d+1
                       * _tsv{d-2}=a, _tsv{d-1}=b
                       * result: _tsv{d-2}=dup(a), _tsv{d-1}=b -> shift b to d, put dup at d-1 */
            jit_buf_printf(cb,
                "    { JSValue _t=_DUP(_tsv%d);"
                " _tsv%d=_tsv%d; _tsv%d=_t; _sp=%d; }\n",
                d-2, d, d-1, d-1, d+1);
            break;
        case OP_dup2: /* a b -> a b a b: depth d -> d+2 */
            jit_buf_printf(cb,
                "    _tsv%d=_DUP(_tsv%d); _tsv%d=_DUP(_tsv%d); _sp=%d;\n",
                d, d-2, d+1, d-1, d+2);
            break;
        case OP_insert2: /* obj a -> a obj a (dup_x1): depth d -> d+1
                          * _tsv{d-2}=obj, _tsv{d-1}=a
                          * result: _tsv{d-2}=dup(a), _tsv{d-1}=obj, _tsv{d}=a */
            jit_buf_printf(cb,
                "    { JSValue _t=_DUP(_tsv%d);"
                " _tsv%d=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_t; _sp=%d; }\n",
                d-1, d, d-1, d-1, d-2, d-2, d+1);
            break;
        /* OP_pop does not exist; OP_drop handles the pop case */
        case OP_nip: /* a b -> b: depth d -> d-1 */
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
            jit_buf_printf(cb,
                "    { JSValue _t=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_t; }\n",
                d-3, d-3, d-2, d-2, d-1, d-1);
            break;
        case OP_rot3r: /* a b c -> c a b: depth unchanged */
            jit_buf_printf(cb,
                "    { JSValue _t=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_tsv%d; _tsv%d=_t; }\n",
                d-1, d-1, d-2, d-2, d-3, d-3);
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

/* P9.4: use _tsd{d} for typed locals, _tsv{d} for JSVAL locals. d is in scope. */
#define GEN_GET_LOC(idx) do { \
    if (_IS_INT(idx)) \
        jit_buf_printf(cb, \
            "    _tsd%d=(double)_jsi_%s; _sp=%d;\n", d, LNAME(idx), d+1); \
    else if (_IS_NUM(idx)) \
        jit_buf_printf(cb, \
            "    _tsd%d=_jsd_%s; _sp=%d;\n", d, LNAME(idx), d+1); \
    else \
        jit_buf_printf(cb, "    _tsv%d=_DUP(_jsv_%s); _sp=%d;\n", d, LNAME(idx), d+1); \
} while(0)

#define GEN_PUT_LOC(idx) do { \
    if (_IS_INT(idx) && gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) { \
        /* P9.4: typed source → read _tsd directly, no unboxing */ \
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
    } else { \
        _P94_ENSURE(d-1); /* P9.4: box typed slot before storing as JSValue */ \
        jit_buf_printf(cb, "    _FREE(_jsv_%s); _jsv_%s=_tsv%d; _sp=%d;\n", \
                       LNAME(idx), LNAME(idx), d-1, d-1); \
    } \
} while(0)

#define GEN_SET_LOC(idx) do { \
    if (_IS_INT(idx) && gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) { \
        /* P9.4: typed source → read _tsd directly, non-destructive peek */ \
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
    } else { \
        _P94_ENSURE(d-1); /* P9.4: box typed slot before storing as JSValue */ \
        jit_buf_printf(cb, "    _FREE(_jsv_%s); _jsv_%s=_DUP(_tsv%d);\n", \
                       LNAME(idx), LNAME(idx), d-1); \
    } \
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

#undef _IS_INT
#undef _IS_NUM
#undef GEN_GET_LOC
#undef GEN_PUT_LOC
#undef GEN_SET_LOC

        /* ---- Argument access (P8.4: int-arg fast path) ----
         * _aim bit i: argv[i] is a JS_TAG_INT and its value is live in _ai[i].
         * GEN_GET_ARG: prefer _ai[i] (register-friendly int32) over argv[i] load.
         * GEN_PUT/SET_ARG: keep _ai[i] and _aim consistent on arg writes. */
/* P9.2: arg access macros use _tsv{d} for push, _tsv{d-1} for pop/peek. */
#define _AI_VALID(idx) ((idx) < 32)
#define GEN_GET_ARG(idx) do { \
    if (_AI_VALID(idx)) \
        jit_buf_printf(cb, \
            "    _tsv%d=((%d)<argc&&(_aim>>%du&1u))" \
            "?JS_MKVAL(JS_TAG_INT,_jai_%s)" \
            ":((%d)<argc?_DUP(argv[%d]):JS_UNDEFINED); _sp=%d;\n", \
            d, idx, (unsigned)(idx), ANAME(idx), idx, idx, d+1); \
    else \
        jit_buf_printf(cb, \
            "    _tsv%d=((%d)<argc?_DUP(argv[%d]):JS_UNDEFINED); _sp=%d;\n", \
            d, idx, idx, d+1); \
} while(0)
#define GEN_PUT_ARG(idx) do { \
    _P94_ENSURE(d-1); \
    if (_AI_VALID(idx)) \
        jit_buf_printf(cb, \
            "    if((%d)<argc){ JSValue _t=_tsv%d; _sp=%d;\n" \
            "      if(JS_VALUE_GET_TAG(_t)==JS_TAG_INT){_jai_%s=JS_VALUE_GET_INT(_t);_aim|=%uu;}else{_aim&=~%uu;}\n" \
            "      _FREE(argv[%d]);argv[%d]=_t;}else{ _FREE(_tsv%d); _sp=%d; }\n", \
            idx, d-1, d-1, ANAME(idx), 1u<<(unsigned)(idx), 1u<<(unsigned)(idx), idx, idx, d-1, d-1); \
    else \
        jit_buf_printf(cb, \
            "    if((%d)<argc){_FREE(argv[%d]); argv[%d]=_tsv%d; _sp=%d;}else{ _FREE(_tsv%d); _sp=%d; }\n", \
            idx, idx, idx, d-1, d-1, d-1, d-1); \
} while(0)
#define GEN_SET_ARG(idx) do { \
    _P94_ENSURE(d-1); \
    if (_AI_VALID(idx)) \
        jit_buf_printf(cb, \
            "    if((%d)<argc){ JSValue _t=_tsv%d;\n" \
            "      if(JS_VALUE_GET_TAG(_t)==JS_TAG_INT){_jai_%s=JS_VALUE_GET_INT(_t);_aim|=%uu;}else{_aim&=~%uu;}\n" \
            "      _FREE(argv[%d]);argv[%d]=_DUP(_t);};\n", \
            idx, d-1, ANAME(idx), 1u<<(unsigned)(idx), 1u<<(unsigned)(idx), idx, idx); \
    else \
        jit_buf_printf(cb, \
            "    if((%d)<argc){_FREE(argv[%d]); argv[%d]=_DUP(_tsv%d);};\n", \
            idx, idx, idx, d-1); \
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
 * We use the vtable accessor rather than ->pvalue directly because JSVarRef
 * is defined only in quickjs.c (incomplete type in generated C). */
#define GEN_GET_VR(idx) \
    jit_buf_printf(cb, "    _tsv%d=_DUP(*_RT->var_ref_value(var_refs[%d])); _sp=%d;\n", \
                   d, idx, d+1)
#define GEN_PUT_VR(idx) do { \
    _P94_ENSURE(d-1); /* P9.4: box typed slot before storing as JSValue */ \
    jit_buf_printf(cb, "    { JSValue *_p=_RT->var_ref_value(var_refs[%d]);" \
                       " _FREE(*_p); *_p=_tsv%d; _sp=%d; }\n", idx, d-1, d-1); \
} while(0)
#define GEN_SET_VR(idx) do { \
    _P94_ENSURE(d-1); /* P9.4: box typed slot before storing as JSValue */ \
    jit_buf_printf(cb, "    { JSValue *_p=_RT->var_ref_value(var_refs[%d]);" \
                       " _FREE(*_p); *_p=_DUP(_tsv%d); }\n", idx, d-1); \
} while(0)

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
            /* Must check for JS_UNINITIALIZED: a non-lexical global var may
             * have been deleted at runtime (delete gvar).  In that case fall
             * back to JS_GetPropertyInternal which throws ReferenceError.
             * Bake the atom number and is_lexical flag into the generated C
             * so the slow path needs no closure_var access at runtime. */
            JSAtom cv_atom     = js_jit_fb_get_closure_var_atom(b, idx);
            int    cv_is_lex   = js_jit_fb_get_closure_var_is_lexical(b, idx);
            /* P9.2: push into _tsv{d}; set _sp before _CHK for exception safety */
            jit_buf_printf(cb,
                "    { JSValue *_pv=_RT->var_ref_value(var_refs[%d]);\n"
                "      if(JS_VALUE_GET_TAG(*_pv)==JS_TAG_UNINITIALIZED){\n"
                "        JSValue _r=_RT->get_var_slow(ctx,%uu,%d);\n"
                "        _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d;\n"
                "      } else { _tsv%d=_DUP(*_pv); _sp=%d; } }\n",
                idx, (unsigned)cv_atom, cv_is_lex, d, d, d+1, d, d+1);
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
             * (implicit global → JS_SetPropertyInternal; lexical TDZ → throw). */
            if (is_init && cv_is_lex) {
                /* lexical init: always write directly, never needs slow path */
                jit_buf_printf(cb,
                    "    { JSValue *_p=_RT->var_ref_value(var_refs[%d]);"
                    " _FREE(*_p); *_p=_tsv%d; _sp=%d; }\n", idx, d-1, d-1);
            } else {
                jit_buf_printf(cb,
                    "    { JSValue _v=_tsv%d; _sp=%d;\n"
                    "      JSValue *_p=_RT->var_ref_value(var_refs[%d]);\n"
                    "      if(js_unlikely(JS_VALUE_GET_TAG(*_p)==JS_TAG_UNINITIALIZED)){\n"
                    "        if(_RT->put_var_slow(ctx,%uu,%d,%d,_v)<0) goto _ex;\n"
                    "      } else { _FREE(*_p); *_p=_v; } }\n",
                    d-1, d-1, idx, (unsigned)cv_atom, cv_is_lex, is_init);
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
            if (_bn) {
                /* P9.4: both typed — pure double add on _tsd */
                jit_buf_printf(cb,
                    "    _tsd%d+=_tsd%d; _sp=%d;\n",
                    d-2, d-1, d-1);
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                jit_buf_printf(cb,
                    "    { JSValue _b=_tsv%d,_a=_tsv%d;\n"
                    "      int _ta=JS_VALUE_GET_TAG(_a),_tb=JS_VALUE_GET_TAG(_b);\n"
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
                    d-1, d-2, d-2, d-1, d-2, d-1, d-2, d-2, d-1);
            }
            break;
        }

        /* sub: int fast path + float64 fast path */
        case OP_sub: {
            int _bn = (gen_sp <= d && _GS_TOP2()>=JIT_T_NUMBER && _GS_TOP2()<=JIT_T_INT && _GS_TOP()>=JIT_T_NUMBER && _GS_TOP()<=JIT_T_INT);
            if (_bn) {
                /* P9.4: both typed — pure double sub on _tsd */
                jit_buf_printf(cb,
                    "    _tsd%d-=_tsd%d; _sp=%d;\n",
                    d-2, d-1, d-1);
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
            if (_bn) {
                /* P9.4: both typed — pure double mul on _tsd */
                jit_buf_printf(cb,
                    "    _tsd%d*=_tsd%d; _sp=%d;\n",
                    d-2, d-1, d-1);
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
            if (_bn) {
                /* P9.4: both typed — pure double div on _tsd */
                jit_buf_printf(cb,
                    "    _tsd%d/=_tsd%d; _sp=%d;\n",
                    d-2, d-1, d-1);
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
            if (_bn) {
                /* P9.4: both typed — fmod on raw _tsd */
                jit_buf_printf(cb,
                    "    _tsd%d=fmod(_tsd%d,_tsd%d); _sp=%d;\n",
                    d-2, d-2, d-1, d-1);
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
        case OP_shl:
            _P94_ENSURE(d-2); _P94_ENSURE(d-1); /* P9.4: box typed slots */
            jit_buf_printf(cb,
                "    { JSValue _b=_tsv%d,_a=_tsv%d;\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n"
                "        _tsv%d=JS_NewInt32(ctx,(int32_t)((uint32_t)JS_VALUE_GET_INT(_a)<<(JS_VALUE_GET_INT(_b)&31)));\n"
                "      else { _sp=%d; JSValue _r=_RT->shl(ctx,_a,_b); _CHK(_r); _tsv%d=_r; }\n"
                "      _sp=%d; }\n",
                d-1, d-2, d-2, d-2, d-2, d-1);
            break;
        case OP_sar:
            _P94_ENSURE(d-2); _P94_ENSURE(d-1); /* P9.4: box typed slots */
            jit_buf_printf(cb,
                "    { JSValue _b=_tsv%d,_a=_tsv%d;\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT&&JS_VALUE_GET_TAG(_b)==JS_TAG_INT)\n"
                "        _tsv%d=JS_NewInt32(ctx,JS_VALUE_GET_INT(_a)>>(JS_VALUE_GET_INT(_b)&31));\n"
                "      else { _sp=%d; JSValue _r=_RT->sar(ctx,_a,_b); _CHK(_r); _tsv%d=_r; }\n"
                "      _sp=%d; }\n",
                d-1, d-2, d-2, d-2, d-2, d-1);
            break;
        case OP_and: _P94_ENSURE(d-2); _P94_ENSURE(d-1); GEN_BITOP_INT("&",  "band"); break;
        case OP_or:  _P94_ENSURE(d-2); _P94_ENSURE(d-1); GEN_BITOP_INT("|",  "bor");  break;
        case OP_xor: _P94_ENSURE(d-2); _P94_ENSURE(d-1); GEN_BITOP_INT("^",  "bxor"); break;

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
            /* P9.4: typed fast path — negate raw double in _tsd */
            if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) {
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
        case OP_not: /* bitwise ~ */
            _P94_ENSURE(d-1); /* P9.4: box typed slot before JSValue read */
            jit_buf_printf(cb,
                "    { JSValue _a=_tsv%d;\n"
                "      if(JS_VALUE_GET_TAG(_a)==JS_TAG_INT)\n"
                "        _tsv%d=JS_NewInt32(ctx,~JS_VALUE_GET_INT(_a));\n"
                "      else { _sp=%d; JSValue _r=_RT->bnot(ctx,_a); _CHK(_r); _tsv%d=_r; }\n"
                "      _sp=%d; }\n",
                d-1, d-1, d-1, d-1, d);
            break;
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

        /* ---- Increment / decrement ---- */
        /* P9.4/P9.2: inc/dec: pop 1, push 1 at same slot; depth unchanged */
        case OP_inc:
            if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) {
                /* P9.4: typed fast path — increment raw double */
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
            if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) {
                /* P9.4: typed fast path — decrement raw double */
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
            if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) {
                /* P9.4: typed fast path — keep _tsd slots valid, no boxing */
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
            if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) {
                /* P9.4: typed fast path — keep _tsd slots valid, no boxing */
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
                if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) {
                    /* P9.4: typed source — read _tsd directly */
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

/* P9.4: typed fused comparison — both operands are raw doubles in _tsd.
 * Box any typed slots BELOW the two comparison operands before the branch so
 * that the branch-target label (which resets gen_st to JSVAL) sees valid _tsv. */
#define GEN_CMP_FUSE_TSD(c_op, ftgt, fneg) do { \
    { int _bx; for (_bx=0; _bx < d-2 && _bx < gen_sp; _bx++) _P94_ENSURE(_bx); } \
    jit_buf_printf(cb, \
        "    { _sp=%d; if(%s(_tsd%d " c_op " _tsd%d)) goto _L%d; }\n", \
        d-2, (fneg)?"!":"", d-2, d-1, (ftgt)); \
} while(0)

/* P9.4: break-emitting variant of GEN_CMP_FUSE_TSD */
#define GEN_CMP_FUSE_TSD_BRK(c_op, fneg) do { \
    { int _bx; for (_bx=0; _bx < d-2 && _bx < gen_sp; _bx++) _P94_ENSURE(_bx); } \
    jit_buf_printf(cb, \
        "    { _sp=%d; if(%s(_tsd%d " c_op " _tsd%d)) break; }\n", \
        d-2, (fneg)?"!":"", d-2, d-1); \
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
            int _bn = (gen_sp <= d && _GS_TOP2()>=JIT_T_NUMBER && _GS_TOP2()<=JIT_T_INT && _GS_TOP()>=JIT_T_NUMBER && _GS_TOP()<=JIT_T_INT);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn) GEN_CMP_FUSE_TSD("<",  _fi.tgt, _fi.negate);
                else { _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                       GEN_CMP_FUSE_GEN("<", "<", "_RT->lt(ctx,_a,_b)",  _fi.tgt, _fi.negate); }
            } else {
                _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                GEN_CMP_UNFUSED("<", "<", "_RT->lt(ctx,_a,_b)");
            }
            _GS_DROP(2); if (!_fi.fuse) _GS_PUSH(JIT_T_JSVAL);
            break;
        }
        case OP_lte: {
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            int _bn = (gen_sp <= d && _GS_TOP2()>=JIT_T_NUMBER && _GS_TOP2()<=JIT_T_INT && _GS_TOP()>=JIT_T_NUMBER && _GS_TOP()<=JIT_T_INT);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn) GEN_CMP_FUSE_TSD("<=", _fi.tgt, _fi.negate);
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
            /* a > b  ≡  b < a  (strict):  vtable uses lt(b,a) */
            JitFuseInfo _fi = jit_check_fuse(bc, pc+sz, bc_len, op_sz, sr);
            int _bn = (gen_sp <= d && _GS_TOP2()>=JIT_T_NUMBER && _GS_TOP2()<=JIT_T_INT && _GS_TOP()>=JIT_T_NUMBER && _GS_TOP()<=JIT_T_INT);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn) GEN_CMP_FUSE_TSD(">",  _fi.tgt, _fi.negate);
                else { _P94_ENSURE(d-2); _P94_ENSURE(d-1);
                       GEN_CMP_FUSE_GEN(">", ">", "_RT->lt(ctx,_b,_a)",  _fi.tgt, _fi.negate); }
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
            int _bn = (gen_sp <= d && _GS_TOP2()>=JIT_T_NUMBER && _GS_TOP2()<=JIT_T_INT && _GS_TOP()>=JIT_T_NUMBER && _GS_TOP()<=JIT_T_INT);
            if (_fi.fuse) {
                sz += _fi.extra_sz;
                if (_bn) GEN_CMP_FUSE_TSD(">=", _fi.tgt, _fi.negate);
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
            if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) {
                /* P9.4: typed fast path — 0.0 or NaN is falsy.
                 * Box any typed slots below the condition before branching. */
                { int _bx; for (_bx=0; _bx < d-1 && _bx < gen_sp-1; _bx++) _P94_ENSURE(_bx); }
                if (p93_depth > 0 && (uint32_t)tgt == p93_active[p93_depth-1].exit_pc)
                    jit_buf_printf(cb,
                        "    { _sp=%d; if(_tsd%d==0.0||_tsd%d!=_tsd%d) break; }\n",
                        d-1, d-1, d-1, d-1);
                else
                    jit_buf_printf(cb,
                        "    { _sp=%d; if(_tsd%d==0.0||_tsd%d!=_tsd%d) goto _L%d; }\n",
                        d-1, d-1, d-1, d-1, tgt);
            } else {
                /* P9.3: emit break instead of goto if target is loop exit */
                _P94_ENSURE(d-1);
                if (p93_depth > 0 && (uint32_t)tgt == p93_active[p93_depth-1].exit_pc)
                    jit_buf_printf(cb,
                        "    { JSValue _v=_tsv%d; _sp=%d; int _b=_BOOL(_v);"
                        " _FREE(_v); if(!_b) break; }\n", d-1, d-1);
                else
                    jit_buf_printf(cb,
                        "    { JSValue _v=_tsv%d; _sp=%d; int _b=_BOOL(_v);"
                        " _FREE(_v); if(!_b) goto _L%d; }\n", d-1, d-1, tgt);
            }
            break;
        }
        case OP_if_true: {
            int32_t delta = (int32_t)bc_u32(&bc[pc+1]);
            int tgt = pc + 1 + delta;
            if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) {
                /* P9.4: typed fast path — non-zero and non-NaN is truthy.
                 * Box any typed slots below the condition before branching. */
                { int _bx; for (_bx=0; _bx < d-1 && _bx < gen_sp-1; _bx++) _P94_ENSURE(_bx); }
                if (p93_depth > 0 && (uint32_t)tgt == p93_active[p93_depth-1].exit_pc)
                    jit_buf_printf(cb,
                        "    { _sp=%d; if(_tsd%d!=0.0&&_tsd%d==_tsd%d) break; }\n",
                        d-1, d-1, d-1, d-1);
                else
                    jit_buf_printf(cb,
                        "    { _sp=%d; if(_tsd%d!=0.0&&_tsd%d==_tsd%d) goto _L%d; }\n",
                        d-1, d-1, d-1, d-1, tgt);
            } else {
                /* P9.3: emit break instead of goto if target is loop exit */
                _P94_ENSURE(d-1);
                if (p93_depth > 0 && (uint32_t)tgt == p93_active[p93_depth-1].exit_pc)
                    jit_buf_printf(cb,
                        "    { JSValue _v=_tsv%d; _sp=%d; int _b=_BOOL(_v);"
                        " _FREE(_v); if(_b) break; }\n", d-1, d-1);
                else
                    jit_buf_printf(cb,
                        "    { JSValue _v=_tsv%d; _sp=%d; int _b=_BOOL(_v);"
                        " _FREE(_v); if(_b) goto _L%d; }\n", d-1, d-1, tgt);
            }
            break;
        }
        case OP_goto: {
            int32_t delta = (int32_t)bc_u32(&bc[pc+1]);
            int tgt = pc + 1 + delta;
            /* P9.3: check if this is a loop back-edge or break-to-exit */
            if (p93_depth > 0) {
                P93Loop *_cl = &p93_active[p93_depth - 1];
                if ((uint32_t)tgt == _cl->header_pc && tgt <= pc) {
                    /* Back-edge: close while(1){ */
                    jit_buf_str(cb, "} /* while */\n");
                    p93_depth--;
                    break;
                }
                if ((uint32_t)tgt == _cl->exit_pc) {
                    jit_buf_str(cb, "    break;\n");
                    break;
                }
            }
            /* P9.4: box typed surviving slots before goto — target label resets gen_st */
            { int _bx; for (_bx=0; _bx < gen_sp; _bx++) _P94_ENSURE(_bx); }
            jit_buf_printf(cb, "    goto _L%d;\n", tgt);
            break;
        }
        case OP_if_false8: {
            int tgt = pc + 1 + (int)(int8_t)bc[pc+1];
            if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) {
                /* P9.4: typed fast path — box surviving slots below condition */
                { int _bx; for (_bx=0; _bx < d-1 && _bx < gen_sp-1; _bx++) _P94_ENSURE(_bx); }
                if (p93_depth > 0 && (uint32_t)tgt == p93_active[p93_depth-1].exit_pc)
                    jit_buf_printf(cb,
                        "    { _sp=%d; if(_tsd%d==0.0||_tsd%d!=_tsd%d) break; }\n",
                        d-1, d-1, d-1, d-1);
                else
                    jit_buf_printf(cb,
                        "    { _sp=%d; if(_tsd%d==0.0||_tsd%d!=_tsd%d) goto _L%d; }\n",
                        d-1, d-1, d-1, d-1, tgt);
            } else {
                /* P9.3: emit break instead of goto if target is loop exit */
                _P94_ENSURE(d-1);
                if (p93_depth > 0 && (uint32_t)tgt == p93_active[p93_depth-1].exit_pc)
                    jit_buf_printf(cb,
                        "    { JSValue _v=_tsv%d; _sp=%d; int _b=_BOOL(_v);"
                        " _FREE(_v); if(!_b) break; }\n", d-1, d-1);
                else
                    jit_buf_printf(cb,
                        "    { JSValue _v=_tsv%d; _sp=%d; int _b=_BOOL(_v);"
                        " _FREE(_v); if(!_b) goto _L%d; }\n", d-1, d-1, tgt);
            }
            break;
        }
        case OP_if_true8: {
            int tgt = pc + 1 + (int)(int8_t)bc[pc+1];
            if (gen_sp > 0 && gen_st[gen_sp-1] >= JIT_T_NUMBER) {
                /* P9.4: typed fast path — box surviving slots below condition */
                { int _bx; for (_bx=0; _bx < d-1 && _bx < gen_sp-1; _bx++) _P94_ENSURE(_bx); }
                if (p93_depth > 0 && (uint32_t)tgt == p93_active[p93_depth-1].exit_pc)
                    jit_buf_printf(cb,
                        "    { _sp=%d; if(_tsd%d!=0.0&&_tsd%d==_tsd%d) break; }\n",
                        d-1, d-1, d-1, d-1);
                else
                    jit_buf_printf(cb,
                        "    { _sp=%d; if(_tsd%d!=0.0&&_tsd%d==_tsd%d) goto _L%d; }\n",
                        d-1, d-1, d-1, d-1, tgt);
            } else {
                /* P9.3: emit break instead of goto if target is loop exit */
                _P94_ENSURE(d-1);
                if (p93_depth > 0 && (uint32_t)tgt == p93_active[p93_depth-1].exit_pc)
                    jit_buf_printf(cb,
                        "    { JSValue _v=_tsv%d; _sp=%d; int _b=_BOOL(_v);"
                        " _FREE(_v); if(_b) break; }\n", d-1, d-1);
                else
                    jit_buf_printf(cb,
                        "    { JSValue _v=_tsv%d; _sp=%d; int _b=_BOOL(_v);"
                        " _FREE(_v); if(_b) goto _L%d; }\n", d-1, d-1, tgt);
            }
            break;
        }
        case OP_goto8: {
            int tgt = pc + 1 + (int)(int8_t)bc[pc+1];
            /* P9.3: check if this is a loop back-edge or break-to-exit */
            if (p93_depth > 0) {
                P93Loop *_cl = &p93_active[p93_depth - 1];
                if ((uint32_t)tgt == _cl->header_pc && tgt <= pc) {
                    jit_buf_str(cb, "} /* while */\n");
                    p93_depth--;
                    break;
                }
                if ((uint32_t)tgt == _cl->exit_pc) {
                    jit_buf_str(cb, "    break;\n");
                    break;
                }
            }
            /* P9.4: box typed surviving slots before goto */
            { int _bx; for (_bx=0; _bx < gen_sp; _bx++) _P94_ENSURE(_bx); }
            jit_buf_printf(cb, "    goto _L%d;\n", tgt);
            break;
        }
        case OP_goto16: {
            int tgt = pc + 1 + (int)(int16_t)bc_u16(&bc[pc+1]);
            /* P9.3: check if this is a loop back-edge or break-to-exit */
            if (p93_depth > 0) {
                P93Loop *_cl = &p93_active[p93_depth - 1];
                if ((uint32_t)tgt == _cl->header_pc && tgt <= pc) {
                    jit_buf_str(cb, "} /* while */\n");
                    p93_depth--;
                    break;
                }
                if ((uint32_t)tgt == _cl->exit_pc) {
                    jit_buf_str(cb, "    break;\n");
                    break;
                }
            }
            /* P9.4: box typed surviving slots before goto */
            { int _bx; for (_bx=0; _bx < gen_sp; _bx++) _P94_ENSURE(_bx); }
            jit_buf_printf(cb, "    goto _L%d;\n", tgt);
            break;
        }

        /* ---- Property access (with inline property cache) ---- */
        /* P9.4/P9.2: get_field: pop obj, push result; depth unchanged (d->d) */
        case OP_get_field: {
            uint32_t atom = bc_u32(&bc[pc+1]);
            _P94_ENSURE(d-1); /* P9.4: box typed obj slot (defensive) */
            jit_buf_printf(cb,
                "    { static JSJITICEntry _ic%d={NULL,0};\n"
                "      JSValue _o=_tsv%d, _r;\n"
                "      if (js_likely(js_jit_ic_check(_o,&_ic%d)))\n"
                "          _r=js_jit_ic_read(ctx,_o,_ic%d.slot);\n"
                "      else { _r=_RT->get_prop(ctx,_o,(JSAtom)%uu);\n"
                "             js_jit_ic_fill_get(ctx,_o,(JSAtom)%uu,&_ic%d); }\n"
                "      _FREE(_o); _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                pc, d-1, pc, pc, atom, atom, pc, d-1, d-1, d);
            break;
        }
        case OP_get_field2: { /* keep object on stack; push result: depth d -> d+1 */
            uint32_t atom = bc_u32(&bc[pc+1]);
            _P94_ENSURE(d-1); /* P9.4: box typed obj slot (defensive) */
            jit_buf_printf(cb,
                "    { static JSJITICEntry _ic%d={NULL,0};\n"
                "      JSValue _r;\n"
                "      if (js_likely(js_jit_ic_check(_tsv%d,&_ic%d)))\n"
                "          _r=js_jit_ic_read(ctx,_tsv%d,_ic%d.slot);\n"
                "      else { _r=_RT->get_prop(ctx,_tsv%d,(JSAtom)%uu);\n"
                "             js_jit_ic_fill_get(ctx,_tsv%d,(JSAtom)%uu,&_ic%d); }\n"
                "      _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                pc, d-1, pc, d-1, pc, d-1, atom, d-1, atom, pc, d, d, d+1);
            break;
        }
        case OP_put_field: { /* pop val, pop obj; depth d -> d-2 */
            uint32_t atom = bc_u32(&bc[pc+1]);
            _P94_ENSURE(d-1); /* P9.4: box typed val slot before use as JSValue */
            jit_buf_printf(cb,
                "    { static JSJITICEntry _ic%d={NULL,0};\n"
                "      JSValue _v=_tsv%d, _o=_tsv%d; _sp=%d; int _ret;\n"
                "      if (js_likely(js_jit_ic_check(_o,&_ic%d)))\n"
                "          _ret=js_jit_ic_write(ctx,_o,_v,_ic%d.slot);\n"
                "      else { _ret=_RT->set_prop(ctx,_o,(JSAtom)%uu,_v);\n"
                "             js_jit_ic_fill_put(ctx,_o,(JSAtom)%uu,&_ic%d); }\n"
                "      _FREE(_o); if(_ret<0) goto _ex; }\n",
                pc, d-1, d-2, d-2, pc, pc, atom, atom, pc);
            break;
        }
        /* P8.5: dense array element fast path.
         * Guard: obj is JS_TAG_OBJECT && idx is JS_TAG_INT.
         * Fast path: js_jit_array_get/set hit the u.array.values[] directly.
         * Slow path: _RT->get/set_array_el goes through JS_ValueToAtom + GetProperty.
         * P9.4/P9.2: get_array_el: box typed idx, pop idx(_tsv{d-1}), pop obj(_tsv{d-2}), push result(_tsv{d-2}); depth d->d-1 */
        case OP_get_array_el:
            _P94_ENSURE(d-1); /* P9.4: box typed idx slot before index check */
            jit_buf_printf(cb,
                "    { JSValue _idx=_tsv%d,_o=_tsv%d;\n"
                "      JSValue _r;\n"
                "      if(js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT"
                               "&&JS_VALUE_GET_TAG(_idx)==JS_TAG_INT)\n"
                "         &&js_jit_array_get(ctx,_o,(uint32_t)JS_VALUE_GET_INT(_idx),&_r))\n"
                "          ;/* fast hit */\n"
                "      else _r=_RT->get_array_el(ctx,_o,_idx);\n"
                "      _FREE(_o);_FREE(_idx); _sp=%d; _CHK(_r); _tsv%d=_r; _sp=%d; }\n",
                d-1, d-2, d-2, d-2, d-1);
            break;
        /* P9.4/P9.2: put_array_el: box typed slots, pop v(_tsv{d-1}), idx(_tsv{d-2}), obj(_tsv{d-3}); depth d->d-3 */
        case OP_put_array_el:
            _P94_ENSURE(d-3); /* P9.4: box typed obj slot (unlikely but safe) */
            _P94_ENSURE(d-2); /* P9.4: box typed idx slot before index check */
            _P94_ENSURE(d-1); /* P9.4: box typed val slot before use as JSValue */
            jit_buf_printf(cb,
                "    { JSValue _v=_tsv%d,_idx=_tsv%d,_o=_tsv%d;\n"
                "      _sp=%d; int _ret;\n"
                "      if(js_likely(JS_VALUE_GET_TAG(_o)==JS_TAG_OBJECT"
                               "&&JS_VALUE_GET_TAG(_idx)==JS_TAG_INT)\n"
                "         &&js_jit_array_set(ctx,_o,(uint32_t)JS_VALUE_GET_INT(_idx),_v))\n"
                "          _ret=0;/* fast hit, _v consumed */\n"
                "      else _ret=_RT->set_array_el(ctx,_o,_idx,_v);\n"
                "      _FREE(_o);_FREE(_idx);if(_ret<0) goto _ex; }\n",
                d-1, d-2, d-3, d-3);
            break;
        /* P9.4/P9.2: get_length: box typed slot, peek obj at _tsv{d-1}, replace with result; depth unchanged */
        case OP_get_length:
            _P94_ENSURE(d-1); /* P9.4: box typed obj slot (defensive) */
            jit_buf_printf(cb,
                "    { JSValue _obj=_tsv%d;\n"
                "      JSValue _r=_RT->get_prop(ctx,_obj,(JSAtom)%uu);\n"
                "      _sp=%d; _CHK(_r); _FREE(_tsv%d); _tsv%d=_r; _sp=%d; }\n",
                d-1, (unsigned)JS_ATOM_length, d-1, d-1, d-1, d);
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
                int fslot = d - nargs - 1; /* absolute slot index of func */
                /* P9.4: box any typed arg slots before building the args array */
                for (int _aj = 0; _aj < nargs; _aj++)
                    _P94_ENSURE(d-nargs+_aj);
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
            _P94_ENSURE(d-1); /* P9.4: box typed slot before reading as JSValue */
            jit_buf_printf(cb, "    { JSValue _r=_tsv%d; _sp=%d;\n", d-1, d-1);
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
            jit_buf_str(cb, "    {");
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

            /* --- get_var: SELF_FUNC if loading the function's own name, else JSVAL.
             * P8.2: marks the stack slot so OP_call* can emit a direct C call. --- */
            case OP_get_var: {
                int _vi = (int)bc_u16(&bc[pc+1]);
                JSAtom _va = js_jit_fb_get_closure_var_atom(b, _vi);
                _gs_push = (self_func_atom != JS_ATOM_NULL && _va == self_func_atom)
                           ? JIT_T_SELF_FUNC : JIT_T_JSVAL;
                break;
            }
            /* get_var_ref* also push JSVAL (never self-func) */
            case OP_get_var_ref: case OP_get_var_ref_check:
            case OP_get_var_ref0: case OP_get_var_ref1:
            case OP_get_var_ref2: case OP_get_var_ref3:
                _gs_push = JIT_T_JSVAL; break;

            /* --- get_length: JSVAL (result stored in _tsv via _RT->get_prop, not _tsd) --- */
            case OP_get_length: _gs_drop=1; _gs_push=JIT_T_JSVAL; break;

            /* --- pow: always JSVAL result (calls runtime, no typed fast path) --- */
            case OP_pow: _gs_drop=2; _gs_push=JIT_T_JSVAL; break;

            /* --- Arithmetic: INT if both INT, NUMBER if both >=NUMBER, else JSVAL --- */
            case OP_add: case OP_sub: case OP_mul: case OP_div: case OP_mod: {
                uint8_t _t2=_GS_TOP2(), _t1=_GS_TOP();
                _gs_drop = 2;
                _gs_push = (_t2>=JIT_T_NUMBER&&_t1>=JIT_T_NUMBER)
                           ? ((_t2==JIT_T_INT&&_t1==JIT_T_INT)?JIT_T_INT:JIT_T_NUMBER)
                           : JIT_T_JSVAL;
                break;
            }
            /* --- Bitwise: result is an int32 stored in _tsv, not _tsd → JSVAL --- */
            case OP_shl: case OP_sar: case OP_shr:
            case OP_and: case OP_or:  case OP_xor: _gs_drop=2; _gs_push=JIT_T_JSVAL; break;
            case OP_not:                            _gs_drop=1; _gs_push=JIT_T_JSVAL; break;

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
#undef _P94_ENSURE
#undef LNAME
#undef ANAME

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
                        const char *js_func_name, uint64_t bc_hash,
                        JSRuntime *rt)
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

    gen_preamble(cb, bc_hash, var_count, arg_count, stack_size,
                 closure_var_count, cpool_count, fname_out, fname_sz,
                 local_type, js_func_name, varnames);

    int unsup = 0;
    if (gen_body(cb, bc, bc_len, &sr, op_sz, op_sz_count,
                 var_count, arg_count, stack_size, &unsup, local_type, b,
                 bc_hash, varnames) < 0) {
        *unsupported = unsup;
        jit_buf_free(cb);
        scan_result_free(&sr);
        free(local_type);
        jit_free_varnames(varnames, arg_count + var_count);
        return -1;
    }

    gen_footer(cb, var_count, arg_count, varnames, stack_size);

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
