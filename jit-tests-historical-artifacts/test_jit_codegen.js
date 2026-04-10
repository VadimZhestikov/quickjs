/* test_jit_codegen.js — Phase 2 code generator smoke tests
 *
 * These run under the interpreter (CONFIG_JIT=y stubs or not) and verify
 * that JS semantics are preserved.  When Phase 3 is wired they will also
 * exercise the generated C code paths.
 */

function assert(cond, msg) {
    if (!cond) throw new Error("FAIL: " + msg);
}

/* Arithmetic */
function add(a, b) { return a + b; }
assert(add(1, 2) === 3, "add integers");
assert(add(1.5, 2.5) === 4.0, "add floats");
assert(add("a", "b") === "ab", "add strings");

function fib(n) {
    if (n <= 1) return n;
    return fib(n-1) + fib(n-2);
}
assert(fib(10) === 55, "fib(10)");

/* Locals */
function locals_test() {
    let a = 10;
    let b = 20;
    let c = a + b;
    return c;
}
assert(locals_test() === 30, "locals");

/* Closures */
function make_counter() {
    let n = 0;
    return function() { return ++n; };
}
let cnt = make_counter();
assert(cnt() === 1, "counter 1");
assert(cnt() === 2, "counter 2");
assert(cnt() === 3, "counter 3");

/* Loops */
function sum_to(n) {
    let s = 0;
    for (let i = 1; i <= n; i++) s += i;
    return s;
}
assert(sum_to(100) === 5050, "sum_to(100)");

/* Property access */
function prop_test() {
    let o = { x: 1, y: 2 };
    return o.x + o.y;
}
assert(prop_test() === 3, "property access");

/* Method calls */
function method_test() {
    let a = [1, 2, 3];
    return a.length;
}
assert(method_test() === 3, "array length");

/* Boolean and comparisons */
function cmp_test(a, b) {
    return a < b ? "less" : a > b ? "greater" : "equal";
}
assert(cmp_test(1, 2) === "less", "cmp less");
assert(cmp_test(2, 1) === "greater", "cmp greater");
assert(cmp_test(1, 1) === "equal", "cmp equal");

/* typeof */
function typeof_test(v) { return typeof v; }
assert(typeof_test(1) === "number", "typeof number");
assert(typeof_test("s") === "string", "typeof string");
assert(typeof_test(undefined) === "undefined", "typeof undefined");

print("PASS test_jit_codegen.js");
