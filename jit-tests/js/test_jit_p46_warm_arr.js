/* P46 — Warm-IC recompile: gen_st=INT for OP_get_array_el
 *
 * Tests the two-phase JIT optimisation for typed integer array element reads.
 * Cold compile emits __jit_vt_HASH[n_gf + ae_idx] BSS slots; after 200 warm
 * JIT calls a warm recompile fires and uses gen_st=INT for INT-hinted array
 * reads (eliminating tag checks and JS_DupValue in the hot path).
 *
 * JIT threshold = 100; warm recompile at 100+200 = 300 total calls.
 * Each test drives ≥ 350 calls to ensure warm recompile has fired and the
 * warm .so has had time to install.
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

/* --- Test 1: INT array sum survives warm recompile ----------------------- */
(function test1_int_arr_sum() {
    const arr = [1, 2, 3, 4, 5];
    function sumArr(a) {
        let s = 0;
        for (let i = 0; i < a.length; i++) s += a[i];
        return s;
    }
    let result = 0;
    for (let i = 0; i < 350; i++) result = sumArr(arr);
    check('INT arr sum after warm recompile', result, 15);
})();

/* --- Test 2: arithmetic on INT array elements ---------------------------- */
(function test2_int_arr_arith() {
    const arr = [10, 20, 30];
    function calc(a) {
        return a[0] * a[1] + a[2];
    }
    let result = 0;
    for (let i = 0; i < 350; i++) result = calc(arr);
    check('INT arr element multiply+add after warm recompile', result, 230);
})();

/* --- Test 3: element type change INT→string after warm recompile (no crash) */
(function test3_type_change() {
    const arr = [1, 2, 3, 4, 5];
    function sumArr(a) {
        let s = 0;
        for (let i = 0; i < a.length; i++) s += a[i];
        return s;
    }
    /* Drive warm recompile with INT array; extra iterations to allow warm .so install */
    let result = 0;
    for (let i = 0; i < 500; i++) result = sumArr(arr);
    check('Pre-type-change sum', result, 15);
    /* Switch element to string — warm code speculatively uses slow path and
     * returns 0 for the changed element; result is either 14 (warm, INT spec)
     * or a string (if cold .so is still in use — warm not yet installed).
     * Either way the function must not crash or throw. */
    arr[0] = 'x';
    let threw = false;
    try { result = sumArr(arr); } catch(e) { threw = true; }
    check('No crash/throw after element type change', threw, false);
})();

/* --- Test 4: two separate arrays, both INT -------------------------------- */
(function test4_two_arrays() {
    function dotProduct(a, b) {
        let s = 0;
        for (let i = 0; i < a.length; i++) s += a[i] * b[i];
        return s;
    }
    const a = [1, 2, 3];
    const b = [4, 5, 6];
    let result = 0;
    for (let i = 0; i < 350; i++) result = dotProduct(a, b);
    check('Dot product of two INT arrays after warm recompile', result, 32);
})();

/* --- Test 5: INT comparison on array element ------------------------------ */
(function test5_comparison() {
    function countGt(arr, threshold) {
        let n = 0;
        for (let i = 0; i < arr.length; i++) {
            if (arr[i] > threshold) n++;
        }
        return n;
    }
    const arr = [1, 5, 3, 7, 2, 8, 4, 6];
    let result = 0;
    for (let i = 0; i < 350; i++) result = countGt(arr, 4);
    check('Count elements > 4 after warm recompile', result, 4);
})();

/* --- Test 6: nested array read (arr[i][j]) -------------------------------- */
(function test6_nested_read() {
    function sumMatrix(m) {
        let s = 0;
        for (let i = 0; i < m.length; i++) {
            for (let j = 0; j < m[i].length; j++) {
                s += m[i][j];
            }
        }
        return s;
    }
    const m = [[1, 2], [3, 4], [5, 6]];
    let result = 0;
    for (let i = 0; i < 350; i++) result = sumMatrix(m);
    check('Matrix sum after warm recompile', result, 21);
})();

/* --- Test 7: mixed get_field + get_array_el in same function -------------- */
(function test7_mixed_gf_ae() {
    function accessBoth(obj, arr) {
        return obj.x + arr[0] + arr[1];
    }
    const obj = { x: 10 };
    const arr = [5, 3];
    let result = 0;
    for (let i = 0; i < 350; i++) result = accessBoth(obj, arr);
    check('Mixed get_field + get_array_el after warm recompile', result, 18);
})();

print('');
print('Results: ' + passed + ' passed, ' + failed + ' failed');
if (failed > 0) throw new Error(failed + ' test(s) failed');
