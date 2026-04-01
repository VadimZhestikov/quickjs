/*
 * QuickJS JIT Compiler
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
#ifndef QUICKJS_JIT_H
#define QUICKJS_JIT_H

#include "quickjs.h"

#ifdef CONFIG_JIT

/* Forward declarations of internal QuickJS types needed by the JIT. */
typedef struct JSFunctionBytecode JSFunctionBytecode;
typedef struct JSContext JSContext;
typedef struct JSVarRef JSVarRef;

/*
 * JSJITFunc — the calling convention for every JIT-compiled function.
 *
 * Matches JS_CallInternal's interface exactly so the hot-path dispatcher
 * can call a JIT function with no argument marshalling:
 *
 *   ctx       — the current JS context
 *   this_val  — the 'this' value for the call
 *   argc      — number of arguments passed
 *   argv      — argument array (may be modified for put_arg)
 *   cpool     — constant pool from JSFunctionBytecode (read-only)
 *   var_refs  — closure variable references (read/write through pvalue)
 *
 * Returns JS_EXCEPTION (tag == JS_TAG_EXCEPTION) on error, otherwise the
 * function's return value.  Ownership: the caller receives a new reference
 * (i.e. refcount already incremented on behalf of the caller).
 */
typedef JSValue (*JSJITFunc)(JSContext    *ctx,
                             JSValue       this_val,
                             int           argc,
                             JSValue      *argv,
                             JSValue      *cpool,
                             JSVarRef    **var_refs);

/*
 * JSJITRuntime — vtable of QuickJS runtime helpers callable from JIT code.
 *
 * Generated C code (compiled by TCC or GCC) cannot call static-inline
 * functions directly (JS_DupValue, JS_FreeValue) nor internal QuickJS
 * symbols without linker visibility.  Every runtime operation goes through
 * this single struct, registered as one symbol ("__jit_rt") with each
 * TCCState / dlopen'd .so.
 *
 * All functions follow QuickJS ownership conventions:
 *   - "consuming" arguments: the callee frees them
 *   - return value: caller owns a new reference (must JS_FreeValue when done)
 */
typedef struct JSJITRuntime {
    /* ------------------------------------------------------------------ */
    /* Arithmetic — full dynamic dispatch (handles all JS types)           */
    /* ------------------------------------------------------------------ */
    JSValue (*add)(JSContext *, JSValue a, JSValue b);
    JSValue (*sub)(JSContext *, JSValue a, JSValue b);
    JSValue (*mul)(JSContext *, JSValue a, JSValue b);
    JSValue (*div)(JSContext *, JSValue a, JSValue b);
    JSValue (*mod)(JSContext *, JSValue a, JSValue b);
    JSValue (*pow)(JSContext *, JSValue a, JSValue b);

    /* Bitwise — all require ToInt32 */
    JSValue (*shl)(JSContext *, JSValue a, JSValue b);
    JSValue (*sar)(JSContext *, JSValue a, JSValue b);
    JSValue (*shr)(JSContext *, JSValue a, JSValue b);
    JSValue (*band)(JSContext *, JSValue a, JSValue b);
    JSValue (*bor)(JSContext *, JSValue a, JSValue b);
    JSValue (*bxor)(JSContext *, JSValue a, JSValue b);

    /* Unary */
    JSValue (*neg)(JSContext *, JSValue a);
    JSValue (*plus)(JSContext *, JSValue a);   /* ToNumber */
    JSValue (*bnot)(JSContext *, JSValue a);   /* ~ */
    JSValue (*type_of)(JSContext *, JSValue a);

    /* ------------------------------------------------------------------ */
    /* Comparisons — return JS_TAG_BOOL result or JS_EXCEPTION            */
    /* ------------------------------------------------------------------ */
    JSValue (*lt)(JSContext *, JSValue a, JSValue b);
    JSValue (*lte)(JSContext *, JSValue a, JSValue b);
    JSValue (*eq)(JSContext *, JSValue a, JSValue b);       /* == */
    JSValue (*strict_eq)(JSContext *, JSValue a, JSValue b); /* === */

    /* ------------------------------------------------------------------ */
    /* Reference counting — wraps the static-inline originals             */
    /* ------------------------------------------------------------------ */
    JSValue (*dup)(JSContext *, JSValue v);    /* JS_DupValue  */
    void    (*free)(JSContext *, JSValue v);   /* JS_FreeValue */

    /* ------------------------------------------------------------------ */
    /* Property access                                                     */
    /* ------------------------------------------------------------------ */
    JSValue (*get_prop)(JSContext *, JSValue obj, JSAtom atom);
    int     (*set_prop)(JSContext *, JSValue obj, JSAtom atom, JSValue val);
    JSValue (*get_array_el)(JSContext *, JSValue obj, JSValue idx);
    int     (*set_array_el)(JSContext *, JSValue obj, JSValue idx, JSValue val);

    /* ------------------------------------------------------------------ */
    /* Function calls                                                      */
    /* ------------------------------------------------------------------ */
    JSValue (*call)(JSContext *, JSValue func, JSValue this_val,
                    int argc, JSValue *argv);
    JSValue (*call_constructor)(JSContext *, JSValue ctor,
                                JSValue new_target,
                                int argc, JSValue *argv);

    /* ------------------------------------------------------------------ */
    /* Exceptions                                                          */
    /* ------------------------------------------------------------------ */
    /* Throw a TypeError with a printf-style message; returns JS_EXCEPTION */
    JSValue (*throw_type_error)(JSContext *, const char *fmt, ...);
    /* Throw an arbitrary value (takes ownership of val) */
    JSValue (*throw_val)(JSContext *, JSValue val);
} JSJITRuntime;

/*
 * Global singleton vtable.  Initialised in js_jit_init() and never changes
 * after that — safe to read from any thread without locking.
 */
extern const JSJITRuntime js_jit_rt;

/* ======================================================================= */
/* JSFunctionBytecode field accessors (defined in quickjs.c)              */
/*                                                                         */
/* JSFunctionBytecode is defined inside quickjs.c and not exposed via a   */
/* shared header.  These accessor functions let quickjs-jit.c read and    */
/* write the fields it needs without seeing the full struct layout.        */
/* Phase 3 will move the struct to a shared header for bulk access.       */
/* ======================================================================= */
#ifdef CONFIG_JIT
/* Read handle/tier for cleanup, then clear all JIT pointers */
uint8_t   js_jit_fb_get_tier(JSFunctionBytecode *b);
void     *js_jit_fb_get_handle(JSFunctionBytecode *b);
void     *js_jit_fb_get_old_handle(JSFunctionBytecode *b);
void      js_jit_fb_clear_handles(JSFunctionBytecode *b);

uint8_t   js_jit_fb_func_kind(JSFunctionBytecode *b);
uint8_t   js_jit_fb_has_simple_params(JSFunctionBytecode *b);
uint8_t   js_jit_fb_need_home_object(JSFunctionBytecode *b);
uint8_t   js_jit_fb_is_derived_ctor(JSFunctionBytecode *b);
uint8_t   js_jit_fb_is_eval(JSFunctionBytecode *b);
uint8_t   js_jit_fb_jit_no_compile(JSFunctionBytecode *b);
void      js_jit_fb_set_no_compile(JSFunctionBytecode *b);
JSJITFunc js_jit_fb_get_func(JSFunctionBytecode *b);
void      js_jit_fb_set_func(JSFunctionBytecode *b, JSJITFunc f,
                              void *handle, int tier);
int       js_jit_fb_inc_count(JSFunctionBytecode *b);

/* Bytecode / metadata accessors for the code generator */
const uint8_t *js_jit_fb_get_bytecode(JSFunctionBytecode *b, int *len);
int            js_jit_fb_get_arg_count(JSFunctionBytecode *b);
int            js_jit_fb_get_var_count(JSFunctionBytecode *b);
int            js_jit_fb_get_stack_size(JSFunctionBytecode *b);
int            js_jit_fb_get_closure_var_count(JSFunctionBytecode *b);
int            js_jit_fb_get_cpool_count(JSFunctionBytecode *b);
/* Opcode size table: opcode_size[opcode] = instruction length in bytes */
const uint8_t *js_jit_get_opcode_size_table(int *count);
#endif

/* ======================================================================= */
/* Internal operator helpers (defined in quickjs.c, used by the vtable)   */
/* ======================================================================= */
#ifdef CONFIG_JIT
JSValue js_jit_op_add(JSContext *, JSValue, JSValue);
JSValue js_jit_op_sub(JSContext *, JSValue, JSValue);
JSValue js_jit_op_mul(JSContext *, JSValue, JSValue);
JSValue js_jit_op_div(JSContext *, JSValue, JSValue);
JSValue js_jit_op_mod(JSContext *, JSValue, JSValue);
JSValue js_jit_op_shl(JSContext *, JSValue, JSValue);
JSValue js_jit_op_sar(JSContext *, JSValue, JSValue);
JSValue js_jit_op_shr(JSContext *, JSValue, JSValue);
JSValue js_jit_op_band(JSContext *, JSValue, JSValue);
JSValue js_jit_op_bor(JSContext *, JSValue, JSValue);
JSValue js_jit_op_bxor(JSContext *, JSValue, JSValue);
JSValue js_jit_op_neg(JSContext *, JSValue);
JSValue js_jit_op_plus(JSContext *, JSValue);
JSValue js_jit_op_bnot(JSContext *, JSValue);
JSValue js_jit_op_lt(JSContext *, JSValue, JSValue);
JSValue js_jit_op_lte(JSContext *, JSValue, JSValue);
JSValue js_jit_op_gt(JSContext *, JSValue, JSValue);
JSValue js_jit_op_gte(JSContext *, JSValue, JSValue);
JSValue js_jit_op_eq(JSContext *, JSValue, JSValue);
JSValue js_jit_op_strict_eq(JSContext *, JSValue, JSValue);
JSValue js_jit_op_type_of(JSContext *, JSValue);
#endif

/* ======================================================================= */
/* Public API                                                               */
/* ======================================================================= */

/*
 * js_jit_is_eligible() — returns 1 if b can be JIT-compiled, 0 otherwise.
 * Excludes: generators, async functions, eval, complex parameter lists,
 * class methods needing home_object, derived constructors.
 */
int js_jit_is_eligible(JSFunctionBytecode *b);

/*
 * js_jit_init() — must be called once before any JIT compilation, typically
 * inside JS_NewRuntime().  Fills js_jit_rt and starts the GCC background
 * thread pool (Phase 4).
 */
void js_jit_init(void);

/*
 * js_jit_free() — called inside JS_FreeRuntime().  Shuts down the GCC
 * background thread and waits for all pending compilations to finish.
 */
void js_jit_free(void);

/*
 * js_jit_compile_tcc() — Tier-1 compilation.  Translates b's bytecode to C,
 * compiles with libtcc, and atomically installs the resulting function pointer
 * in b->jit_func.  If compilation fails for any reason (unsupported opcodes,
 * TCC error) b->jit_no_compile is set so the function is never retried.
 *
 * Called from JS_CallInternal when b->jit_call_count reaches JIT_THRESHOLD_TCC.
 * May be called from any thread; uses a CAS to guard against races.
 */
void js_jit_compile_tcc(JSContext *ctx, JSFunctionBytecode *b);

/*
 * js_jit_queue_gcc() — Tier-2 upgrade.  Enqueues b for recompilation with
 * GCC -O2 on the background worker thread.  The TCC-compiled version
 * continues running while GCC compiles; on completion the pointer is swapped
 * atomically.
 *
 * Called from JS_CallInternal when b->jit_call_count reaches JIT_THRESHOLD_GCC.
 */
void js_jit_queue_gcc(JSContext *ctx, JSFunctionBytecode *b);

/*
 * js_jit_free_bytecode() — cleanup hook, called from free_function_bytecode().
 * Frees the TCCState (tier-1) or dlclose's the .so handle (tier-2).
 */
void js_jit_free_bytecode(JSFunctionBytecode *b);

#endif /* CONFIG_JIT */
#endif /* QUICKJS_JIT_H */
