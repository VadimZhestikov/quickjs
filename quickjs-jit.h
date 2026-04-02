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
 * Generated C code (compiled by GCC) cannot call static-inline functions
 * directly (JS_DupValue, JS_FreeValue) nor internal QuickJS symbols without
 * linker visibility.  Every runtime operation goes through this single struct,
 * registered as one symbol ("js_jit_rt") visible to each dlopen'd .so.
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
    /* Variable reference access (JSVarRef is opaque to generated code)   */
    /* ------------------------------------------------------------------ */
    /* Returns pointer to the stored JSValue inside a JSVarRef object.
     * Generated C uses this instead of accessing ->pvalue directly since
     * JSVarRef is defined in quickjs.c (not public).                     */
    JSValue *(*var_ref_value)(JSVarRef *ref);

    /* ------------------------------------------------------------------ */
    /* Property access                                                     */
    /* ------------------------------------------------------------------ */
    JSValue (*get_prop)(JSContext *, JSValue obj, JSAtom atom);
    int     (*set_prop)(JSContext *, JSValue obj, JSAtom atom, JSValue val);
    /* Slow path for OP_get_var when value is JS_UNINITIALIZED (deleted global or TDZ) */
    JSValue (*get_var_slow)(JSContext *, JSAtom atom, int is_lexical);
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

    /* ------------------------------------------------------------------ */
    /* P8.2 — interrupt poll for direct self-recursive JIT calls           */
    /* ------------------------------------------------------------------ */
    /* Mirrors js_poll_interrupts (static inline in quickjs.c).
     * Called once per direct recursive call to honour JS_SetInterruptHandler.
     * Returns non-zero and sets an exception on the context if interrupted. */
    int (*poll_interrupts)(JSContext *);
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
JSAtom         js_jit_fb_get_closure_var_atom(JSFunctionBytecode *b, int idx);
int            js_jit_fb_get_closure_var_is_lexical(JSFunctionBytecode *b, int idx);
int            js_jit_fb_get_cpool_count(JSFunctionBytecode *b);
/* Opcode size table: opcode_size[opcode] = instruction length in bytes */
const uint8_t *js_jit_get_opcode_size_table(int *count);
/* Function name as a C string (static buf — for debug/logging only) */
const char    *js_jit_fb_get_func_name(JSRuntime *rt, JSFunctionBytecode *b);
/* P8.2: function's own name atom (JS_ATOM_NULL if anonymous) */
JSAtom         js_jit_fb_get_func_atom(JSFunctionBytecode *b);
/* P8.2: interrupt poll — wraps js_poll_interrupts (static inline) for vtable use */
int            js_jit_poll_interrupts(JSContext *ctx);
#endif

/* ======================================================================= */
/* Variable reference accessor (defined in quickjs.c)                     */
/* ======================================================================= */
#ifdef CONFIG_JIT
/* Returns the address of the JSValue stored inside a JSVarRef.
 * Generated C cannot access JSVarRef->pvalue directly since JSVarRef is
 * defined in quickjs.c (not a public header). */
JSValue *js_jit_var_ref_value(JSVarRef *ref);
#endif

/* ======================================================================= */
/* Inline Property Cache (IC) — Phase 6.2                                 */
/* Implemented in quickjs.c; used by JIT-generated C code.                */
/* ======================================================================= */
#ifdef CONFIG_JIT
/*
 * Monomorphic IC entry: caches the shape pointer and property slot index
 * for a single (object-shape, atom) pair.  Zero-initialised entries have
 * shape == NULL and are always treated as misses.
 */
typedef struct {
    void     *shape;  /* JSShape* — opaque outside quickjs.c */
    uint32_t  slot;   /* index into JSObject->prop[] */
} JSJITICEntry;

/*
 * js_jit_ic_check: returns non-zero iff obj is an OBJECT whose shape
 * matches ic->shape (fast inline shape guard).
 */
int js_jit_ic_check(JSValue obj, const JSJITICEntry *ic);

/*
 * js_jit_ic_fill_get: populate ic after a get_field miss.
 * Only caches simple own data properties (JS_PROP_NORMAL, not accessor/varref).
 * Returns 1 if cached, 0 if not cacheable (prototype property, accessor, etc.).
 */
int js_jit_ic_fill_get(JSContext *ctx, JSValue obj, JSAtom atom,
                       JSJITICEntry *ic);

/*
 * js_jit_ic_fill_put: populate ic after a put_field miss.
 * Only caches writable own data properties.
 * Returns 1 if cached, 0 if not cacheable.
 */
int js_jit_ic_fill_put(JSContext *ctx, JSValue obj, JSAtom atom,
                       JSJITICEntry *ic);

/*
 * js_jit_ic_read: read a property from the cached slot.
 * Assumes ic check has already passed.  Returns a new reference.
 */
JSValue js_jit_ic_read(JSContext *ctx, JSValue obj, uint32_t slot);

/*
 * js_jit_ic_write: write val to the cached slot (transfers ownership of val).
 * Assumes ic check has already passed.  Always returns 0.
 */
int js_jit_ic_write(JSContext *ctx, JSValue obj, JSValue val, uint32_t slot);
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
JSValue js_jit_op_pow(JSContext *, JSValue, JSValue);
JSValue js_jit_op_get_var_slow(JSContext *, JSAtom atom, int is_lexical);
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
 * js_jit_init() — starts the GCC background worker thread.
 * Called once from JS_NewRuntime() before any JS execution.
 */
void js_jit_init(void);

/*
 * js_jit_free() — drains the job queue and shuts down the worker thread.
 * Called from JS_FreeRuntime() before GC begins freeing bytecodes.
 */
void js_jit_free(void);

/*
 * js_jit_queue_gcc() — enqueue b for GCC -O2 background compilation.
 * Generates C source synchronously, then hands the job to the worker thread.
 * Returns immediately; jit_func is installed atomically when GCC finishes.
 *
 * Called from JS_CallInternal when b->jit_call_count reaches JIT_THRESHOLD_GCC.
 */
void js_jit_queue_gcc(JSContext *ctx, JSFunctionBytecode *b);

/*
 * js_jit_free_bytecode() — cleanup hook, called from free_function_bytecode().
 * dlclose()s the compiled .so handle if present.
 */
void js_jit_free_bytecode(JSFunctionBytecode *b);

/*
 * js_jit_compile_all() — recursively enqueue all eligible nested functions
 * in the bytecode tree rooted at b for GCC background compilation.
 * Called in --jit-aot mode after JS_Eval(COMPILE_ONLY), before JS_EvalFunction.
 */
void js_jit_compile_all(JSContext *ctx, JSFunctionBytecode *b);

/*
 * js_jit_drain() — block until all enqueued GCC jobs have completed.
 * Called in --jit-aot mode to ensure all functions are compiled before
 * execution begins.
 */
void js_jit_drain(void);

#endif /* CONFIG_JIT */
#endif /* QUICKJS_JIT_H */
