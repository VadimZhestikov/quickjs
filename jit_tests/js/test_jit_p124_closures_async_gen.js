/* JIT P12.2 + P12.4: generator/async with closures; async generators
 * P12.2 tests: generator functions that create closures capturing local vars
 * P12.4 tests: async generator functions (async function*)
 */
function assert(v, msg) {
    if (!v) throw new Error("FAIL: " + (msg || "assertion"));
}

/* ================================================================
 * P12.2 — generator + closure tests
 * Closures created inside a generator body capture locals that must
 * survive across yield boundaries.
 * ================================================================ */

/* Generator yields a closure that captures a local variable.
 * The closure reflects the CURRENT value of the captured variable (shared upvalue). */
function test_gen_yields_closure() {
    function *make_adder(start) {
        var n = start;
        while (1) {
            var adder = function(x) { return n + x; };
            yield adder;
            n = n + 1;
        }
    }
    var i;
    for (i = 0; i < 200; i++) {
        var g = make_adder(10);
        var fn0 = g.next().value;  /* generator suspended at yield with n=10 */
        /* fn0 captures n by reference; n=10 right now */
        assert(fn0(5) === 15, "fn0 before resume: " + fn0(5));
        var fn1 = g.next().value;  /* resumes; n becomes 11, yields new adder */
        /* Both fn0 and fn1 now see n=11 */
        assert(fn0(5) === 16, "fn0 after 1st resume: " + fn0(5));
        assert(fn1(5) === 16, "fn1 after 1st resume: " + fn1(5));
        var fn2 = g.next().value;  /* n=12 */
        assert(fn0(0) === 12, "shared n=12: " + fn0(0));
        assert(fn1(0) === 12, "fn1 shared n: " + fn1(0));
        assert(fn2(0) === 12, "fn2 shared n: " + fn2(0));
    }
    print("test_gen_yields_closure: PASS");
}

/* Generator captures a local, yields it multiple times, and the closure
 * created after resume sees the updated value. */
function test_gen_closure_post_yield() {
    function *counter() {
        var count = 0;
        yield count;           /* yield 0 */
        count = count + 100;
        yield count;           /* yield 100 */
        var snap = function() { return count; };
        yield snap;            /* yield the closure */
    }
    var i;
    for (i = 0; i < 200; i++) {
        var g = counter();
        assert(g.next().value === 0,   "counter 0");
        assert(g.next().value === 100, "counter 100");
        var fn = g.next().value;
        assert(fn() === 100, "snap: " + fn());
    }
    print("test_gen_closure_post_yield: PASS");
}

/* Closure captures an argument (not a local variable). */
function test_gen_captures_arg() {
    function *wrap(base) {
        yield function(x) { return base + x; };
        base = base * 2;
        yield function(x) { return base + x; };
    }
    var i;
    for (i = 0; i < 200; i++) {
        var g = wrap(5);
        var f1 = g.next().value;
        var f2 = g.next().value;
        /* Both closures share 'base'; after second yield base=10 */
        assert(f1(1) === 11, "f1: " + f1(1));
        assert(f2(1) === 11, "f2: " + f2(1));
    }
    print("test_gen_captures_arg: PASS");
}

/* Multiple closures over different locals */
function test_gen_multi_closure() {
    function *pair(a, b) {
        var sum = a + b;
        yield { get_a: function() { return a; },
                get_b: function() { return b; },
                get_sum: function() { return sum; } };
        sum = sum + 1;
        yield function() { return sum; };
    }
    var i;
    for (i = 0; i < 200; i++) {
        var g = pair(3, 7);
        var obj = g.next().value;
        assert(obj.get_a() === 3,   "a");
        assert(obj.get_b() === 7,   "b");
        assert(obj.get_sum() === 10, "sum");
        var fn = g.next().value;
        assert(fn() === 11, "sum+1: " + fn());
    }
    print("test_gen_multi_closure: PASS");
}

/* ================================================================
 * P12.4 — async generator tests (async function*)
 * ================================================================ */

/* Collect all values from an async generator into an array */
async function collect(ag) {
    var result = [];
    var r;
    while (!(r = await ag.next()).done) {
        result.push(r.value);
    }
    return result;
}

/* Basic async generator: yields integers */
async function test_async_gen_basic() {
    async function *range(n) {
        var i = 0;
        while (i < n) {
            yield i;
            i = i + 1;
        }
    }
    var i;
    for (i = 0; i < 200; i++) {
        var vals = await collect(range(4));
        assert(vals.length === 4, "range len");
        assert(vals[0] === 0, "r0");
        assert(vals[1] === 1, "r1");
        assert(vals[2] === 2, "r2");
        assert(vals[3] === 3, "r3");
    }
    print("test_async_gen_basic: PASS");
}

/* Async generator with await inside */
async function test_async_gen_with_await() {
    async function *delayed(n) {
        var i = 0;
        while (i < n) {
            var v = await Promise.resolve(i * 2);
            yield v;
            i = i + 1;
        }
    }
    var i;
    for (i = 0; i < 200; i++) {
        var vals = await collect(delayed(3));
        assert(vals[0] === 0, "d0");
        assert(vals[1] === 2, "d2");
        assert(vals[2] === 4, "d4");
    }
    print("test_async_gen_with_await: PASS");
}

/* Async generator with try/catch inside */
async function test_async_gen_try_catch() {
    async function *safe_gen() {
        try {
            yield await Promise.resolve(1);
            yield await Promise.reject(new Error("oops"));
            yield 99;
        } catch(e) {
            yield "caught:" + e.message;
        }
    }
    var i;
    for (i = 0; i < 200; i++) {
        var g = safe_gen();
        var r1 = await g.next();
        assert(r1.value === 1, "sg1");
        var r2 = await g.next();
        assert(r2.value === "caught:oops", "sg2: " + r2.value);
        var r3 = await g.next();
        assert(r3.done === true, "sg done");
    }
    print("test_async_gen_try_catch: PASS");
}

/* Run P12.2 tests synchronously (generators are synchronous) */
test_gen_yields_closure();
test_gen_closure_post_yield();
test_gen_captures_arg();
test_gen_multi_closure();

/* Run P12.4 tests inside async IIFE */
(async function() {
    await test_async_gen_basic();
    await test_async_gen_with_await();
    await test_async_gen_try_catch();
    print("P12.4 async generator JIT tests: ALL PASS");
})().catch(function(e) {
    print("UNHANDLED ERROR: " + e);
    throw e;
});
