/* test_jit_tcc.js — Phase 3 TCC tier-1 JIT tests
 *
 * When run with JIT_THRESHOLD_TCC=2 (or any small value) each function
 * below gets TCC-compiled after 2 calls and the remaining calls use the
 * JIT-generated code.  The test verifies semantic correctness.
 */

function assert(cond, msg) {
    if (!cond) throw new Error("FAIL: " + msg);
}

/* Call each function many times to ensure JIT path is exercised. */
function repeat(f, n) {
    let r;
    for (let i = 0; i < n; i++) r = f();
    return r;
}

/* --- Arithmetic --- */
function arith_test() {
    let x = 3, y = 4;
    return x * x + y * y;  // 25
}
assert(repeat(arith_test, 10) === 25, "arith_test");

/* --- Fibonacci (recursion + conditionals) --- */
function fib(n) {
    if (n <= 1) return n;
    return fib(n-1) + fib(n-2);
}
// warm up so all callees get JIT-compiled
for (let i = 0; i < 5; i++) fib(i);
assert(fib(15) === 610, "fib(15)");
assert(fib(20) === 6765, "fib(20)");

/* --- Loop with locals --- */
function sum_squares(n) {
    let s = 0;
    for (let i = 1; i <= n; i++) s += i * i;
    return s;
}
for (let k = 0; k < 5; k++) sum_squares(5);
assert(sum_squares(5) === 55, "sum_squares");
assert(sum_squares(10) === 385, "sum_squares 10");

/* --- Closures over var refs --- */
function make_adder(x) {
    return function(y) { return x + y; };
}
let add5 = make_adder(5);
let add10 = make_adder(10);
for (let i = 0; i < 5; i++) { add5(i); add10(i); }
assert(add5(3) === 8, "closure add5");
assert(add10(3) === 13, "closure add10");

/* --- String operations --- */
function concat(a, b) { return a + b; }
for (let i = 0; i < 5; i++) concat("x", "y");
assert(concat("hello", " world") === "hello world", "concat");

/* --- Property access --- */
function point_dist(p) { return p.x * p.x + p.y * p.y; }
let p = {x: 3, y: 4};
for (let i = 0; i < 5; i++) point_dist(p);
assert(point_dist(p) === 25, "point_dist");

/* --- typeof --- */
function is_num(v) { return typeof v === "number"; }
for (let i = 0; i < 5; i++) is_num(i);
assert(is_num(42), "is_num(42)");
assert(!is_num("x"), "is_num string");

/* --- Boolean short-circuit (uses if_false/if_true opcodes) --- */
function and_test(a, b) { return a && b; }
function or_test(a, b)  { return a || b; }
for (let i = 0; i < 5; i++) { and_test(1,2); or_test(0,2); }
assert(and_test(1, 2) === 2, "and_test");
assert(or_test(0, 2) === 2, "or_test");
assert(and_test(0, 2) === 0, "and_test false");

print("PASS test_jit_tcc.js");
