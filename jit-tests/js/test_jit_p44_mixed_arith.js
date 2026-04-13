/* P44 — Mixed-Type Arithmetic Fast Path
 * Tests half-typed (NUMBER/INT × JSVAL) arithmetic and comparison paths.
 * Each test is run with JIT-compiled hot function and checked for correctness.
 */

/* ---- OP_add: typed accumulator + JSVAL element ---- */
function arr_sum_int(arr) {
    var s = 0;
    for (var i = 0; i < arr.length; i++) s += arr[i];
    return s;
}
var r1 = arr_sum_int([1, 2, 3, 4, 5]);
if (r1 !== 15) throw new Error("arr_sum_int: " + r1);

function arr_sum_float(arr) {
    var s = 0.0;
    for (var i = 0; i < arr.length; i++) s += arr[i];
    return s;
}
var r2 = arr_sum_float([1.5, 2.5, 3.0]);
if (Math.abs(r2 - 7.0) > 1e-10) throw new Error("arr_sum_float: " + r2);

/* ---- OP_sub: typed accumulator minus JSVAL ---- */
function arr_sub(arr) {
    var s = 100;
    for (var i = 0; i < arr.length; i++) s -= arr[i];
    return s;
}
if (arr_sub([10, 20, 5]) !== 65) throw new Error("arr_sub");

function arr_sub_float(arr) {
    var s = 10.0;
    for (var i = 0; i < arr.length; i++) s -= arr[i];
    return s;
}
if (Math.abs(arr_sub_float([1.5, 2.5]) - 6.0) > 1e-10)
    throw new Error("arr_sub_float: " + arr_sub_float([1.5, 2.5]));

/* ---- OP_mul: typed × JSVAL ---- */
function arr_product(arr) {
    var p = 1;
    for (var i = 0; i < arr.length; i++) p *= arr[i];
    return p;
}
if (arr_product([2, 3, 4]) !== 24) throw new Error("arr_product");

function arr_product_float(arr) {
    var p = 1.0;
    for (var i = 0; i < arr.length; i++) p *= arr[i];
    return p;
}
if (Math.abs(arr_product_float([1.5, 2.0, 4.0]) - 12.0) > 1e-10)
    throw new Error("arr_product_float");

/* ---- OP_div: typed / JSVAL ---- */
function arr_div(arr) {
    var s = 1024.0;
    for (var i = 0; i < arr.length; i++) s /= arr[i];
    return s;
}
if (Math.abs(arr_div([2, 4, 8]) - 16.0) > 1e-10)
    throw new Error("arr_div: " + arr_div([2, 4, 8]));

/* ---- OP_mod: typed % JSVAL ---- */
function arr_mod_check(arr) {
    var r = 0;
    for (var i = 0; i < arr.length; i++) {
        if ((100 % arr[i]) === 0) r++;
    }
    return r;
}
if (arr_mod_check([5, 10, 7, 25]) !== 3) throw new Error("arr_mod_check");

/* ---- JSVAL on left, typed on right (reversed operand order) ---- */
function arr_sub_r(arr) {
    /* JSVAL element minus INT constant */
    var s = 0;
    var k = 3;
    for (var i = 0; i < arr.length; i++) s += (arr[i] - k);
    return s;
}
if (arr_sub_r([10, 11, 12]) !== 24) throw new Error("arr_sub_r: " + arr_sub_r([10,11,12]));

/* ---- Overflow / edge: JSVAL element is string → add vtable path must fire ---- */
function arr_concat(arr) {
    var s = "";
    for (var i = 0; i < arr.length; i++) s += arr[i];
    return s;
}
if (arr_concat(["a", "b", "c"]) !== "abc") throw new Error("arr_concat");

/* ---- Comparison: typed INT vs JSVAL in fused branch (OP_lt half-typed) ---- */
function count_below(arr, limit) {
    /* limit is a typed INT arg; arr[i] is JSVAL — tests HALF_R comparison */
    var n = 0;
    for (var i = 0; i < arr.length; i++) {
        if (arr[i] < limit) n++;
    }
    return n;
}
/* arr[i]=JSVAL, limit=typed arg (gets JIT_T_INT from call-site if JIT infers) */
if (count_below([1, 5, 3, 9, 2], 4) !== 3) throw new Error("count_below");

function count_above(arr, limit) {
    var n = 0;
    for (var i = 0; i < arr.length; i++) {
        if (arr[i] > limit) n++;
    }
    return n;
}
if (count_above([1, 5, 3, 9, 2], 4) !== 2) throw new Error("count_above");

/* typed loop counter vs JSVAL threshold */
function first_exceeding(n, threshold) {
    for (var i = 1; i <= n; i++) {
        var v = [10, 20, 30, 40, 50][i - 1];  /* JSVAL array load */
        if (i > threshold) return i;
    }
    return -1;
}
if (first_exceeding(5, 3) !== 4) throw new Error("first_exceeding");

/* ---- Large iteration: verify numeric correctness at scale ---- */
function sum_range(n) {
    /* s is NUMBER accumulator (starts 0.0), arr[i] is JSVAL */
    var arr = [];
    for (var i = 0; i < n; i++) arr.push(i);
    var s = 0.0;
    for (var i = 0; i < arr.length; i++) s += arr[i];
    return s;
}
var expected = (99 * 100) / 2;  /* sum 0..99 */
if (sum_range(100) !== expected) throw new Error("sum_range: " + sum_range(100));

print("P44 mixed-type arithmetic: ALL PASS");
