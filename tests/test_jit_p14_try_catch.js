/* JIT P14 — try/catch/finally correctness tests */
function assert(v, msg) { if (!v) throw new Error("FAIL: " + (msg||"assertion")); }

/* ---- Basic try/catch: exception caught ---- */
function test_basic_catch() {
    var caught = 0;
    var i = 0;
    while (i < 200) {
        try {
            throw new Error("test");
        } catch(e) {
            caught++;
        }
        i++;
    }
    assert(caught === 200, "basic catch count");
    print("test_basic_catch: PASS");
}

/* ---- try/catch: no exception (normal path via OP_nip_catch) ---- */
function test_no_exception() {
    var s = 0;
    var i = 0;
    while (i < 200) {
        try {
            s += i;
        } catch(e) {
            s += 10000; /* must not run */
        }
        i++;
    }
    /* sum 0..199 = 199*200/2 = 19900 */
    assert(s === 19900, "no-exception sum");
    print("test_no_exception: PASS");
}

/* ---- Catch sees the thrown value ---- */
function test_catch_value() {
    var i = 0;
    while (i < 200) {
        var val = 0;
        try {
            throw 42;
        } catch(e) {
            val = e;
        }
        assert(val === 42, "catch value");
        i++;
    }
    print("test_catch_value: PASS");
}

/* ---- Finally runs on normal path ---- */
function test_finally_normal() {
    var fin = 0;
    var i = 0;
    while (i < 200) {
        try {
            /* nothing */
        } finally {
            fin++;
        }
        i++;
    }
    assert(fin === 200, "finally normal count");
    print("test_finally_normal: PASS");
}

/* ---- Finally runs on exception path ---- */
function test_finally_exception() {
    var fin = 0;
    var caught = 0;
    var i = 0;
    while (i < 200) {
        try {
            try {
                throw new Error("x");
            } finally {
                fin++;
            }
        } catch(e) {
            caught++;
        }
        i++;
    }
    assert(fin === 200,    "finally-exception: finally ran");
    assert(caught === 200, "finally-exception: outer catch ran");
    print("test_finally_exception: PASS");
}

/* ---- Catch + finally: normal path ---- */
function test_catch_finally_normal() {
    var fin = 0;
    var s = 0;
    var i = 0;
    while (i < 200) {
        try {
            s += i;
        } catch(e) {
            s += 10000;
        } finally {
            fin++;
        }
        i++;
    }
    assert(s === 19900, "catch-finally: normal sum");
    assert(fin === 200, "catch-finally: finally count");
    print("test_catch_finally_normal: PASS");
}

/* ---- Catch + finally: exception path ---- */
function test_catch_finally_exception() {
    var caught = 0;
    var fin = 0;
    var i = 0;
    while (i < 200) {
        try {
            throw new Error("y");
        } catch(e) {
            caught++;
        } finally {
            fin++;
        }
        i++;
    }
    assert(caught === 200, "catch-finally-exception: catch count");
    assert(fin === 200,    "catch-finally-exception: finally count");
    print("test_catch_finally_exception: PASS");
}

/* ---- Return value from try block survives nip_catch ---- */
function helper_try_return(x) {
    var r = 0;
    try {
        r = x * 2;
    } catch(e) {
        r = -1;
    }
    return r;
}
function test_try_return() {
    var i = 0;
    while (i < 200) {
        assert(helper_try_return(i) === i * 2, "try-return value");
        i++;
    }
    print("test_try_return: PASS");
}

/* ---- Nested try/catch ---- */
function test_nested_try() {
    var outer = 0;
    var inner_caught = 0;
    var i = 0;
    while (i < 200) {
        try {
            try {
                throw new Error("inner");
            } catch(e1) {
                inner_caught++;
                /* no rethrow — outer catch should NOT run */
            }
            outer++;
        } catch(e2) {
            outer += 1000; /* must not run */
        }
        i++;
    }
    assert(inner_caught === 200, "nested: inner catch count");
    assert(outer === 200,        "nested: outer body ran without outer catch");
    print("test_nested_try: PASS");
}

test_basic_catch();
test_no_exception();
test_catch_value();
test_finally_normal();
test_finally_exception();
test_catch_finally_normal();
test_catch_finally_exception();
test_try_return();
test_nested_try();
print("P14 try/catch/finally tests: ALL PASS");
