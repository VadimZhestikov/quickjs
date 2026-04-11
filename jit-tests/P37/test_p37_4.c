/* P37.4 test: bimorphic IC
 *
 * Verifies the JSJITICEntry2 bimorphic IC:
 *   A: After shape A fill  → n=1, hits on shape A
 *   B: After shape B fill  → n=2, hits on both shape A and shape B
 *   C: After shape C fill  → n=3 (megamorphic), all checks miss
 *   D: Alternating A/B calls in a JS loop return correct results
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
    JSValue global = JS_GetGlobalObject(ctx);

    /* Create three objects with distinct shapes */
    eval_ok(ctx, "var oa = {x: 10};");           /* shape A: {x} */
    eval_ok(ctx, "var ob = {x: 20, y: 0};");     /* shape B: {x, y} */
    eval_ok(ctx, "var oc = {x: 30, y: 0, z: 0};"); /* shape C: {x, y, z} */

    JSValue oa = JS_GetPropertyStr(ctx, global, "oa");
    JSValue ob = JS_GetPropertyStr(ctx, global, "ob");
    JSValue oc = JS_GetPropertyStr(ctx, global, "oc");

    /* ------------------------------------------------------------------ */
    /* A: fill with shape A → n=1, mono hit                                */
    /* ------------------------------------------------------------------ */
    printf("=== A: mono fill (shape A) → n=1 ===\n");

    JSJITICEntry2 ic2 = {{}, 0};
    js_jit_ic2_fill_get(ctx, oa, atom_x, &ic2);

    if (ic2.n != 1)
        fail("A: expected n=1 after first fill");
    if (ic2.e[0].shape == NULL || ic2.e[0].shape == JIT_IC_MEGAMORPHIC)
        fail("A: e[0].shape invalid after first fill");

    /* Check hit on shape A */
    int hit_a = js_jit_ic_check(oa, &ic2.e[0]);
    if (!hit_a)
        fail("A: IC miss on shape A after mono fill");

    /* Shape B must miss on e[0] */
    int miss_b_on_e0 = js_jit_ic_check(ob, &ic2.e[0]);
    if (miss_b_on_e0)
        fail("A: IC hit on shape B using e[0] — unexpected");

    printf("PASS A: n=%d, e[0].slot=%u, shape A hits, shape B misses e[0]\n",
           ic2.n, ic2.e[0].slot);

    /* ------------------------------------------------------------------ */
    /* B: fill with shape B → n=2, bimorphic                               */
    /* ------------------------------------------------------------------ */
    printf("=== B: bi fill (shape B) → n=2 ===\n");

    js_jit_ic2_fill_get(ctx, ob, atom_x, &ic2);

    if (ic2.n != 2)
        fail("B: expected n=2 after second fill");
    if (ic2.e[1].shape == NULL || ic2.e[1].shape == JIT_IC_MEGAMORPHIC)
        fail("B: e[1].shape invalid after bimorphic fill");

    /* Both shapes must hit on their respective slots */
    int hit_a2 = js_jit_ic_check(oa, &ic2.e[0]);
    int hit_b2 = js_jit_ic_check(ob, &ic2.e[1]);
    if (!hit_a2)
        fail("B: shape A misses e[0] after bimorphic fill");
    if (!hit_b2)
        fail("B: shape B misses e[1] after bimorphic fill");

    printf("PASS B: n=%d, e[0].slot=%u e[1].slot=%u, both shapes hit\n",
           ic2.n, ic2.e[0].slot, ic2.e[1].slot);

    /* ------------------------------------------------------------------ */
    /* C: fill with shape C → n=3 (megamorphic)                            */
    /* ------------------------------------------------------------------ */
    printf("=== C: third shape → n=3 megamorphic ===\n");

    js_jit_ic2_fill_get(ctx, oc, atom_x, &ic2);

    if (ic2.n != 3)
        fail("C: expected n=3 (megamorphic) after third fill");
    if (ic2.e[0].shape != JIT_IC_MEGAMORPHIC)
        fail("C: e[0].shape is not JIT_IC_MEGAMORPHIC");

    /* All checks must miss in megamorphic state */
    if (js_jit_ic_check(oa, &ic2.e[0]))
        fail("C: IC hit on e[0] in megamorphic state (should always miss)");
    if (js_jit_ic_check(ob, &ic2.e[0]))
        fail("C: IC hit on e[0] for ob in megamorphic state");

    printf("PASS C: n=%d megamorphic, all checks miss\n", ic2.n);

    /* ------------------------------------------------------------------ */
    /* D: JS-level correctness with alternating shapes via JIT              */
    /* ------------------------------------------------------------------ */
    printf("=== D: JS alternating A/B shapes correctness ===\n");

    eval_ok(ctx,
        "function g(o) { return o.x; }\n"
    );

    JSValue func_g = JS_GetPropertyStr(ctx, global, "g");

    /* Warm up to trigger JIT compilation */
    for (int i = 0; i < 150; i++) {
        JSValue r = JS_Call(ctx, func_g, JS_UNDEFINED, 1, (i % 2 == 0) ? &oa : &ob);
        JS_FreeValue(ctx, r);
    }

    /* Now verify correctness with alternating shapes */
    int ok = 1;
    for (int i = 0; i < 20; i++) {
        JSValue obj = (i % 2 == 0) ? oa : ob;
        JSValue r = JS_Call(ctx, func_g, JS_UNDEFINED, 1, &obj);
        if (JS_IsException(r)) {
            js_std_dump_error(ctx);
            fail("D: exception in g()");
        }
        int rv;
        JS_ToInt32(ctx, &rv, r);
        JS_FreeValue(ctx, r);
        int expected = (i % 2 == 0) ? 10 : 20;
        if (rv != expected) {
            fprintf(stderr, "FAIL D: i=%d expected %d got %d\n", i, expected, rv);
            ok = 0;
        }
    }
    if (!ok) exit(1);
    printf("PASS D: 20 alternating A/B calls all returned correct values\n");

    /* ------------------------------------------------------------------ */
    JS_FreeValue(ctx, oa);
    JS_FreeValue(ctx, ob);
    JS_FreeValue(ctx, oc);
    JS_FreeValue(ctx, func_g);
    JS_FreeValue(ctx, global);
    JS_FreeAtom(ctx, atom_x);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("ALL P37.4 TESTS PASSED\n");
    return 0;
}
