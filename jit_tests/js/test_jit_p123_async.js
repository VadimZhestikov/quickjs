/* JIT P12.3: async function JIT correctness tests
 * Run with enough iterations to trigger JIT compilation (threshold ~100).
 *
 * In QuickJS, Promise microtasks run after the top-level script, so we
 * sequence all assertions inside a single async IIFE and print the final
 * "ALL PASS" line there.  Any assertion failure throws, making the Promise
 * reject and causing qjs to report an unhandled rejection (non-zero exit).
 */
function assert(v, msg) {
    if (!v) throw new Error("FAIL: " + (msg || "assertion"));
}

/* ---- basic async: no await ---- */
async function test_no_await() {
    async function f(x) { return x + 1; }
    var i;
    for (i = 0; i < 200; i++) {
        var r = await f(5);
        assert(r === 6, "no_await: " + r);
    }
    print("test_no_await: PASS");
}

/* ---- single await with already-resolved Promise ---- */
async function test_single_await() {
    async function f(x) {
        var v = await Promise.resolve(x + 10);
        return v * 2;
    }
    var i;
    for (i = 0; i < 200; i++) {
        var r = await f(5);
        assert(r === 30, "single_await: " + r);
    }
    print("test_single_await: PASS");
}

/* ---- multiple awaits, locals preserved across await ---- */
async function test_multiple_await() {
    async function f(a, b) {
        var x = await Promise.resolve(a);
        var y = await Promise.resolve(b);
        return x + y;
    }
    var i;
    for (i = 0; i < 200; i++) {
        var r = await f(3, 7);
        assert(r === 10, "multiple_await: " + r);
    }
    print("test_multiple_await: PASS");
}

/* ---- await inside try/catch — rejected promise is caught ---- */
async function test_await_try_catch() {
    async function f() {
        var result;
        try {
            var v = await Promise.reject(new Error("boom"));
            result = "no_throw";
        } catch(e) {
            result = "caught:" + e.message;
        }
        return result;
    }
    var i;
    for (i = 0; i < 200; i++) {
        var r = await f();
        assert(r === "caught:boom", "await_try_catch: " + r);
    }
    print("test_await_try_catch: PASS");
}

/* ---- async with conditional await ---- */
async function test_conditional_await() {
    async function f(flag) {
        var x = 1;
        if (flag) {
            x = await Promise.resolve(10);
        }
        return x + 5;
    }
    var i;
    for (i = 0; i < 200; i++) {
        var r1 = await f(true);
        var r2 = await f(false);
        assert(r1 === 15, "cond_await true: " + r1);
        assert(r2 === 6,  "cond_await false: " + r2);
    }
    print("test_conditional_await: PASS");
}

/* ---- accumulator loop with await ---- */
async function test_await_loop() {
    async function sum(n) {
        var acc = 0;
        var i = 0;
        while (i < n) {
            acc = acc + await Promise.resolve(i);
            i = i + 1;
        }
        return acc;
    }
    var i;
    for (i = 0; i < 200; i++) {
        var r = await sum(5);
        assert(r === 10, "await_loop: " + r);
    }
    print("test_await_loop: PASS");
}

/* Run all tests sequentially inside an async IIFE */
(async function() {
    await test_no_await();
    await test_single_await();
    await test_multiple_await();
    await test_await_try_catch();
    await test_conditional_await();
    await test_await_loop();
    print("P12.3 async JIT tests: ALL PASS");
})().catch(function(e) {
    print("UNHANDLED ERROR: " + e);
    throw e;
});
