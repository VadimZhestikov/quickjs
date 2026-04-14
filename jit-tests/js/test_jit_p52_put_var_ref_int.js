/* P52 — Warm-IC recompile: skip refcount check for INT put_var_ref / set_var_ref
 *
 * Tests the two-phase JIT optimisation for put_var_ref* and set_var_ref* opcodes.
 * Cold compile emits __jit_vt_HASH[base + pv_idx] BSS slots recording the old
 * cell value tag before the write; after JIT_WARM_THRESHOLD_GCC calls a warm
 * recompile fires and when vt_hints[pv_idx]==JS_TAG_INT (0) the write becomes
 * just  *_vrp = JS_MKVAL(JS_TAG_INT, (int32_t)_ti)  — no JS_VALUE_HAS_REF_COUNT
 * test needed because INT JSValues are immediate (no refcount).
 *
 * Each test drives >= 350 calls so the warm .so has installed.
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

/* --- Test 1: basic counter closure (put_var_ref INT fast path) ----------- */
(function test1_counter() {
    /* makeCounter captures 'n' (an INT cell).  Each call does:
     *   get_var_ref n   → _ti via P49 warm INT
     *   add 1           → _ti via P51 warm INT
     *   put_var_ref n   → warm INT write via P52 (no refcount check)
     *   return n        */
    function makeCounter() {
        let n = 0;
        return function() { n += 1; return n; };
    }

    const cnt = makeCounter();
    let last = 0;
    for (let i = 0; i < 400; i++) last = cnt();
    check('counter after 400 increments', last, 400);
})();

/* --- Test 2: two captured INT vars, both use put_var_ref warm path ------- */
(function test2_two_vars() {
    function makeAccum() {
        let a = 0;
        let b = 0;
        return function(x) { a += x; b += 1; return a + b; };
    }

    const acc = makeAccum();
    let r = 0;
    for (let i = 0; i < 400; i++) r = acc(i);
    /* After 400 calls with i=0..399:  a=0+1+…+399=79800,  b=400,  r=80200 */
    check('two vars: a+b after 400 calls', r, 80200);
})();

/* --- Test 3: set_var_ref (peek variant — stack not popped) --------------- */
(function test3_set_var_ref() {
    /* The compiler emits OP_set_var_ref (rather than put_var_ref) when the
     * stored value is also kept on the stack, e.g. the result of an assignment
     * used in an expression.  Build a pattern: x = (x + 1). */
    function makeSetVarRef() {
        let x = 0;
        return function() { return (x = x + 1); };
    }

    const f = makeSetVarRef();
    let v = 0;
    for (let i = 0; i < 400; i++) v = f();
    check('set_var_ref: x after 400 increments', v, 400);
})();

/* --- Test 4: correctness — put_var_ref warm INT write retains value ------- */
(function test4_value_correctness() {
    /* Multiple captured INT cells, non-trivial arithmetic. */
    function makeFib() {
        let a = 0, b = 1;
        return function() {
            let tmp = a + b;
            a = b;
            b = tmp;
            return b;
        };
    }

    const fib = makeFib();
    let r = 0;
    for (let i = 0; i < 400; i++) r = fib();
    /* After 400 calls the Fibonacci sequence overflows int32 quickly into
     * float64.  We check only after a small fixed prefix to keep the test
     * deterministic and independent of float precision. */
    const fib2 = makeFib();
    let vals = [];
    for (let i = 0; i < 15; i++) vals.push(fib2());
    /* F(1)..F(15): 1,2,3,5,8,13,21,34,55,89,144,233,377,610,987 */
    check('fib(15) correctness', vals[14], 987);
    check('fib(10) correctness', vals[9], 89);
})();

/* --- Test 5: put_var_ref after conditional (mixed INT stores) ------------ */
(function test5_conditional_store() {
    function makeAbs() {
        let total = 0;
        return function(x) { total += (x >= 0 ? x : -x); return total; };
    }

    const f = makeAbs();
    let s = 0;
    for (let i = 0; i < 400; i++) s = f(i % 2 === 0 ? i : -i);
    /* sum of |0|+|-1|+|2|+|-3|+... = 0+1+2+3+...+399 = 79800 */
    check('abs accumulator over 400 calls', s, 79800);
})();

print('');
print('Results: ' + passed + ' passed, ' + failed + ' failed');
if (failed > 0) throw new Error('P52 tests failed');
