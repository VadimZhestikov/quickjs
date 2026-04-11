/* P37.3 test: refcount correctness after property read loop
 *
 * Verifies that the peephole optimization (get_loc skips DupValue when
 * immediately followed by get_field) does not corrupt the object's refcount.
 *
 * A: f(o) { let s=0; for(i=0;i<10000;i++) s+=o.x; return s; } returns correct value
 * B: Object is still usable after the call (refcount not drifted)
 * C: Mixed-shape calls (IC miss path) also return correct results
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

    /* ------------------------------------------------------------------ */
    /* Setup: define f and objects                                          */
    /* ------------------------------------------------------------------ */
    /* Use a moderate iteration count so the JIT has time to compile.
     * JS_SetMaxStackSize is not needed — loops don't recurse. */
    eval_ok(ctx,
        "function f(o) {\n"
        "  let s = 0;\n"
        "  for (let i = 0; i < 10000; i++) s += o.x;\n"
        "  return s;\n"
        "}\n"
    );

    JSValue global = JS_GetGlobalObject(ctx);
    eval_ok(ctx, "var o1 = {x: 3};");
    eval_ok(ctx, "var o2 = {x: 7, y: 0};");  /* different shape */

    JSValue func_f = JS_GetPropertyStr(ctx, global, "f");
    JSValue o1 = JS_GetPropertyStr(ctx, global, "o1");
    JSValue o2 = JS_GetPropertyStr(ctx, global, "o2");

    /* ------------------------------------------------------------------ */
    /* A: correct result for monomorphic call                               */
    /* ------------------------------------------------------------------ */
    printf("=== A: correct result for property read loop ===\n");

    JSValue args_a[1] = { o1 };
    JSValue result_a = JS_Call(ctx, func_f, JS_UNDEFINED, 1, args_a);
    if (JS_IsException(result_a)) {
        js_std_dump_error(ctx);
        fail("A: exception calling f(o1)");
    }
    double val_a;
    JS_ToFloat64(ctx, &val_a, result_a);
    JS_FreeValue(ctx, result_a);
    if (val_a != 30000.0) {
        fprintf(stderr, "FAIL A: expected 30000, got %.0f\n", val_a);
        exit(1);
    }
    printf("PASS A: f(o1) = %.0f (correct)\n", val_a);

    /* ------------------------------------------------------------------ */
    /* B: object still usable after call (refcount integrity)               */
    /* ------------------------------------------------------------------ */
    printf("=== B: object usable after call (refcount check) ===\n");

    /* Read o1.x again — if refcount was corrupted the runtime may segfault
     * or return garbage.  This is a best-effort check. */
    JSValue check = JS_GetPropertyStr(ctx, o1, "x");
    if (JS_IsException(check))
        fail("B: exception reading o1.x after call — possible refcount corruption");
    int iv;
    JS_ToInt32(ctx, &iv, check);
    JS_FreeValue(ctx, check);
    if (iv != 3)
        fail("B: o1.x != 3 after call — possible refcount corruption");
    printf("PASS B: o1.x = %d after call (object intact)\n", iv);

    /* ------------------------------------------------------------------ */
    /* C: mixed-shape calls return correct results                          */
    /* ------------------------------------------------------------------ */
    printf("=== C: mixed-shape calls (IC miss path correctness) ===\n");

    /* First call primed the IC for o1's shape.  o2 has a different shape,
     * so the IC miss path is exercised. */
    JSValue args_c[1] = { o2 };
    JSValue result_c = JS_Call(ctx, func_f, JS_UNDEFINED, 1, args_c);
    if (JS_IsException(result_c)) {
        js_std_dump_error(ctx);
        fail("C: exception calling f(o2)");
    }
    double val_c;
    JS_ToFloat64(ctx, &val_c, result_c);
    JS_FreeValue(ctx, result_c);
    if (val_c != 70000.0) {
        fprintf(stderr, "FAIL C: expected 70000, got %.0f\n", val_c);
        exit(1);
    }
    printf("PASS C: f(o2) = %.0f (correct via IC miss path)\n", val_c);

    /* Call again with o1 — bimorphic IC hit */
    JSValue result_d = JS_Call(ctx, func_f, JS_UNDEFINED, 1, args_a);
    if (JS_IsException(result_d)) {
        js_std_dump_error(ctx);
        fail("C2: exception calling f(o1) second time");
    }
    double val_d;
    JS_ToFloat64(ctx, &val_d, result_d);
    JS_FreeValue(ctx, result_d);
    if (val_d != 30000.0) {
        fprintf(stderr, "FAIL C2: expected 30000 on second call, got %.0f\n", val_d);
        exit(1);
    }
    printf("PASS C2: f(o1) = %.0f on second call (bimorphic IC)\n", val_d);

    /* ------------------------------------------------------------------ */
    JS_FreeValue(ctx, o1);
    JS_FreeValue(ctx, o2);
    JS_FreeValue(ctx, func_f);
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("ALL P37.3 TESTS PASSED\n");
    return 0;
}
