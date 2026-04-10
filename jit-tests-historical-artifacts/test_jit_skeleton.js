/*
 * jit-tests/test_jit_skeleton.js
 *
 * Phase 1.1 — Smoke test: verifies the JIT infrastructure compiles and
 * links without errors.  All tests must pass identically whether the engine
 * is built with or without CONFIG_JIT=y.
 *
 * Run (no JIT):   ./qjs jit-tests/test_jit_skeleton.js
 * Run (with JIT): make CONFIG_JIT=y && ./qjs jit-tests/test_jit_skeleton.js
 */

"use strict";

function assert(cond, msg) {
    if (!cond) throw new Error("FAIL: " + msg);
}

/* Basic interpreter sanity — must work with and without JIT */
function add(a, b) { return a + b; }
function fib(n) { return n <= 1 ? n : fib(n - 1) + fib(n - 2); }

assert(add(1, 2) === 3,   "add(1,2) === 3");
assert(add("a", "b") === "ab", "string concat");
assert(fib(10) === 55,    "fib(10) === 55");
assert(fib(0) === 0,      "fib(0) === 0");
assert(fib(1) === 1,      "fib(1) === 1");

/* Closures — exercises var_refs path */
function makeCounter(start) {
    let n = start;
    return function() { return n++; };
}
const c = makeCounter(10);
assert(c() === 10, "counter 10");
assert(c() === 11, "counter 11");
assert(c() === 12, "counter 12");

/* Exceptions must still work */
let caught = false;
try { null.x; } catch(e) { caught = true; }
assert(caught, "exception caught");

/* typeof must work */
assert(typeof 42 === "number",    "typeof number");
assert(typeof "s" === "string",   "typeof string");
assert(typeof true === "boolean", "typeof boolean");
assert(typeof null === "object",  "typeof null");
assert(typeof undefined === "undefined", "typeof undefined");

print("PASS test_jit_skeleton.js");
