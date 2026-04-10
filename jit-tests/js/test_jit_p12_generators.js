/* JIT P12: generator function correctness tests
 * Run with enough iterations to trigger JIT compilation (threshold ~100).
 */
function assert(v, msg) {
    if (!v) throw new Error("FAIL: " + (msg || "assertion"));
}

/* ---- basic generator: single yield ---- */
function test_basic_yield() {
    function *gen(x) {
        var a = x + 1;
        yield a;
        return a + 10;
    }
    var i;
    for (i = 0; i < 200; i++) {
        var g = gen(5);
        var r1 = g.next();
        assert(r1.value === 6 && r1.done === false, "yield value");
        var r2 = g.next();
        assert(r2.value === 16 && r2.done === true, "return value");
        var r3 = g.next();
        assert(r3.value === undefined && r3.done === true, "after done");
    }
    print("test_basic_yield: PASS");
}

/* ---- multiple yields ---- */
function test_multiple_yields() {
    function *count(n) {
        var i = 0;
        while (i < n) {
            yield i;
            i = i + 1;
        }
        return n;
    }
    var j;
    for (j = 0; j < 200; j++) {
        var g = count(3);
        assert(g.next().value === 0, "count 0");
        assert(g.next().value === 1, "count 1");
        assert(g.next().value === 2, "count 2");
        var r = g.next();
        assert(r.value === 3 && r.done === true, "count done");
    }
    print("test_multiple_yields: PASS");
}

/* ---- .next(v) passes value back through yield ---- */
function test_next_value() {
    function *echo() {
        var a = yield 1;
        var b = yield 2;
        return a + b;
    }
    var i;
    for (i = 0; i < 200; i++) {
        var g = echo();
        assert(g.next().value === 1, "echo first yield");
        assert(g.next(10).value === 2, "echo second yield");
        var r = g.next(20);
        assert(r.value === 30 && r.done === true, "echo return");
    }
    print("test_next_value: PASS");
}

/* ---- generator.return(v) closes the generator early ---- */
function test_return_early() {
    function *inf() {
        var n = 0;
        while (1) {
            yield n;
            n = n + 1;
        }
    }
    var i;
    for (i = 0; i < 200; i++) {
        var g = inf();
        assert(g.next().value === 0, "inf first");
        assert(g.next().value === 1, "inf second");
        var r = g.return(99);
        assert(r.value === 99 && r.done === true, "early return");
        /* generator is done now */
        assert(g.next().done === true, "after return");
    }
    print("test_return_early: PASS");
}

/* ---- generator.throw(e) throws inside the generator ---- */
function test_throw() {
    function *safe() {
        yield 1;
        yield 2;
    }
    var i;
    for (i = 0; i < 200; i++) {
        var g = safe();
        assert(g.next().value === 1, "throw: first yield");
        var caught = false;
        try {
            g.throw(new Error("oops"));
        } catch(e) {
            caught = true;
        }
        assert(caught, "throw propagated");
        assert(g.next().done === true, "throw: done after throw");
    }
    print("test_throw: PASS");
}

/* ---- local variable preservation across yields ---- */
function test_locals_preserved() {
    function *sum_gen(n) {
        var acc = 0;
        var i = 0;
        while (i < n) {
            acc = acc + i;
            yield acc;
            i = i + 1;
        }
        return acc;
    }
    var t;
    for (t = 0; t < 200; t++) {
        var g = sum_gen(4);
        assert(g.next().value === 0,  "sum 0");
        assert(g.next().value === 1,  "sum 1");
        assert(g.next().value === 3,  "sum 3");
        assert(g.next().value === 6,  "sum 6");
        var r = g.next();
        assert(r.value === 6 && r.done === true, "sum done");
    }
    print("test_locals_preserved: PASS");
}

/* ---- generator with arguments ---- */
function test_args() {
    function *range(start, end, step) {
        var v = start;
        while (v < end) {
            yield v;
            v = v + step;
        }
    }
    var i;
    for (i = 0; i < 200; i++) {
        var g = range(0, 6, 2);
        assert(g.next().value === 0, "range 0");
        assert(g.next().value === 2, "range 2");
        assert(g.next().value === 4, "range 4");
        assert(g.next().done === true, "range done");
    }
    print("test_args: PASS");
}

/* ---- empty generator (only initial_yield + return) ---- */
function test_empty_gen() {
    function *empty() {
        /* no yields, just returns */
    }
    var i;
    for (i = 0; i < 200; i++) {
        var g = empty();
        var r = g.next();
        assert(r.value === undefined && r.done === true, "empty done");
    }
    print("test_empty_gen: PASS");
}

/* ---- generator called via spread (tests reuse across calls) ---- */
function test_multiple_instances() {
    function *fib() {
        var a = 0, b = 1;
        while (1) {
            yield a;
            var c = a + b;
            a = b;
            b = c;
        }
    }
    var i;
    for (i = 0; i < 200; i++) {
        var g = fib();
        /* consume first 8 Fibonacci numbers */
        var expected = [0, 1, 1, 2, 3, 5, 8, 13];
        var j;
        for (j = 0; j < 8; j++) {
            assert(g.next().value === expected[j], "fib " + j);
        }
    }
    print("test_multiple_instances: PASS");
}

test_basic_yield();
test_multiple_yields();
test_next_value();
test_return_early();
test_throw();
test_locals_preserved();
test_args();
test_empty_gen();
test_multiple_instances();
print("P12 generator JIT tests: ALL PASS");
