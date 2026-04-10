/* Test suite for JIT Phase 18:
 * - Type-test opcodes: is_null, is_undefined, is_undefined_or_null,
 *                      typeof_is_undefined, typeof_is_function
 * - Stack-shuffle opcodes: dup3, nip1, insert3, insert4,
 *                          perm3, perm4, perm5, rot4l, rot5l, swap2
 *
 * Each test function is simple enough to be JIT-compiled at warm-up.
 * We run each N times so the JIT compiles it, then verify the result.
 */

'use strict';

var passed = 0;
var failed = 0;

function assert(cond, msg) {
    if (!cond) {
        print("FAIL: " + msg);
        failed++;
    } else {
        passed++;
    }
}

function assertEq(a, b, msg) {
    if (a !== b) {
        print("FAIL: " + msg + " (got " + a + ", expected " + b + ")");
        failed++;
    } else {
        passed++;
    }
}

/* =========================================================
 * Type-test opcodes
 * ========================================================= */

/* OP_is_null: emitted by the optimizer for `x === null` */
function test_is_null(x) { return x === null; }
/* OP_is_undefined: emitted for `x === undefined` */
function test_is_undefined(x) { return x === undefined; }
/* OP_is_undefined_or_null: emitted for `x == null` (loose equality) */
function test_is_undef_or_null(x) { return x == null; }
/* OP_typeof_is_undefined: typeof x === "undefined" */
function test_typeof_undef(x) { return typeof x === "undefined"; }
/* OP_typeof_is_function: typeof x === "function" */
function test_typeof_func(x) { return typeof x === "function"; }

/* Warm up */
for (var _i = 0; _i < 200; _i++) {
    test_is_null(null);
    test_is_undefined(undefined);
    test_is_undef_or_null(null);
    test_typeof_undef(undefined);
    test_typeof_func(print);
}

/* is_null */
assert(test_is_null(null),           "is_null(null) => true");
assert(!test_is_null(undefined),     "is_null(undefined) => false");
assert(!test_is_null(0),             "is_null(0) => false");
assert(!test_is_null(""),            "is_null('') => false");
assert(!test_is_null(false),         "is_null(false) => false");
assert(!test_is_null({}),            "is_null({}) => false");

/* is_undefined */
assert(test_is_undefined(undefined), "is_undefined(undefined) => true");
assert(!test_is_undefined(null),     "is_undefined(null) => false");
assert(!test_is_undefined(0),        "is_undefined(0) => false");

/* is_undefined_or_null */
assert(test_is_undef_or_null(null),      "undef_or_null(null) => true");
assert(test_is_undef_or_null(undefined), "undef_or_null(undefined) => true");
assert(!test_is_undef_or_null(0),        "undef_or_null(0) => false");
assert(!test_is_undef_or_null(""),       "undef_or_null('') => false");
assert(!test_is_undef_or_null(false),    "undef_or_null(false) => false");

/* typeof_is_undefined */
assert(test_typeof_undef(undefined),   "typeof_undef(undefined) => true");
assert(!test_typeof_undef(null),       "typeof_undef(null) => false");
assert(!test_typeof_undef(0),          "typeof_undef(0) => false");

/* typeof_is_function */
assert(test_typeof_func(print),        "typeof_func(print) => true");
assert(test_typeof_func(function(){}), "typeof_func(function(){}) => true");
assert(!test_typeof_func(null),        "typeof_func(null) => false");
assert(!test_typeof_func(42),          "typeof_func(42) => false");
assert(!test_typeof_func({}),          "typeof_func({}) => false");

/* =========================================================
 * Stack-shuffle opcodes
 * These are exercised implicitly through JS patterns that
 * the compiler emits them for.
 * ========================================================= */

/* ---- OP_dup3: a b c -> a b c a b c ----
 * The compiler emits dup3 for destructuring assignments with temp:
 * e.g. [a, b, c] = [c, a, b]  (triple swap via temporaries)
 * Simplest direct trigger: array destructuring swap */
function test_dup3() {
    var a = 1, b = 2, c = 3;
    [a, b, c] = [c, a, b];
    return a * 100 + b * 10 + c;
}
for (var _i = 0; _i < 200; _i++) test_dup3();
assertEq(test_dup3(), 312, "dup3: [1,2,3] cycled -> 312");

/* ---- OP_nip1: a b c -> b c ----
 * Emitted in for-in/for-of cleanup. Use a for-in loop. */
function test_nip1() {
    var obj = {x:1, y:2, z:3};
    var sum = 0;
    for (var k in obj) sum += obj[k];
    return sum;
}
for (var _i = 0; _i < 200; _i++) test_nip1();
assertEq(test_nip1(), 6, "nip1: for-in sum == 6");

/* ---- OP_insert3 / OP_insert4 and OP_perm3 / OP_perm4 / OP_perm5 ----
 * These appear in compound assignment to property/index expressions.
 * e.g.  obj.x += n   emits: get_obj, get_field x, add, insert3/perm3, put_field x
 *       obj[k] += n  emits: get_obj, get_key, get_array_el, insert4/perm4, put_array_el
 */
function test_perm3(obj) {
    obj.x += 10;
    obj.y += 20;
    return obj.x + obj.y;
}
for (var _i = 0; _i < 200; _i++) test_perm3({x:1,y:2});
assertEq(test_perm3({x: 5, y: 3}), 38, "perm3: obj.x+=10, obj.y+=20 -> 38");

function test_perm4(arr, i) {
    arr[i] += 7;
    return arr[i];
}
for (var _i = 0; _i < 200; _i++) test_perm4([1,2,3], 1);
assertEq(test_perm4([10, 20, 30], 2), 37, "perm4: arr[2]+=7 -> 37");

function test_perm5(obj, key) {
    obj[key] += 5;
    return obj[key];
}
for (var _i = 0; _i < 200; _i++) test_perm5({a:1}, 'a');
assertEq(test_perm5({a: 100}, 'a'), 105, "perm5: obj[key]+=5 -> 105");

/* ---- OP_rot4l / OP_rot5l ----
 * Emitted in some for-of patterns and multi-temp destructuring.
 * Use compound assignment through closure to trigger. */
function test_rot4l() {
    var result = [];
    var pairs = [[1,10],[2,20],[3,30]];
    for (var [k,v] of pairs) {
        result.push(k + v);
    }
    return result[0] + result[1] + result[2];
}
for (var _i = 0; _i < 200; _i++) test_rot4l();
assertEq(test_rot4l(), 66, "rot4l: for-of destructuring sum == 66");

function test_rot5l() {
    /* rot5l appears in multi-level compound assignments.
     * Use a nested property update pattern. */
    var o = {a: {v: 1}};
    o.a.v += 41;
    return o.a.v;
}
for (var _i = 0; _i < 200; _i++) test_rot5l();
assertEq(test_rot5l(), 42, "rot5l: nested prop += 41 -> 42");

/* ---- OP_swap2 ----
 * swap2 appears in destructuring with two temporaries needed simultaneously.
 * Use a two-element swap via array destructuring. */
function test_swap2() {
    var a = 7, b = 13;
    [a, b] = [b, a];
    return a * 100 + b;
}
for (var _i = 0; _i < 200; _i++) test_swap2();
assertEq(test_swap2(), 1307, "swap2: swap a=7,b=13 -> a=13,b=7 -> 1307");

/* =========================================================
 * Summary
 * ========================================================= */
print("P18 tests: " + passed + " passed, " + failed + " failed");
if (failed > 0) throw new Error("P18 test failures: " + failed);
