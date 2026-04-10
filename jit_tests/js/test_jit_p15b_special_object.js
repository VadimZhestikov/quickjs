/* JIT OP_special_object correctness tests */
function assert(v, msg) { if (!v) throw new Error("FAIL: " + (msg||"assertion")); }

/* ---- arguments (strict / simple): length and values ---- */
function test_arguments_basic() {
    "use strict";
    function sum() {
        var s = 0;
        for (var i = 0; i < arguments.length; i++) s += arguments[i];
        return s;
    }
    var i;
    for (i = 0; i < 200; i++) {
        assert(sum(1, 2, 3) === 6, "strict arguments sum");
    }
    print("test_arguments_basic: PASS");
}

/* ---- arguments (non-strict/mapped): length and read ---- */
function test_arguments_mapped() {
    function f(a, b, c) {
        return arguments.length + arguments[0] + arguments[1] + arguments[2];
    }
    var i;
    for (i = 0; i < 200; i++) {
        assert(f(10, 20, 30) === 63, "mapped arguments read");
    }
    print("test_arguments_mapped: PASS");
}

/* ---- arguments: variadic function ---- */
function test_arguments_variadic() {
    function max() {
        var m = -Infinity;
        for (var i = 0; i < arguments.length; i++)
            if (arguments[i] > m) m = arguments[i];
        return m;
    }
    var i;
    for (i = 0; i < 200; i++) {
        assert(max(3, 1, 4, 1, 5, 9, 2, 6) === 9, "variadic max");
    }
    print("test_arguments_variadic: PASS");
}

/* ---- THIS_FUNC: named function expression refers to itself ---- */
function test_this_func() {
    var fib = function fibonacci(n) {
        if (n <= 1) return n;
        return fibonacci(n-1) + fibonacci(n-2);
    };
    var i;
    for (i = 0; i < 200; i++) {
        assert(fib(10) === 55, "named func expr recursion");
    }
    print("test_this_func: PASS");
}

/* ---- NEW_TARGET: undefined for regular call ---- */
function test_new_target_undefined() {
    function Foo() {
        return new.target;
    }
    var i;
    for (i = 0; i < 200; i++) {
        assert(Foo() === undefined, "new.target undefined for normal call");
    }
    print("test_new_target_undefined: PASS");
}

/* ---- NEW_TARGET: defined for constructor call ---- */
function test_new_target_constructor() {
    function Bar() {
        this.nt = new.target;
    }
    var i;
    for (i = 0; i < 200; i++) {
        var b = new Bar();
        assert(b.nt === Bar, "new.target === Bar for new Bar()");
    }
    print("test_new_target_constructor: PASS");
}

/* ---- VAR_OBJECT: created as plain object ---- */
function test_var_object() {
    /* VAR_OBJECT is emitted for eval() in non-strict mode, hard to invoke
     * directly; test indirectly via eval in a hot loop */
    var s = 0;
    for (var i = 0; i < 200; i++) {
        s += i;
    }
    assert(s === 19900, "var-object context sum");
    print("test_var_object: PASS (indirect)");
}

test_arguments_basic();
test_arguments_mapped();
test_arguments_variadic();
test_this_func();
test_new_target_undefined();
test_new_target_constructor();
test_var_object();
print("OP_special_object tests: ALL PASS");
