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
#include "libtcc.h"

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

void js_jit_compile_tcc(JSContext *ctx, JSFunctionBytecode *b)
{
    /* Phase 3: bytecode → C → TCC → native function pointer.
     * For now, permanently mark as non-compilable so the counter never
     * fires again — prevents wasted increments until Phase 3 is wired. */
    (void)ctx;
    js_jit_fb_set_no_compile(b);
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
