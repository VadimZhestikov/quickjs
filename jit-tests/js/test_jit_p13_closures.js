/* JIT P13 — OP_fclosure and OP_fclosure8 correctness tests */
function assert(v, msg) { if (!v) throw new Error("FAIL: " + (msg||"assertion")); }

/* ---- Basic counter closure: local captured and mutated ---- */
function test_counter_closure() {
    function makeCounter() {
        var n = 0;
        return function() { return n++; };
    }
    var c = makeCounter();
    var i = 0;
    while (i < 200) {
        var r0 = c(); /* 0, 1, 2, ... */
        var r1 = c();
        var r2 = c();
        assert(r1 === r0 + 1, "counter inc 1");
        assert(r2 === r0 + 2, "counter inc 2");
        i++;
    }
    /* reset */
    var d = makeCounter();
    assert(d() === 0 && d() === 1 && d() === 2, "counter fresh");
    print("test_counter_closure: PASS");
}

/* ---- Multiple closures sharing the same captured local ---- */
function test_shared_closure() {
    function shared() {
        var x = 0;
        var inc = function() { x++; };
        var get = function() { return x; };
        return { inc: inc, get: get };
    }
    var i = 0;
    while (i < 200) {
        var obj = shared();
        obj.inc(); obj.inc(); obj.inc();
        assert(obj.get() === 3, "shared var via multiple closures");
        i++;
    }
    print("test_shared_closure: PASS");
}

/* ---- Argument capture ---- */
function test_arg_capture() {
    function makeAdder(x) {
        return function(y) { return x + y; };
    }
    var add5 = makeAdder(5);
    var add10 = makeAdder(10);
    var i = 0;
    while (i < 200) {
        assert(add5(3) === 8,  "arg capture add5");
        assert(add10(3) === 13, "arg capture add10");
        i++;
    }
    print("test_arg_capture: PASS");
}

/* ---- Captured local written before closure creation ---- */
function test_written_before_closure() {
    function f() {
        var x = 10;
        x = 20;
        var g = function() { return x; };
        x = 30;
        return g;
    }
    var i = 0;
    while (i < 200) {
        var g = f();
        assert(g() === 30, "written-before-closure: sees post-mutation value");
        i++;
    }
    print("test_written_before_closure: PASS");
}

/* ---- Closure captures both a local and an arg ---- */
function test_local_and_arg_capture() {
    function outer(base) {
        var offset = 100;
        return function(x) { return base + offset + x; };
    }
    var fn = outer(1);
    var i = 0;
    while (i < 200) {
        assert(fn(0) === 101, "local+arg capture");
        assert(fn(5) === 106, "local+arg capture with delta");
        i++;
    }
    print("test_local_and_arg_capture: PASS");
}

/* ---- Closure returned from JIT function, then called many times ---- */
function test_hot_inner_closure() {
    function outer(n) {
        var acc = 0;
        return function() { acc += n; return acc; };
    }
    var addn = outer(7);
    var i = 0;
    var s = 0;
    while (i < 200) {
        s = addn();
        i++;
    }
    assert(s === 200 * 7, "hot inner closure accumulated sum");
    print("test_hot_inner_closure: PASS");
}

/* ---- Two independent outer functions, each returning closures ---- */
function test_two_independent_closures() {
    function mkA(x) { return function() { return x * 2; }; }
    function mkB(x) { return function() { return x * 3; }; }
    var a = mkA(10);
    var b = mkB(10);
    var i = 0;
    while (i < 200) {
        assert(a() === 20, "closure A");
        assert(b() === 30, "closure B");
        i++;
    }
    print("test_two_independent_closures: PASS");
}

/* ---- REF pass-through: outer closure variable passed to inner closure ---- */
function test_ref_passthrough() {
    function makeOuter() {
        var state = 0;
        function inner() { state++; }
        function getter() { return state; }
        /* wrapper creates a closure that captures inner and getter via REF */
        function wrapper() {
            inner(); inner();
            return getter();
        }
        return wrapper;
    }
    var w = makeOuter();
    var i = 0;
    while (i < 200) {
        var r = w();
        i++;
    }
    /* Each call to w() calls inner() twice, so state = 200*2 = 400.
     * Final w() adds 2 more (inner() twice) -> state = 402. */
    assert(w() === 402, "REF passthrough state");
    print("test_ref_passthrough: PASS");
}

test_counter_closure();
test_shared_closure();
test_arg_capture();
test_written_before_closure();
test_local_and_arg_capture();
test_hot_inner_closure();
test_two_independent_closures();
test_ref_passthrough();
print("P13 closure tests: ALL PASS");
