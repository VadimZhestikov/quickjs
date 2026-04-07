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
    /* P18: typeof_is_undefined / typeof_is_function — consume value, return int */
    int (*typeof_is_undefined)(JSContext *, JSValue a);
    int (*typeof_is_function)(JSContext *, JSValue a);

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
    /* Slow path for OP_put_var when value is JS_UNINITIALIZED (implicit global or TDZ).
     * is_put_init=1 if opcode is OP_put_var_init (allows writing to uninit lexical).
     * val is consumed (freed or stored) on success; on exception val is freed too. */
    int     (*put_var_slow)(JSContext *, JSAtom atom, int is_lexical, int is_put_init, JSValue val);
    JSValue (*get_array_el)(JSContext *, JSValue obj, JSValue idx);
    int     (*set_array_el)(JSContext *, JSValue obj, JSValue idx, JSValue val);

    /* ------------------------------------------------------------------ */
    /* Property deletion (P16)                                             */
    /* ------------------------------------------------------------------ */
    /* OP_delete_var: delete a global variable by atom.
     * Mirrors JS_DeleteGlobalVar: checks global_var_obj first (lexical vars
     * cannot be deleted), then deletes from global_obj.
     * Returns 1 (deleted), 0 (not deletable), or -1 (exception). */
    int (*delete_global_var)(JSContext *, JSAtom atom);

    /* ------------------------------------------------------------------ */
    /* Function calls                                                      */
    /* ------------------------------------------------------------------ */
    JSValue (*call)(JSContext *, JSValue func, JSValue this_val,
                    int argc, JSValue *argv);
    JSValue (*call_constructor)(JSContext *, JSValue ctor,
                                JSValue new_target,
                                int argc, JSValue *argv);
    /* OP_apply: f.apply(this, args_array) / f(...spread).
     * func, this_val and args_array are borrowed (not consumed).
     * magic: 0=apply, 1=new apply, 2=spread-call. */
    JSValue (*apply)(JSContext *, JSValue func, JSValue this_val,
                     JSValue args_array, int magic);
    /* OP_apply_eval: eval(...spread) or f(...spread) at a call site where
     * the callee might be eval.  func and args_array are borrowed.
     * scope_idx is the encoded scope index from the bytecode operand
     * (already adjusted by ARG_SCOPE_END by the caller). */
    JSValue (*apply_eval)(JSContext *, JSValue func, JSValue args_array,
                          int scope_idx);

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

    /* ------------------------------------------------------------------ */
    /* P19 — simple utility ops                                            */
    /* ------------------------------------------------------------------ */
    /* OP_get_var_undef slow path: returns JS_UNDEFINED (not exception)
     * when the global variable is not found.  is_lexical: if true, TDZ throw. */
    JSValue (*get_var_undef)(JSContext *, JSAtom atom, int is_lexical);
    /* OP_throw_error: throw by type+atom (type 0-4, see interpreter) */
    void    (*throw_error)(JSContext *, JSAtom atom, int type);
    /* OP_to_object: JS_ToObject — borrows val, returns new ref */
    JSValue (*to_object)(JSContext *, JSValue val);
    /* OP_to_propkey: JS_ToPropertyKey — borrows val, returns new ref */
    JSValue (*to_propkey)(JSContext *, JSValue val);
    /* OP_regexp: JS_NewRegexp — CONSUMES pattern and bc */
    JSValue (*regexp)(JSContext *, JSValue pattern, JSValue bc);
    /* OP_set_name_computed: JS_DefineObjectNameComputed — borrows both */
    int     (*set_name_computed)(JSContext *, JSValue func, JSValue name_src);
    /* OP_set_proto: JS_SetPrototypeInternal — borrows both */
    int     (*set_proto)(JSContext *, JSValue obj, JSValue proto);
    /* OP_set_home_object: js_method_set_home_object — borrows both */
    void    (*set_home_object)(JSContext *, JSValue func, JSValue home);
    /* OP_get_array_el2: JS_GetPropertyValue — borrows obj, CONSUMES prop */
    JSValue (*get_array_el2)(JSContext *, JSValue obj, JSValue prop);
    /* OP_define_array_el: JS_DefinePropertyValueValue — borrows arr,
     * CONSUMES prop and val */
    int     (*define_array_el)(JSContext *, JSValue arr, JSValue prop, JSValue val);
    /* OP_push_bigint_i32: __JS_NewShortBigInt — always succeeds */
    JSValue (*push_bigint_i32)(JSContext *, int32_t v);
    /* OP_close_loc: detach one captured local's JSVarRef from its cap_buf slot */
    void    (*close_loc)(JSContext *, JSVarRef *vref);

    /* ------------------------------------------------------------------ */
    /* P20 — reference-slot ops                                            */
    /* ------------------------------------------------------------------ */
    /* Create a ref-pair JSObject using an existing JSVarRef.
     * Increments vref->ref_count; fills *pobj and *patom.
     * Used for OP_make_loc_ref / OP_make_arg_ref / OP_make_var_ref_ref. */
    int     (*make_ref_pair)(JSContext *, JSVarRef *vref, JSAtom atom,
                             JSValue *pobj, JSValue *patom);
    /* OP_make_var_ref: creates a ref-pair for a global variable via
     * JS_GetGlobalVarRef; fills *pobj and *patom. */
    int     (*make_var_ref)(JSContext *, JSAtom atom,
                            JSValue *pobj, JSValue *patom);
    /* OP_get_ref_value: read value from ref-pair (borrows obj and atom_val) */
    JSValue (*get_ref_value)(JSContext *, JSValue obj, JSValue atom_val);
    /* OP_put_ref_value: write value via ref-pair (CONSUMES obj, atom_val, val) */
    int     (*put_ref_value)(JSContext *, JSValue obj, JSValue atom_val, JSValue val);
    /* P21: spread / rest / copy */
    JSValue (*rest)(JSContext *, int first, int argc, JSValue *argv);
    int     (*append)(JSContext *, JSValue *parray, JSValue *ppos, JSValue enumobj);
    int     (*copy_data_properties)(JSContext *, JSValue target, JSValue source,
                                    JSValue excluded);
    /* P22: private fields */
    JSValue (*private_symbol)(JSContext *, JSAtom atom);
    JSValue (*get_private_field)(JSContext *, JSValue obj, JSValue prop);
    int     (*put_private_field)(JSContext *, JSValue obj, JSValue prop, JSValue val);
    int     (*define_private_field)(JSContext *, JSValue obj, JSValue prop, JSValue val);
    JSValue (*private_in)(JSContext *, JSValue obj, JSValue prop);
    /* P23: OOP / class helpers (check_ctor/init_ctor/define_class deferred) */
    int     (*check_brand)(JSContext *, JSValue obj, JSValue func);
    int     (*add_brand)(JSContext *, JSValue obj, JSValue home_obj);
    JSValue (*get_super_value)(JSContext *, JSValue this_val, JSValue obj, JSValue prop);
    int     (*put_super_value)(JSContext *, JSValue this_val, JSValue obj,
                               JSValue prop, JSValue val);
    int     (*define_method)(JSContext *, JSValue obj, JSValue func,
                             JSAtom atom, int op_flags);
    int     (*define_method_computed)(JSContext *, JSValue obj, JSValue key,
                                      JSValue func, int op_flags);
    /* P26: constructor / class-definition (via ctx->rt->current_stack_frame) */
    int     (*check_ctor)(JSContext *);
    JSValue (*init_ctor)(JSContext *, int argc, JSValue *argv);
    int     (*define_class)(JSContext *, JSValue *pparent, JSValue *pbfunc,
                            JSAtom atom, int class_flags, JSVarRef **var_refs);
    int     (*define_class_computed)(JSContext *, JSValue *pkey,
                                     JSValue *pparent, JSValue *pbfunc,
                                     JSAtom atom, int class_flags,
                                     JSVarRef **var_refs);
    /* P27: dynamic import */
    JSValue (*import_op)(JSContext *, JSValue specifier, JSValue options);
    /* P30: async for-of */
    /* for_await_of_start: like for_of_start but uses Symbol.asyncIterator.
     * obj ownership transferred; sets *piter and *pnext. */
    int (*for_await_of_start)(JSContext *, JSValue *piter, JSValue *pnext, JSValue obj);
    /* for_await_of_next: advance async for-of. Clears *pcatch_ph, calls next(iter),
     * puts raw Promise in *ppromise. iter and next are borrowed (not consumed). */
    int (*for_await_of_next)(JSContext *, JSValue iter, JSValue next,
                             JSValue *pcatch_ph, JSValue *ppromise);
    /* P30: with_* — object-environment-record scope lookup helpers */
    /* with_has: JS_HasProperty + optional @@unscopables check.
     * obj borrowed. Returns 1 (found+in-scope), 0 (not found or unscopable), -1 (error). */
    int     (*with_has)(JSContext *, JSValue obj, JSAtom atom, int is_with);
    /* with_get_var found path: get property; *pobj_val in=obj out=val (freed & replaced). */
    int     (*with_get_var)(JSContext *, JSValue *pobj_val, JSAtom atom);
    /* with_put_var found path: set property. obj borrowed, val consumed. */
    int     (*with_put_var)(JSContext *, JSValue obj, JSAtom atom, JSValue val);
    /* with_delete_var found path: delete property. obj borrowed.
     * Returns 1 (deleted), 0 (non-deletable), -1 (exception). */
    int     (*with_delete_var)(JSContext *, JSValue obj, JSAtom atom);
    /* with_make_ref found path: return atom as JSValue (caller owns). */
    JSValue (*with_make_ref)(JSContext *, JSAtom atom);
    /* with_get_ref found path: get property for method call ref. obj borrowed. */
    JSValue (*with_get_ref)(JSContext *, JSValue obj, JSAtom atom);
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
void      js_jit_fb_set_bc_hash(JSFunctionBytecode *b, uint64_t hash);
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
/* P9.0: per-PC stack depth table; value 0xffff = unreachable */
const uint16_t *js_jit_fb_get_stack_depth_tab(JSFunctionBytecode *b);
/* P9.3: control-flow annotation kinds */
typedef enum {
    JIT_CF_WHILE_LOOP   = 0,  /* while() {} back-edge — safe to restructure */
    JIT_CF_DOWHILE_LOOP = 1,  /* do {} while() back-edge — safe to restructure */
    JIT_CF_FOR_LOOP     = 2,  /* for(;;) back-edge — NOT restructured */
    JIT_CF_FORIN_LOOP   = 3,  /* for-in/of back-edge — NOT restructured */
    JIT_CF_IF           = 4,  /* future: if/else */
} JSJITCFKind;
typedef struct {
    uint32_t header_pc;  /* loop header bytecode offset */
    uint32_t exit_pc;    /* first bytecode offset after the loop */
    uint8_t  kind;       /* JSJITCFKind */
} JSJITCFAnnotation;
/* P9.3: CF annotation table accessor */
const JSJITCFAnnotation *js_jit_fb_cf_annotations(JSFunctionBytecode *b, int *count_out);
/* P9.1: atom → C string helper (wraps JS_AtomGetStrRT) */
const char *js_jit_atom_get_str(JSRuntime *rt, char *buf, int buf_size, JSAtom atom);
/* P9.1: JS identifier atoms for locals and arguments */
JSAtom         js_jit_fb_get_local_atom(JSFunctionBytecode *b, int local_idx);
JSAtom         js_jit_fb_get_arg_atom  (JSFunctionBytecode *b, int arg_idx);
/* Opcode size table: opcode_size[opcode] = instruction length in bytes */
const uint8_t *js_jit_get_opcode_size_table(int *count);
/* Function name as a C string (static buf — for debug/logging only) */
const char    *js_jit_fb_get_func_name(JSRuntime *rt, JSFunctionBytecode *b);
const char    *js_jit_fb_get_source(JSFunctionBytecode *b, int *len_out);
/* P8.2: function's own name atom (JS_ATOM_NULL if anonymous) */
JSAtom         js_jit_fb_get_func_atom(JSFunctionBytecode *b);
/* P13: inner function's cpool entry as JSFunctionBytecode* (NULL if not function) */
JSFunctionBytecode *js_jit_cpool_get_fb(JSFunctionBytecode *b, int cpool_idx);
/* P13: JSClosureTypeEnum values — must match quickjs.c (verified by _Static_assert). */
#define JIT_CLOSURE_LOCAL         0  /* var_idx = local index in outer function */
#define JIT_CLOSURE_ARG           1  /* var_idx = arg index in outer function */
#define JIT_CLOSURE_REF           2  /* var_idx = closure-var index (pass-through ref) */
#define JIT_CLOSURE_GLOBAL_REF    3  /* var_idx = closure-var index (global pass-through) */
#define JIT_CLOSURE_GLOBAL_DECL   4  /* eval only — unsupported in JIT */
#define JIT_CLOSURE_GLOBAL        5  /* eval only — unsupported in JIT */
#define JIT_CLOSURE_MODULE_DECL   6  /* module only — unsupported in JIT */
#define JIT_CLOSURE_MODULE_IMPORT 7  /* module only — unsupported in JIT */
/* P13: inner function's closure_var entry accessors */
int js_jit_fb_get_inner_cv_type(JSFunctionBytecode *b_inner, int cv_idx);
int js_jit_fb_get_inner_cv_var_idx(JSFunctionBytecode *b_inner, int cv_idx);
/* P13: outer function's var_ref_count (size of sf->var_refs[]) */
int js_jit_fb_get_var_ref_count(JSFunctionBytecode *b);
/* P13: outer function's per-local/arg captured-variable metadata */
int js_jit_fb_get_local_var_ref_idx(JSFunctionBytecode *b, int local_idx);
int js_jit_fb_get_arg_var_ref_idx(JSFunctionBytecode *b, int arg_idx);
int js_jit_fb_is_local_captured(JSFunctionBytecode *b, int local_idx);
int js_jit_fb_is_arg_captured(JSFunctionBytecode *b, int arg_idx);
/* P13: runtime helpers for JIT-managed closures */
JSVarRef  *js_jit_make_var_ref(JSContext *ctx, JSValue *slot);
void       js_jit_close_caps(JSContext *ctx, JSVarRef **vrefs, int n);
JSValue    js_jit_create_closure(JSContext *ctx, JSValue bfunc,
                                  JSVarRef **pre_vrefs, int n_vrefs);
/* P8.2: interrupt poll — wraps js_poll_interrupts (static inline) for vtable use */
int            js_jit_poll_interrupts(JSContext *ctx);
/* P8.3: JIT-to-JIT fast call — checks jit_func, calls directly if compiled */
JSValue        js_jit_call(JSContext *, JSValue func, JSValue this_val,
                            int argc, JSValue *argv);
/* P15: iterator helpers */
JSValue js_jit_special_object(JSContext *ctx, int kind, int argc, JSValue *argv);
int js_jit_for_in_start(JSContext *ctx, JSValue *pobj);
int js_jit_for_in_next(JSContext *ctx, JSValue iter,
                       JSValue *pkey, JSValue *pdone);
int js_jit_for_of_start(JSContext *ctx,
                        JSValue *piter, JSValue *pnext, JSValue obj);
int js_jit_for_of_next(JSContext *ctx, JSValue *piter, JSValue next,
                       JSValue *pvalue, JSValue *pdone);
int js_jit_iterator_close(JSContext *ctx, JSValue iter, JSValue next);
/* P30: async for-of helpers */
int js_jit_for_await_of_start(JSContext *ctx,
                               JSValue *piter, JSValue *pnext, JSValue obj);
int js_jit_for_await_of_next(JSContext *ctx, JSValue iter, JSValue next,
                              JSValue *pcatch_ph, JSValue *ppromise);
/* P30: with_* object-environment helpers */
int     js_jit_with_has(JSContext *ctx, JSValue obj, JSAtom atom, int is_with);
int     js_jit_with_get_var(JSContext *ctx, JSValue *pobj_val, JSAtom atom);
int     js_jit_with_put_var(JSContext *ctx, JSValue obj, JSAtom atom, JSValue val);
int     js_jit_with_delete_var(JSContext *ctx, JSValue obj, JSAtom atom);
JSValue js_jit_with_make_ref(JSContext *ctx, JSAtom atom);
JSValue js_jit_with_get_ref(JSContext *ctx, JSValue obj, JSAtom atom);
int js_jit_iterator_get_value_done(JSContext *ctx, JSValue obj,
                                   JSValue *pvalue, JSValue *pdone);
int js_jit_iterator_next_step(JSContext *ctx,
                              JSValue iter, JSValue next,
                              JSValue val, JSValue *presult);
int js_jit_iterator_call(JSContext *ctx,
                         JSValue iter, JSValue val, int flags,
                         JSValue *presult, int *pret_flag);

/* P12: generator frame helpers */

/*
 * JSJITGeneratorFrame — JIT-side save area for a suspended generator.
 *
 * Allocated once on the first call to a JIT-compiled generator function and
 * freed by js_jit_gen_frame_free() from async_func_free_frame().
 *
 * resume_idx:
 *   -1  = never yielded (initial call, run function from the top)
 *    0  = suspended after OP_initial_yield
 *    N  = suspended after the N-th OP_yield (N >= 1)
 *
 * saved_lv[0..n_lv-1]:
 *   Spilled JSValues for each local variable slot (var_count).
 *   JS_UNDEFINED means the slot held no live reference at yield time.
 */
typedef struct JSJITGeneratorFrame {
    int      resume_idx;   /* -1=first call; 0=after initial_yield; N=after N-th yield */
    int      n_lv;         /* var_count of the generator function */
    JSValue *saved_lv;     /* heap array[n_lv], JS_UNDEFINED for empty slot */
    /* P12.1: catch state saved at every yield/await so try/catch works across suspensions */
    int      catch_depth;  /* number of active catch frames at last yield */
    int      catch_sp[32]; /* stack depth for each catch frame */
    int      catch_h[32];  /* handler PC for each catch frame */
    /* P12.2: closure var-ref save area — frame owns one ref per non-NULL entry */
    int        n_vrefs;        /* size of saved_vrefs[] (= var_ref_count of function) */
    JSVarRef **saved_vrefs;    /* heap array[n_vrefs] of JSVarRef*, NULL = not live */
} JSJITGeneratorFrame;

JSJITGeneratorFrame *js_jit_gen_init_frame(JSContext *ctx, int n_lv, int n_vrefs);
int     js_jit_gen_get_throw(JSContext *ctx);
void    js_jit_gen_yield_setup(JSContext *ctx, JSValue yield_val,
                               int resume_idx, JSJITGeneratorFrame *gf);
JSValue js_jit_gen_get_next_val(JSContext *ctx);
int     js_jit_gen_get_magic_int(JSContext *ctx);
/* P12.2: save/restore _sf_vrefs[] across yield/await */
void    js_jit_gen_save_vrefs(JSContext *ctx, JSVarRef **vrefs, int n,
                               JSJITGeneratorFrame *gf);
void    js_jit_gen_restore_vrefs(JSVarRef **vrefs, int n, JSJITGeneratorFrame *gf);
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
    uint32_t  atom;   /* JSAtom at slot — ABA guard: if shape is freed and
                       * reallocated for a different layout, the atom at this
                       * slot will differ, preventing false IC hits. */
    uint8_t   kind;   /* 0=general, 1=float64 typed slot (P8.6) */
    uint8_t   _pad[3];
} JSJITICEntry;

/*
 * JIT_IC_MEGAMORPHIC: sentinel stored in JSJITICEntry.shape when a callsite
 * has seen more than one shape.  Always causes a miss.
 */
#define JIT_IC_MEGAMORPHIC ((void *)(uintptr_t)1)

/*
 * Struct byte offsets for the inline IC check (JIT_IC_CHECK macro below)
 * and the inline property slot read (P11.1).
 * Verified by _Static_assert in quickjs.c.  Do NOT change without updating
 * both sets together.
 *   JSObject.shape        = byte 32
 *   JSObject.prop         = byte 40  (pointer to JSProperty array)
 *   JSShape.prop_count    = byte 40
 *   JSShape.prop[]        = byte 64  (flexible array of JSShapeProperty)
 *   JSShapeProperty.atom  = byte  4  (after 4-byte bitfield word)
 *   sizeof(JSShapeProperty) = 8
 *   sizeof(JSProperty)    = 16       (= sizeof(JSValue); u.value is at offset 0)
 */
#define JIT_OBJIC_SHAPE_OFF       32
#define JIT_OBJ_PROP_OFF          40  /* JSObject.prop pointer */
#define JIT_PROP_SIZE             16  /* sizeof(JSProperty) == sizeof(JSValue) */
#define JIT_SHAPEIC_PROPCOUNT_OFF 40
#define JIT_SHAPEIC_PROP_OFF      64
#define JIT_SHAPEIC_PROPSIZE       8
#define JIT_SHAPEIC_ATOM_OFF       4

/*
 * P11.4: JSObject layout constants for inline array element fast path.
 *   JSObject.class_id         = byte  6  (uint16_t inside bitfield word)
 *   JS_CLASS_ARRAY            = 2        (enum value for dense arrays)
 *   JSObject.u.array.u.values = byte 56  (JSValue* element array)
 *   JSObject.u.array.count    = byte 64  (int element count)
 * Verified by _Static_assert in quickjs.c.
 */
#define JIT_OBJ_CLASSID_OFF   6   /* JSObject.class_id (uint16_t) */
#define JIT_CLASS_ARRAY       2   /* JS_CLASS_ARRAY enum value */
#define JIT_ARR_VALUES_OFF   56   /* JSObject.u.array.u.values (JSValue*) */
#define JIT_ARR_COUNT_OFF    64   /* JSObject.u.array.count (int) */

/*
 * P11.3: JSObject layout constants for call IC.
 *   JS_CLASS_BYTECODE_FUNCTION        = 13       (enum value for JS bytecode functions)
 *   JSObject.u.func.function_bytecode = byte 48  (JSFunctionBytecode*)
 *   JSObject.u.func.var_refs          = byte 56  (JSVarRef**)
 *   JSFunctionBytecode.jit_func       = byte 112 (JSJITFunc)
 *   JSFunctionBytecode.jit_bc_hash    = byte 128 (uint64_t, stable per-bytecode identity)
 * Verified by _Static_assert in quickjs.c.
 */
#define JIT_FUNC_BC_OFF            48  /* JSObject.u.func.function_bytecode */
#define JIT_FUNC_VARREFS_OFF       56  /* JSObject.u.func.var_refs */
#define JIT_CLASS_BYTECODE_FUNCTION 13 /* JS_CLASS_BYTECODE_FUNCTION enum value */
#define JIT_BC_JIT_FUNC_OFF       112  /* JSFunctionBytecode.jit_func (JSJITFunc) */
#define JIT_BC_BCHASH_OFF         128  /* JSFunctionBytecode.jit_bc_hash (uint64_t) */

/*
 * JIT_IC_CHECK(obj, ic): inline shape-guard + ABA-atom-guard.
 * Equivalent to js_jit_ic_check() but expands inline in JIT-generated code
 * so the compiler can see the body and optimize across the IC boundary.
 *
 * Safety: obj and ic must be simple lvalues (evaluated at most twice).
 */
#define JIT_IC_CHECK(obj, ic) \
    ((ic)->shape != NULL && \
     (ic)->shape != JIT_IC_MEGAMORPHIC && \
     JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT && \
     *(void **)((char*)JS_VALUE_GET_PTR(obj) + JIT_OBJIC_SHAPE_OFF) == (ic)->shape && \
     (uint32_t)*(const int *)((const char*)(ic)->shape + JIT_SHAPEIC_PROPCOUNT_OFF) > (ic)->slot && \
     *(const uint32_t*)((const char*)(ic)->shape + JIT_SHAPEIC_PROP_OFF + \
                        (ic)->slot * JIT_SHAPEIC_PROPSIZE + JIT_SHAPEIC_ATOM_OFF) == (ic)->atom)

/*
 * js_jit_ic_check: same logic as JIT_IC_CHECK but as a callable function.
 * Used by code that cannot inline the macro (e.g. debug helpers).
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

/* P8.5: Dense array element fast paths.
 * Both functions require JS_VALUE_GET_TAG(obj)==JS_TAG_OBJECT (caller-checked).
 * js_jit_array_get: on hit returns 1 with *out set to a new ref; 0 on miss.
 * js_jit_array_set: on hit returns 1 (val consumed); 0 on miss (val intact).
 * (Retained for AOT-cached .so files that call them; inlined at new call sites.)
 */
int js_jit_array_get(JSContext *ctx, JSValue obj, uint32_t idx, JSValue *out);
int js_jit_array_set(JSContext *ctx, JSValue obj, uint32_t idx, JSValue val);

/*
 * P11.3 — Monomorphic call IC.
 *
 * Per-call-site cache for function-call opcodes.  When a call site always
 * invokes the same bytecode function (monomorphic), this IC lets the JIT:
 *   1. Skip the class_id check, jit_func atomic load, and JS_Call vtable on hit.
 *   2. Call the callee's JIT function directly (if compiled) with pre-fetched
 *      cpool and var_refs — eliminating the identity-check inside js_jit_call.
 *
 * ABA safety: expected_bc is the bytecode pointer of the cached callee.  If
 * the function object is freed and another one allocated at the same address,
 * its function_bytecode pointer will differ → IC miss (no wrong-callee call).
 *
 * Ownership: the IC holds raw (non-refcounted) pointers.  callee_cpool and
 * callee_var_refs are stable for the lifetime of the bytecode / closure.
 * If var_refs can be reallocated (rare), the ABA guard on expected_bc catches it.
 */
typedef struct {
    void                *expected_func;      /* JSObject* — NULL=cold, JIT_IC_MEGAMORPHIC=poly */
    JSFunctionBytecode  *expected_bc;        /* ABA guard: func->u.func.function_bytecode */
    JSJITFunc            direct_jit;         /* non-NULL = callee is JIT-compiled */
    JSValue             *callee_cpool;       /* callee's constant pool */
    JSVarRef           **callee_var_refs;    /* callee's closure var refs */
    int                  callee_arg_count;   /* b->arg_count (for arg padding) */
    uint64_t             callee_bc_hash;     /* P11.3 double-ABA guard: b->jit_bc_hash */
} JSJITCallICEntry;

/*
 * js_jit_callIC_fill: populate ic after a call IC miss.
 * Caches expected_func/bc, extracts direct_jit/cpool/var_refs/arg_count.
 * Goes megamorphic if a second distinct function is seen at this site.
 * Also re-checks direct_jit when ic hits the same function again but
 * jit_func was not yet compiled at the time of the previous fill.
 */
void js_jit_callIC_fill(JSContext *ctx, JSValue func, JSJITCallICEntry *ic);
JSValue js_jit_ic_direct_call(JSContext *ctx, JSValue this_val,
                               int nargs, JSValue *argv,
                               JSJITCallICEntry *ic, JSVarRef **var_refs);
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
int     js_jit_op_put_var_slow(JSContext *, JSAtom atom, int is_lexical, int is_put_init, JSValue val);
int     js_jit_op_delete_global_var(JSContext *, JSAtom atom);
JSValue js_jit_op_apply(JSContext *, JSValue func, JSValue this_val, JSValue args_array, int magic);
JSValue js_jit_op_apply_eval(JSContext *, JSValue func, JSValue args_array, int scope_idx);
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
int js_jit_op_typeof_is_undefined(JSContext *, JSValue);
int js_jit_op_typeof_is_function(JSContext *, JSValue);
/* P19: utility op helpers */
JSValue js_jit_op_get_var_undef(JSContext *, JSAtom, int);
void    js_jit_op_throw_error(JSContext *, JSAtom, int);
JSValue js_jit_op_to_object(JSContext *, JSValue);
JSValue js_jit_op_to_propkey(JSContext *, JSValue);
JSValue js_jit_op_regexp(JSContext *, JSValue, JSValue);
int     js_jit_op_set_name_computed(JSContext *, JSValue, JSValue);
int     js_jit_op_set_proto(JSContext *, JSValue, JSValue);
void    js_jit_op_set_home_object(JSContext *, JSValue, JSValue);
JSValue js_jit_op_get_array_el2(JSContext *, JSValue, JSValue);
int     js_jit_op_define_array_el(JSContext *, JSValue, JSValue, JSValue);
JSValue js_jit_op_push_bigint_i32(JSContext *, int32_t);
void    js_jit_op_close_loc(JSContext *, JSVarRef *);
/* P20: ref-slot op helpers */
int     js_jit_op_make_ref_pair(JSContext *, JSVarRef *, JSAtom, JSValue *, JSValue *);
int     js_jit_op_make_var_ref(JSContext *, JSAtom, JSValue *, JSValue *);
JSValue js_jit_op_get_ref_value(JSContext *, JSValue, JSValue);
int     js_jit_op_put_ref_value(JSContext *, JSValue, JSValue, JSValue);
/* P21: spread / rest / copy helpers */
JSValue js_jit_op_rest(JSContext *, int first, int argc, JSValue *argv);
int     js_jit_op_append(JSContext *, JSValue *, JSValue *, JSValue);
int     js_jit_op_copy_data_properties(JSContext *, JSValue, JSValue, JSValue);
/* P22: private field helpers */
JSValue js_jit_op_private_symbol(JSContext *, JSAtom);
JSValue js_jit_op_get_private_field(JSContext *, JSValue, JSValue);
int     js_jit_op_put_private_field(JSContext *, JSValue, JSValue, JSValue);
int     js_jit_op_define_private_field(JSContext *, JSValue, JSValue, JSValue);
JSValue js_jit_op_private_in(JSContext *, JSValue, JSValue);
/* P23: OOP helpers */
int     js_jit_op_check_brand(JSContext *, JSValue, JSValue);
int     js_jit_op_add_brand(JSContext *, JSValue, JSValue);
JSValue js_jit_op_get_super_value(JSContext *, JSValue, JSValue, JSValue);
int     js_jit_op_put_super_value(JSContext *, JSValue, JSValue, JSValue, JSValue);
int     js_jit_op_define_method(JSContext *, JSValue, JSValue, JSAtom, int);
int     js_jit_op_define_method_computed(JSContext *, JSValue, JSValue, JSValue, int);
/* P26: constructor / class-definition helpers */
int     js_jit_op_check_ctor(JSContext *);
JSValue js_jit_op_init_ctor(JSContext *, int argc, JSValue *argv);
int     js_jit_op_define_class(JSContext *, JSValue *pparent, JSValue *pbfunc,
                               JSAtom atom, int class_flags, JSVarRef **var_refs);
int     js_jit_op_define_class_computed(JSContext *, JSValue *pkey,
                                        JSValue *pparent, JSValue *pbfunc,
                                        JSAtom atom, int class_flags,
                                        JSVarRef **var_refs);
/* P27: dynamic import */
JSValue js_jit_op_import(JSContext *, JSValue specifier, JSValue options);
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
 * var_refs — the function's own closure variable refs (from p->u.func.var_refs
 * at the call site).  Used by P10.3 to detect known JIT callees at codegen
 * time.  Pass NULL in AOT pre-pass (no live var_refs available).
 *
 * Called from JS_CallInternal when b->jit_call_count reaches JIT_THRESHOLD_GCC.
 */
void js_jit_queue_gcc(JSContext *ctx, JSFunctionBytecode *b, JSVarRef **var_refs);

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

/*
 * js_jit_set_aot_mode() / js_jit_get_aot_mode() — global flag that tells
 * dynamic script loaders (e.g. js_loadScript / load()) to pre-compile all
 * functions before executing the loaded script.  Set by qjs.c when
 * --jit-aot or --jit-warmup is active.
 */
void js_jit_set_aot_mode(int active);
int  js_jit_get_aot_mode(void);

/*
 * js_jit_set_threshold() / js_jit_get_threshold() — runtime override for the
 * JIT_THRESHOLD_GCC compile-time constant.  Set by --jit-threshold-gcc=N.
 * N=0: compile all static functions before first execution (AOT pre-pass).
 * N>=1: compile after N calls (default JIT_THRESHOLD_GCC, typically 100).
 */
void js_jit_set_threshold(int n);
int  js_jit_get_threshold(void);

/*
 * js_jit_set_dump_c_mode() — when active, print generated C to stdout
 * for each JIT-compiled function.  Set by --jit-dump-c.
 */
void js_jit_set_dump_c_mode(int active);

/*
 * js_jit_set_save_sources() — when active, write the original JS source of
 * each JIT-compiled function to <cache>/<hash>.js alongside the .c and .so.
 * Set by --jit-save-sources.  No-op if debug info is stripped (-s flag).
 */
void js_jit_set_save_sources(int active);

/*
 * js_jit_cache_has_c_src() — returns 1 if a .c source file exists in the
 * cache alongside the compiled .so for this function's bytecode hash.
 * Used by P10.2 --jit-link to discover which functions can be LTO-combined.
 */
int js_jit_cache_has_c_src(JSFunctionBytecode *b);

/*
 * js_jit_set_link_mode() — enable hash recording for --jit-link.
 * When active, every bc_hash seen in js_jit_queue_gcc() is appended to an
 * internal list so js_jit_link() can collect the corresponding .c files.
 * Must be called before execution begins.
 */
void js_jit_set_link_mode(int active);

/*
 * js_jit_link() — combine all per-function .c cache files into a single
 * GCC LTO compilation unit: `gcc -O2 -flto -shared -fPIC <files> -o combined.so`.
 * Writes to <cache_dir>/combined.so.  Also emits a manifest section so the
 * combined.so exports __jit_manifest[] and __jit_manifest_count.
 * Returns the number of functions combined, 0 if nothing to link, -1 on error.
 * Requires --jit-warmup to have been run first to populate the .c cache files.
 */
int js_jit_link(void);

/*
 * JSJITManifestEntry — one entry in the combined.so manifest.
 * The combined.so exports __jit_manifest[] (array) and __jit_manifest_count (int).
 * js_jit_install_combined() reads these to patch jit_func in every bytecode.
 */
typedef struct {
    uint64_t  bc_hash;   /* FNV-1a hash of function bytecode + build stamp */
    JSJITFunc func_ptr;  /* address of the compiled JIT function           */
} JSJITManifestEntry;

/*
 * js_jit_preload_combined() — open combined.so and cache its manifest without
 * installing anything yet.  Call BEFORE js_jit_compile_all() so that
 * js_jit_queue_gcc() can skip loading individual .so files for functions
 * already present in the manifest (fast-path in js_jit_queue_gcc).
 * Returns manifest entry count (≥0) or -1 on error.  Idempotent.
 */
int js_jit_preload_combined(void);

/*
 * js_jit_install_combined_if_exists() — if combined.so is present in the cache,
 * dlopen it (idempotent with js_jit_preload_combined), read the manifest, and
 * atomically update jit_func for every function whose bc_hash is known so far.
 * May be called multiple times as new scripts are loaded; each call patches
 * any newly discovered bytecodes that match the manifest.
 * Individual per-function .so handles are closed; the combined.so handle is
 * kept alive until js_jit_free().
 * Returns number of functions installed this call, 0 if none, -1 on error.
 */
int js_jit_install_combined_if_exists(void);

/*
 * js_jit_ordinary_instanceof() — JIT entrypoint for OP_instanceof.
 * Routes through JS_IsInstanceOf (handles Symbol.hasInstance), matching
 * interpreter behaviour.  The name is retained for combined.so compat:
 * the manifest.c shim provides a weak JS_OrdinaryIsInstanceOf alias that
 * calls this function so old cached .c files still link.
 */
int js_jit_ordinary_instanceof(JSContext *ctx, JSValue val, JSValue obj);
/* Direct export of JS_OrdinaryIsInstanceOf (made non-static for JIT use) */
int JS_OrdinaryIsInstanceOf(JSContext *ctx, JSValueConst val, JSValueConst obj);

/*
 * js_jit_get_callee_fb() — extract JSFunctionBytecode* from a JSValue.
 * P10.3: used at codegen time to check whether a closure variable holds a
 * bytecode function (so its bc_hash can be recorded for direct call emit).
 * Returns NULL if the value is not a JS_CLASS_BYTECODE_FUNCTION object.
 */
JSFunctionBytecode *js_jit_get_callee_fb(JSValue func);

/*
 * js_jit_check_and_extract() — guard check for P10.3 generated direct calls.
 * At runtime, verifies that 'func' is exactly the expected JIT function
 * (pointer identity on the jit_func field), then extracts cpool and var_refs.
 * Returns 1 if the guard passes and fills *cpool_out / *var_refs_out.
 * Returns 0 if the guard fails (callee must fall back to _RT->call).
 */
int js_jit_check_and_extract(JSValue func, JSJITFunc expected,
                              JSValue **cpool_out, JSVarRef ***var_refs_out);

#endif /* CONFIG_JIT */
#endif /* QUICKJS_JIT_H */
