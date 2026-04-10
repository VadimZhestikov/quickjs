"use strict";
/* test_jit_p33.js — JIT P33: has_simple_parameter_list=false
 *   Covers: rest params, default params, destructuring params.
 *
 * Key invariant under test:
 *   The JIT must pass the ORIGINAL argc to the compiled function so that
 *   OP_rest computes the correct rest-element count.  All arg slots
 *   [0..arg_count) are unconditionally readable/writable because arg_buf
 *   is always padded to arg_count by the caller (js_jit_call and the
 *   interpreter hot-path).
 */

function assert(cond, msg) {
    if (!cond) throw new Error("FAIL: " + msg);
}
function assertEq(a, b, msg) {
    if (a !== b) throw new Error("FAIL: " + msg + " — got " + JSON.stringify(a) + " expected " + JSON.stringify(b));
}

/* ============================================================
 * REST PARAMETERS  (arg_count includes rest slot)
 * ============================================================ */

/* Basic rest: zero extra args → empty rest array */
function restZero(a, b, ...rest) { return rest.length; }
for (let i = 0; i < 200; i++) {
    assertEq(restZero(1, 2), 0, "rest zero length");
}

/* Rest with exactly the right number of args */
function restExact(a, b, ...rest) { return rest; }
for (let i = 0; i < 200; i++) {
    let r = restExact(10, 20, 30, 40);
    assertEq(r.length, 2, "rest exact length");
    assertEq(r[0], 30, "rest exact [0]");
    assertEq(r[1], 40, "rest exact [1]");
}

/* Rest as the only parameter */
function restOnly(...args) { return args; }
for (let i = 0; i < 200; i++) {
    let r = restOnly(1, 2, 3, 4, 5);
    assertEq(r.length, 5, "rest-only length");
    assertEq(r[0], 1, "rest-only [0]");
    assertEq(r[4], 5, "rest-only [4]");
}
for (let i = 0; i < 200; i++) {
    let r = restOnly();
    assertEq(r.length, 0, "rest-only empty");
}

/* Rest sum — verifies arithmetic on rest elements */
function restSum(...nums) {
    var s = 0;
    for (var i = 0; i < nums.length; i++) s += nums[i];
    return s;
}
for (let i = 0; i < 200; i++) {
    assertEq(restSum(1, 2, 3, 4, 5), 15, "rest sum");
    assertEq(restSum(), 0, "rest sum empty");
    assertEq(restSum(7), 7, "rest sum single");
}

/* Rest + spread call: sum(...[1,2,3,4,5]) must give 15 */
for (let i = 0; i < 200; i++) {
    var arr = [1, 2, 3, 4, 5];
    assertEq(restSum(...arr), 15, "rest sum via spread");
}

/* Rest + .apply */
for (let i = 0; i < 200; i++) {
    assertEq(restSum.apply(null, [10, 20, 30]), 60, "rest sum via apply");
}

/* Rest element identity (not shallow-copied from outside) */
function restId(...args) { return args; }
for (let i = 0; i < 200; i++) {
    let r = restId(i, i + 1);
    assertEq(r[0], i, "rest id [0]");
    assertEq(r[1], i + 1, "rest id [1]");
}

/* ============================================================
 * DEFAULT PARAMETERS  (GEN_GET_ARG detects undefined slot)
 * ============================================================ */

/* Single default, supplied */
function defA(x, y = 99) { return x + y; }
for (let i = 0; i < 200; i++) {
    assertEq(defA(1, 2), 3, "default supplied");
    assertEq(defA(10), 109, "default used");
}

/* Default at beginning */
function defFirst(x = 5, y) { return x * 10 + y; }
for (let i = 0; i < 200; i++) {
    assertEq(defFirst(2, 3), 23, "default-first supplied");
}

/* Multiple defaults */
function defMulti(a = 1, b = 2, c = 3) { return a + b + c; }
for (let i = 0; i < 200; i++) {
    assertEq(defMulti(), 6, "all defaults");
    assertEq(defMulti(10), 15, "first supplied");
    assertEq(defMulti(10, 20), 33, "two supplied");
    assertEq(defMulti(10, 20, 30), 60, "all supplied");
}

/* Default is an expression */
function defExpr(x, y = x * 2) { return x + y; }
for (let i = 0; i < 200; i++) {
    assertEq(defExpr(3), 9, "default expr");
    assertEq(defExpr(3, 4), 7, "default expr override");
}

/* Default with rest */
function defAndRest(x = 0, ...rest) { return x + rest.length; }
for (let i = 0; i < 200; i++) {
    assertEq(defAndRest(), 0, "def+rest empty");
    assertEq(defAndRest(5), 5, "def+rest no-rest");
    assertEq(defAndRest(5, 1, 2, 3), 8, "def+rest full");
}

/* ============================================================
 * DESTRUCTURING PARAMETERS
 * ============================================================ */

/* Object destructuring */
function objDest({ x, y }) { return x + y; }
for (let i = 0; i < 200; i++) {
    assertEq(objDest({ x: 3, y: 4 }), 7, "obj destruct");
}

/* Array destructuring */
function arrDest([a, b, c]) { return a + b + c; }
for (let i = 0; i < 200; i++) {
    assertEq(arrDest([1, 2, 3]), 6, "arr destruct");
}

/* Destructuring with default */
function objDestDef({ x = 10, y = 20 } = {}) { return x + y; }
for (let i = 0; i < 200; i++) {
    assertEq(objDestDef({ x: 1, y: 2 }), 3, "obj destruct supplied");
    assertEq(objDestDef({}), 30, "obj destruct defaults");
}

/* Mixed: normal + destructuring + rest */
function mixed(a, { b, c }, ...rest) { return a + b + c + rest.length; }
for (let i = 0; i < 200; i++) {
    assertEq(mixed(1, { b: 2, c: 3 }), 6, "mixed no rest");
    assertEq(mixed(1, { b: 2, c: 3 }, 4, 5), 8, "mixed with rest");
}

/* ============================================================
 * COMBINED SCENARIOS
 * ============================================================ */

/* Class method with rest */
class Formatter {
    constructor(sep) { this.sep = sep; }
    format(...parts) { return parts.join(this.sep); }
}
for (let i = 0; i < 200; i++) {
    let f = new Formatter("-");
    assertEq(f.format("a", "b", "c"), "a-b-c", "class rest method");
    assertEq(f.format("x"), "x", "class rest single");
    assertEq(f.format(), "", "class rest empty");
}

/* Recursive with rest */
function sum2(...nums) {
    if (nums.length === 0) return 0;
    return nums[0] + sum2(...nums.slice(1));
}
for (let i = 0; i < 200; i++) {
    assertEq(sum2(1, 2, 3), 6, "recursive rest");
}

/* Closure capturing rest */
function makeAdder(...increments) {
    return function(x) {
        var r = x;
        for (var k = 0; k < increments.length; k++) r += increments[k];
        return r;
    };
}
for (let i = 0; i < 200; i++) {
    let add = makeAdder(1, 2, 3);
    assertEq(add(10), 16, "closure rest capture");
}

/* Nested function with rest */
function outer(x, ...rest) {
    function inner(y) { return x + y + rest.length; }
    return inner(10);
}
for (let i = 0; i < 200; i++) {
    assertEq(outer(1, 2, 3, 4), 14, "nested rest");  /* x=1, y=10, rest=[2,3,4] → 1+10+3=14 */
    assertEq(outer(1), 11, "nested rest empty");     /* x=1, y=10, rest=[] → 1+10+0=11 */
}

print("test_jit_p33: all tests passed");
