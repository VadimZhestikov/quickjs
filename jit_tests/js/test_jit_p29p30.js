/* test_jit_p29p30.js — JIT P29/P30
 *   P29: yield_star, async_yield_star
 *   P30: for_await_of_start/next, with_get_var/put_var/delete_var/make_ref/get_ref
 *
 * Note: this file intentionally omits "use strict" because OP_with_* opcodes
 * are only generated for non-strict-mode code (with-statements).
 */

function assert(cond, msg) {
    if (!cond) throw new Error("FAIL: " + msg);
}
function assertEq(a, b, msg) {
    if (a !== b) throw new Error("FAIL: " + msg + " — got " + JSON.stringify(a) + " expected " + JSON.stringify(b));
}
function assertDeepEq(a, b, msg) {
    if (JSON.stringify(a) !== JSON.stringify(b))
        throw new Error("FAIL: " + msg + " — got " + JSON.stringify(a) + " expected " + JSON.stringify(b));
}

/* ============================================================
 * P29 — yield_star (sync generator delegation)
 * ============================================================ */

function* genDelegateArray() {
    yield 1;
    yield* [2, 3, 4];
    yield 5;
}
for (let i = 0; i < 200; i++) {
    assertDeepEq([...genDelegateArray()], [1, 2, 3, 4, 5], "yield* array delegation");
}

/* yield* with another generator */
function* inner() {
    yield 10;
    yield 20;
    yield 30;
}
function* outer() {
    yield 0;
    yield* inner();
    yield 40;
}
for (let i = 0; i < 200; i++) {
    assertDeepEq([...outer()], [0, 10, 20, 30, 40], "yield* generator delegation");
}

/* yield* return value — the done value of the inner iterator */
function* producingReturn() {
    yield 1;
    return "inner-done";
}
function* consumingYieldStar() {
    let ret = yield* producingReturn();
    yield ret; /* ret should be "inner-done" */
}
for (let i = 0; i < 200; i++) {
    assertDeepEq([...consumingYieldStar()], [1, "inner-done"], "yield* return value");
}

/* yield* with string (iterable) */
function* yieldStarString() {
    yield* "abc";
}
for (let i = 0; i < 200; i++) {
    assertDeepEq([...yieldStarString()], ["a", "b", "c"], "yield* string delegation");
}

/* Multiple sequential yield* */
function* multiDelegate() {
    yield* [1, 2];
    yield* [3, 4];
    yield* [5];
}
for (let i = 0; i < 200; i++) {
    assertDeepEq([...multiDelegate()], [1, 2, 3, 4, 5], "yield* multi sequential");
}

/* yield* with spread interleaved */
function* mixedGen() {
    yield 'a';
    yield* ['b', 'c'];
    yield 'd';
    yield* ['e'];
}
for (let i = 0; i < 200; i++) {
    assertDeepEq([...mixedGen()], ['a', 'b', 'c', 'd', 'e'], "yield* mixed");
}

/* ============================================================
 * P29 — async_yield_star (async generator delegation)
 * ============================================================ */

async function* asyncGenDelegate() {
    yield 1;
    yield* [2, 3];
    yield 4;
}

async function runAsyncYieldStar() {
    let result = [];
    for await (let v of asyncGenDelegate()) {
        result.push(v);
    }
    assertDeepEq(result, [1, 2, 3, 4], "async yield* delegation");
}

/* ============================================================
 * P30 — for_await_of (async for-of)
 * ============================================================ */

async function sumForAwait(values) {
    let sum = 0;
    for await (let v of values) {
        sum += v;
    }
    return sum;
}

async function runForAwait() {
    /* Sync iterable works with for await */
    let s = await sumForAwait([1, 2, 3, 4, 5]);
    assertEq(s, 15, "for await of sync iterable sum");

    /* Async iterable */
    async function* asyncRange(n) {
        for (let i = 0; i < n; i++) yield i;
    }
    let s2 = await sumForAwait(asyncRange(5));
    assertEq(s2, 10, "for await of async generator sum");
}

async function forAwaitMultipleIterations() {
    for (let iter = 0; iter < 50; iter++) {
        let result = [];
        for await (let v of [10, 20, 30]) {
            result.push(v);
        }
        assertDeepEq(result, [10, 20, 30], "for await repeated iter " + iter);
    }
}

/* ============================================================
 * P30 — with_* (object environment record lookup)
 * Note: 'with' is a non-strict-mode feature; these tests run in sloppy mode
 * ============================================================ */

/* with_get_var: read variable from with-scope */
function testWithGetVar() {
    let obj = { x: 42, y: 100 };
    let result;
    /* jshint ignore:start */
    with (obj) {
        result = x;  /* uses OP_with_get_var */
    }
    /* jshint ignore:end */
    return result;
}
/* 'with' disallowed in strict mode — wrap in non-strict function */
(function() {
    for (let i = 0; i < 200; i++) {
        assertEq(testWithGetVar(), 42, "with_get_var");
    }
})();

/* with_get_var fallthrough: variable NOT in scope object — reads outer scope */
(function() {
    let outer_val = 99;
    function testWithFallthrough() {
        let obj = { unrelated: 1 };
        let result;
        with (obj) {
            result = outer_val;  /* with_get_var falls through to outer scope */
        }
        return result;
    }
    for (let i = 0; i < 200; i++) {
        assertEq(testWithFallthrough(), 99, "with_get_var fallthrough");
    }
})();

/* with_put_var: write to property found in with-scope */
(function() {
    function testWithPutVar() {
        let obj = { z: 0 };
        with (obj) {
            z = 77;  /* uses OP_with_put_var */
        }
        return obj.z;
    }
    for (let i = 0; i < 200; i++) {
        assertEq(testWithPutVar(), 77, "with_put_var");
    }
})();

/* with_delete_var: delete property from with-scope */
(function() {
    function testWithDeleteVar() {
        let obj = { toDelete: 123 };
        let result;
        with (obj) {
            result = delete toDelete;  /* uses OP_with_delete_var */
        }
        return result && !('toDelete' in obj);
    }
    for (let i = 0; i < 200; i++) {
        assert(testWithDeleteVar(), "with_delete_var");
    }
})();

/* Nested with */
(function() {
    function testNestedWith() {
        let a = { p: 1 };
        let b = { q: 2 };
        let r;
        with (a) {
            with (b) {
                r = p + q;  /* p from outer with, q from inner with */
            }
        }
        return r;
    }
    for (let i = 0; i < 200; i++) {
        assertEq(testNestedWith(), 3, "nested with");
    }
})();

/* with: property shadowing outer variable */
(function() {
    function testWithShadow() {
        let x = 1;
        let obj = { x: 2 };
        let r;
        with (obj) {
            r = x;  /* should read obj.x, not outer x */
        }
        return r;
    }
    for (let i = 0; i < 200; i++) {
        assertEq(testWithShadow(), 2, "with shadow");
    }
})();

/* ============================================================
 * Run async tests (top-level async IIFE — QuickJS drains promise queue on exit)
 * ============================================================ */

(async function() {
    await runAsyncYieldStar();
    await runForAwait();
    await forAwaitMultipleIterations();
    print("test_jit_p29p30: all tests passed");
})().catch(function(e) {
    print("UNHANDLED ERROR: " + e);
    throw e;
});
