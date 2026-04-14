/* P54 — Warm-IC recompile: 1-slot monomorphic IC for get_field
 *
 * Cold compile emits  static JSJITICEntry2 _icN={{},0}  (4 slots, 200 bytes)
 * for every OP_get_field site.  P54 changes the warm recompile for INT sites
 * to emit  static JSJITICEntry _icN={0}  (1 slot, 48 bytes) with a single
 * JIT_IC_CHECK_FAST check and js_jit_ic_fill_get in the miss path — no dead
 * n>=2/3/4 branches.
 *
 * Correctness test: after warming, the function must still return the right
 * values for the monomorphic case (same shape every call).
 *
 * Each test drives >= 220 JIT calls so the warm .so has installed.
 * __jit_drain() is called after 205 JIT calls to wait for warm compilation.
 */

'use strict';

let passed = 0;
let failed = 0;

function check(label, got, expected) {
    if (got === expected) {
        print('PASS: ' + label);
        passed++;
    } else {
        print('FAIL: ' + label + ' got=' + got + ' expected=' + expected);
        failed++;
    }
}

/* --- Test 1: monomorphic INT get_field warm path (basic) ----------------- */
(function test1_mono_int() {
    /* sumXY accesses .x and .y — two INT get_field sites.
     * All objects have the same shape, so both sites stay monomorphic. */
    function sumXY(o) { return o.x + o.y; }

    var obj = { x: 3, y: 7 };

    /* Trigger cold GCC, wait for it, then accumulate 205 JIT calls. */
    sumXY(obj);
    __jit_drain();
    var s = 0;
    for (var i = 0; i < 205; i++) s += sumXY(obj);
    __jit_drain();  /* install warm .so */

    /* A few calls on warm JIT: should return 10 each time. */
    check('test1 warm result', sumXY(obj), 10);
    check('test1 accumulation', s, 205 * 10);
})();

/* --- Test 2: monomorphic get_field on array element objects -------------- */
(function test2_array_elem() {
    /* sumCoords iterates an array of {x,y} objects.
     * All array elements have the same shape → both .x and .y sites are
     * monomorphic throughout. */
    function sumCoords(arr) {
        var s = 0;
        for (var i = 0; i < arr.length; i++) {
            s += arr[i].x + arr[i].y;
        }
        return s;
    }

    var data = [];
    for (var i = 0; i < 20; i++) data.push({ x: i, y: i + 1 });
    /* sum = Σ(i + i+1) for i=0..19 = 20*20 = 400 */
    var expected = 400;

    sumCoords(data);
    __jit_drain();
    var total = 0;
    for (var iter = 0; iter < 205; iter++) total += sumCoords(data);
    __jit_drain();

    check('test2 per-call result', sumCoords(data), expected);
    check('test2 accumulation', total, 205 * expected);
})();

/* --- Test 3: warm correctness with different INT values ------------------ */
(function test3_varying_ints() {
    /* getVal reads a single INT field from the same-shaped object.
     * The field value varies each call; the warm 1-slot IC must still
     * produce the correct runtime value (not a stale cached copy). */
    function getVal(o) { return o.v; }

    var obj = { v: 0 };

    obj.v = 1; getVal(obj);
    __jit_drain();
    for (var i = 0; i < 205; i++) { obj.v = i; getVal(obj); }
    __jit_drain();

    obj.v = 42;
    check('test3 warm varying int', getVal(obj), 42);
    obj.v = -7;
    check('test3 warm negative int', getVal(obj), -7);
})();

/* --- Test 4: VT update in miss path enables second warm ------------------- */
(function test4_vt_update() {
    /* First warm pass sees INT; the miss path in the warm .so still writes
     * __jit_vt_HASH[gf_idx] so a hypothetical second warm could re-use hints.
     * Here we just confirm the function keeps working correctly after
     * hitting the miss path (first miss for each warm static IC). */
    function readProp(o) { return o.p; }

    var a = { p: 100 };
    var b = { p: 200 };

    a.p = 1; readProp(a);
    __jit_drain();
    for (var i = 0; i < 205; i++) readProp(a);
    __jit_drain();

    /* Hit the warm miss path by passing an object with a different shape. */
    var c = { p: 99, q: 0 };  /* extra 'q' → different shape */
    check('test4 warm miss path', readProp(c), 99);
    check('test4 warm hit path', readProp(a), 1);
    check('test4 original value', readProp(b), 200);
})();

/* --- Test 5: nested objects (deeper INT chain) --------------------------- */
(function test5_nested() {
    /* inner reads two INT fields; outer reads one INT field from the
     * inner-function result.  Tests that the warm 1-slot IC is per-site
     * and doesn't interfere between functions. */
    function inner(o) { return o.a + o.b; }
    function outer(o) { return inner(o) * o.c; }

    var obj = { a: 2, b: 3, c: 4 };  /* (2+3)*4 = 20 */

    inner(obj); outer(obj);
    __jit_drain();
    for (var i = 0; i < 205; i++) outer(obj);
    __jit_drain();

    check('test5 nested result', outer(obj), 20);
    obj.a = 5; obj.b = 5; obj.c = 2;  /* (5+5)*2 = 20 */
    check('test5 updated values', outer(obj), 20);
})();

/* --- Summary ------------------------------------------------------------ */
print('');
print('P54 results: ' + passed + ' passed, ' + failed + ' failed');
if (failed > 0) throw new Error('P54 test failures: ' + failed);
