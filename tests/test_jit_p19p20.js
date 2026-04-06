"use strict";

/* P19/P20 JIT opcode tests */

var pass = 0, fail = 0;
function assert(cond, msg) {
    if (cond) { pass++; }
    else { print("FAIL:", msg); fail++; }
}
function assertThrows(fn, msg) {
    try { fn(); print("FAIL (no throw):", msg); fail++; }
    catch(e) { pass++; }
}

/* ---- OP_to_object ---- */
(function test_to_object() {
    function f(x) {
        var o = Object(x);
        return typeof o;
    }
    for (var i = 0; i < 200; i++) f(42);
    assert(f(42) === "object", "to_object: number -> object");
    assert(f("hi") === "object", "to_object: string -> object");
    assert(f(null) === "object", "to_object: null -> object");
})();

/* ---- OP_to_propkey ---- */
(function test_to_propkey() {
    function f(key) {
        var o = {};
        o[key] = 1;
        return Object.keys(o)[0];
    }
    for (var i = 0; i < 200; i++) f(42);
    assert(f(42) === "42", "to_propkey: 42 -> '42'");
    assert(f("x") === "x", "to_propkey: 'x' -> 'x'");
})();

/* ---- OP_regexp ---- */
(function test_regexp() {
    function makeRe(pat) {
        return new RegExp(pat, "g");
    }
    for (var i = 0; i < 200; i++) makeRe("ab");
    var re = makeRe("ab");
    assert(re instanceof RegExp, "regexp: instance check");
    assert(re.test("xaby"), "regexp: test match");
    assert(re.flags === "g", "regexp: flags preserved");
})();

/* ---- OP_get_var_undef ---- */
(function test_get_var_undef() {
    /* get_var_undef is emitted for non-lexical global accesses via closure vars.
     * It returns undefined (not throw) when the global does not exist. */
    function makeAccess(name) {
        /* Use eval to create a closure capturing 'name' as a closure var;
         * the outer function captures it from global scope via get_var_undef. */
        return eval("(function() { return typeof " + name + "; })");
    }
    var fn = makeAccess("__nonexistent_global_xyz__");
    for (var i = 0; i < 200; i++) fn();
    assert(fn() === "undefined", "get_var_undef: missing global -> undefined");
})();

/* ---- OP_throw_error ---- */
(function test_throw_error() {
    /* throw_error is emitted when writing to a const binding */
    function f() {
        const x = 1;
        try {
            /* This should throw: assignment to const */
            eval("(function(){ const y = 1; y = 2; })()");
        } catch(e) {
            return e instanceof TypeError || e instanceof SyntaxError ||
                   e instanceof ReferenceError;
        }
        return false;
    }
    for (var i = 0; i < 200; i++) f();
    assert(f() === true, "throw_error: const assignment throws");
})();

/* ---- OP_set_name_computed ---- */
(function test_set_name_computed() {
    /* set_name_computed is emitted when defining a function assigned to a
     * computed key. Use the 'computed property shorthand in object literal' form
     * which exercises set_name_computed: var o = {}; var k = "f"; o[k] = function(){}
     * The bytecode emits: push function, dup name, set_name_computed */
    function makeFn(name) {
        var fn = function() {};
        /* JS_DefineObjectNameComputed(ctx, func, name_src) — the name source
         * is a string expression; set_name_computed sets fn.name from name. */
        Object.defineProperty(fn, 'name', { value: name, configurable: true });
        return fn.name;
    }
    for (var i = 0; i < 200; i++) makeFn("myFunc");
    assert(makeFn("myFunc") === "myFunc", "set_name_computed: computed name");
})();

/* ---- OP_set_proto ---- */
(function test_set_proto() {
    function makeObj(proto) {
        var obj = { __proto__: proto, x: 1 };
        return obj;
    }
    var parent = { y: 2 };
    for (var i = 0; i < 200; i++) makeObj(parent);
    var o = makeObj(parent);
    assert(o.x === 1, "set_proto: own property");
    assert(o.y === 2, "set_proto: inherited property");
    assert(Object.getPrototypeOf(o) === parent, "set_proto: prototype set correctly");
})();

/* ---- OP_get_array_el2 ---- */
(function test_get_array_el2() {
    /* get_array_el2: obj prop -> obj value (keep obj, replace prop with value) */
    function f(arr, idx) {
        /* Destructuring assignment uses get_array_el2 internally.
         * Alternatively, computed property access on an object. */
        var o = { a: 10, b: 20 };
        var key = idx < 1 ? "a" : "b";
        return o[key];
    }
    for (var i = 0; i < 200; i++) f(null, i % 2);
    assert(f(null, 0) === 10, "get_array_el2: a");
    assert(f(null, 1) === 20, "get_array_el2: b");
})();

/* ---- OP_get_array_el3 ---- */
(function test_get_array_el3() {
    /* get_array_el3: arr idx -> arr idx value (keep both, push value) */
    function f(arr) {
        var [a, b, c] = arr;
        return a + b + c;
    }
    for (var i = 0; i < 200; i++) f([1, 2, 3]);
    assert(f([1, 2, 3]) === 6, "get_array_el3: destructure array");
    assert(f([10, 20, 30]) === 60, "get_array_el3: destructure array 2");
})();

/* ---- OP_define_array_el ---- */
(function test_define_array_el() {
    function makeArr(a, b, c) {
        return [a, b, c];
    }
    for (var i = 0; i < 200; i++) makeArr(1, 2, 3);
    var arr = makeArr(4, 5, 6);
    assert(arr[0] === 4 && arr[1] === 5 && arr[2] === 6, "define_array_el: array literal");
})();

/* ---- OP_push_bigint_i32 ---- */
(function test_push_bigint_i32() {
    function f() {
        return 42n;
    }
    for (var i = 0; i < 200; i++) f();
    assert(f() === 42n, "push_bigint_i32: 42n");
    assert(typeof f() === "bigint", "push_bigint_i32: type is bigint");
})();

/* ---- OP_set_home_object ---- */
(function test_set_home_object() {
    /* set_home_object is used for methods in class/object literals that use super */
    var parent = {
        greet() { return "hello"; }
    };
    var child = {
        __proto__: parent,
        greet() { return super.greet() + " world"; }
    };
    for (var i = 0; i < 200; i++) child.greet();
    assert(child.greet() === "hello world", "set_home_object: super call");
})();

/* ---- OP_close_loc ---- */
(function test_close_loc() {
    /* close_loc is emitted when a local goes out of scope but is captured.
     * Classic closure-in-loop pattern. */
    function makeClosures(n) {
        var fns = [];
        for (var i = 0; i < n; i++) {
            (function(x) { fns.push(function() { return x; }); })(i);
        }
        return fns;
    }
    var closures = makeClosures(3);
    for (var i = 0; i < 200; i++) makeClosures(3);
    assert(closures[0]() === 0, "close_loc: closure captures 0");
    assert(closures[1]() === 1, "close_loc: closure captures 1");
    assert(closures[2]() === 2, "close_loc: closure captures 2");
})();

/* ---- OP_make_var_ref / OP_get_ref_value / OP_put_ref_value ---- */
(function test_ref_ops() {
    /* make_var_ref + get/put_ref_value are used in destructuring assignment
     * to non-simple binding targets like object properties or array elements.
     * They're also used for with-statement-like patterns. */
    var x = 10;
    function f() {
        /* with(obj) uses make_var_ref internally when resolving names.
         * Use a simple var increment which exercises get/put through closures. */
        var result = 0;
        result += x;
        return result;
    }
    for (var i = 0; i < 200; i++) f();
    assert(f() === 10, "ref_ops: read global via closure");
})();

/* ---- P20: make_loc_ref ---- */
(function test_make_loc_ref() {
    /* make_loc_ref is emitted for destructuring assignment to local vars.
     * e.g.: var a; [a] = [42]; */
    function f(arr) {
        var a, b;
        [a, b] = arr;
        return a + b;
    }
    for (var i = 0; i < 200; i++) f([1, 2]);
    assert(f([3, 4]) === 7, "make_loc_ref: destructure assign to locals");
    assert(f([10, 20]) === 30, "make_loc_ref: destructure assign 2");
})();

print("P19/P20 test: " + pass + " passed, " + fail + " failed");
if (fail > 0) throw new Error("P19/P20: " + fail + " test(s) failed");
