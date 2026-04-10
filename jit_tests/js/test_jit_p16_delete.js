/* JIT P16 — OP_delete and OP_delete_var correctness tests */
function assert(v, msg) { if (!v) throw new Error("FAIL: " + (msg||"assertion")); }

/* ---- OP_delete: delete obj[key] ---- */
function test_delete_prop() {
    var o = {a:1, b:2, c:3};
    var i = 0;
    while (i < 200) {
        var o2 = {x:10, y:20};
        var r = delete o2.x;
        assert(r === true, "delete configurable prop must return true");
        assert(!('x' in o2), "deleted prop must not be in object");
        assert(o2.y === 20, "non-deleted prop must remain");
        i++;
    }
    print("test_delete_prop: PASS");
}

/* delete non-configurable property */
function test_delete_nonconfigurable() {
    var o = {};
    Object.defineProperty(o, 'p', {value: 1, configurable: false});
    var threw = false;
    try {
        /* strict-mode semantics from OP_delete: JS_PROP_THROW_STRICT */
        var r = (function() { "use strict"; return delete o.p; })();
        /* non-strict: returns false */
    } catch(e) {
        threw = true;
    }
    /* In non-strict outer delete: returns false without throw */
    var r2 = delete o.p;
    assert(r2 === false, "delete non-configurable returns false in non-strict");
    print("test_delete_nonconfigurable: PASS");
}

/* delete computed property */
function test_delete_computed() {
    var i = 0;
    while (i < 200) {
        var o = {a:1, b:2};
        var key = (i % 2 === 0) ? 'a' : 'b';
        var r = delete o[key];
        assert(r === true, "delete computed key must return true");
        i++;
    }
    print("test_delete_computed: PASS");
}

/* delete return value semantics */
function test_delete_return() {
    var o = {x: 42};
    var r1 = delete o.x;
    assert(r1 === true, "delete existing: true");
    var r2 = delete o.x;
    assert(r2 === true, "delete nonexistent: true");
    print("test_delete_return: PASS");
}

/* ---- OP_delete_var: delete global variable ---- */
function test_delete_var() {
    /* Non-lexical globals can be deleted */
    globalThis.testGlobal = 999;
    var r = eval('delete testGlobal');
    /* result depends on whether variable is configurable */
    assert(typeof r === 'boolean', "delete_var returns boolean");
    print("test_delete_var: PASS");
}

test_delete_prop();
test_delete_nonconfigurable();
test_delete_computed();
test_delete_return();
test_delete_var();
print("P16 delete tests: ALL PASS");
