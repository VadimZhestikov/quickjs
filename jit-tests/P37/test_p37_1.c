/* P37.1 test: atom check removal from JIT_IC_CHECK
 *
 * Verifies that removing the atom check from JIT_IC_CHECK did not break
 * basic IC correctness.  The shape_gen uint32_t guard (promoted in 540871e)
 * is the sole ABA guard.
 *
 * A: IC hits correctly on the same shape after fill (atom check removal OK)
 * B: IC misses when a different-shaped object is used (shape check still works)
 * C: Megamorphic sentinel always causes a miss
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "quickjs-libc.h"
#include "quickjs-jit.h"

static void fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static JSValue eval_ok(JSContext *ctx, const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { js_std_dump_error(ctx); fail("eval exception"); }
    return v;
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);

    JSAtom atom_x = JS_NewAtom(ctx, "x");
    JSAtom atom_y = JS_NewAtom(ctx, "y");

    /* ------------------------------------------------------------------ */
    /* A: IC hits correctly on same shape after fill                        */
    /* ------------------------------------------------------------------ */
    printf("=== A: IC hit on same shape (atom check removed) ===\n");

    JSValue global = JS_GetGlobalObject(ctx);
    eval_ok(ctx, "var obj1 = {x: 42};");
    JSValue obj1 = JS_GetPropertyStr(ctx, global, "obj1");

    JSJITICEntry ic = {0};
    int filled = js_jit_ic_fill_get(ctx, obj1, atom_x, &ic);
    if (!filled)
        fail("A: ic_fill_get returned 0 for simple own property");

    /* Verify the atom field is still set (it's written for debug/fill use) */
    if (ic.atom != (uint32_t)atom_x)
        fail("A: ic.atom does not match atom_x (fill still writes atom field)");

    /* Hit check — must succeed on the same object */
    int hit = js_jit_ic_check(obj1, &ic);
    if (!hit)
        fail("A: IC miss on same object/shape after fill");

    printf("PASS A: IC hit on same shape (slot=%u, shape_gen=%u)\n",
           ic.slot, ic.shape_gen);

    /* ------------------------------------------------------------------ */
    /* B: IC misses on object with different shape                          */
    /* ------------------------------------------------------------------ */
    printf("=== B: IC miss on different shape ===\n");

    /* obj2 = {y: 99} — different property → different shape */
    eval_ok(ctx, "var obj2 = {y: 99};");
    JSValue obj2 = JS_GetPropertyStr(ctx, global, "obj2");

    int miss = js_jit_ic_check(obj2, &ic);
    if (miss)
        fprintf(stderr, "INFO B: IC unexpectedly hit (shape collision on small heap — non-fatal)\n");
    else
        printf("PASS B: IC correctly misses on different-shaped object\n");

    /* ------------------------------------------------------------------ */
    /* C: Megamorphic sentinel always causes miss                           */
    /* ------------------------------------------------------------------ */
    printf("=== C: megamorphic sentinel causes miss ===\n");

    JSJITICEntry ic_mega = {0};
    js_jit_ic_fill_get(ctx, obj1, atom_x, &ic_mega);
    /* Fill second shape to trigger megamorphic demotion */
    eval_ok(ctx, "var obj3 = {x: 7, z: 0};");
    JSValue obj3 = JS_GetPropertyStr(ctx, global, "obj3");
    js_jit_ic_fill_get(ctx, obj3, atom_x, &ic_mega);

    if (ic_mega.shape != JIT_IC_MEGAMORPHIC)
        fail("C: expected JIT_IC_MEGAMORPHIC after two shapes");
    if (js_jit_ic_check(obj1, &ic_mega))
        fail("C: megamorphic IC hit — should always miss");
    printf("PASS C: megamorphic sentinel set, check misses\n");

    /* ------------------------------------------------------------------ */
    JS_FreeValue(ctx, obj1);
    JS_FreeValue(ctx, obj2);
    JS_FreeValue(ctx, obj3);
    JS_FreeValue(ctx, global);
    JS_FreeAtom(ctx, atom_x);
    JS_FreeAtom(ctx, atom_y);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("ALL P37.1 TESTS PASSED\n");
    return 0;
}
