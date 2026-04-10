/* JIT P17 — OP_apply and OP_apply_eval correctness tests */
function assert(v, msg) { if (!v) throw new Error("FAIL: " + (msg||"assertion")); }

/* ---- OP_apply: f(...spread) and f.apply(this, args) ---- */
function sum(...args) {
    var s = 0;
    for (var i = 0; i < args.length; i++) s += args[i];
    return s;
}

function test_spread_call() {
    var i = 0;
    while (i < 200) {
        var arr = [1, 2, 3, 4, 5];
        var r = sum(...arr);
        assert(r === 15, "spread call sum");
        i++;
    }
    print("test_spread_call: PASS");
}

function test_apply() {
    var i = 0;
    while (i < 200) {
        var arr = [10, 20, 30];
        var r = sum.apply(null, arr);
        assert(r === 60, "apply call sum");
        i++;
    }
    print("test_apply: PASS");
}

/* OP_apply magic=1: new f(...spread) */
function Point(x, y) {
    this.x = x;
    this.y = y;
}

function test_new_spread() {
    var i = 0;
    while (i < 200) {
        var args = [3, 4];
        var p = new Point(...args);
        assert(p.x === 3 && p.y === 4, "new with spread");
        i++;
    }
    print("test_new_spread: PASS");
}

/* method call with apply */
function test_method_apply() {
    var obj = {
        base: 100,
        add: function(a, b) { return this.base + a + b; }
    };
    var i = 0;
    while (i < 200) {
        var r = obj.add.apply(obj, [5, 10]);
        assert(r === 115, "method apply");
        i++;
    }
    print("test_method_apply: PASS");
}

/* empty spread */
function test_empty_spread() {
    function noargs() { return arguments.length; }
    var i = 0;
    while (i < 200) {
        var arr = [];
        var r = noargs(...arr);
        assert(r === 0, "empty spread");
        i++;
    }
    print("test_empty_spread: PASS");
}

/* Math.max via spread */
function test_math_max_spread() {
    var i = 0;
    while (i < 200) {
        var nums = [3, 1, 4, 1, 5, 9, 2, 6];
        var r = Math.max(...nums);
        assert(r === 9, "Math.max spread");
        i++;
    }
    print("test_math_max_spread: PASS");
}

/* ---- OP_apply_eval: eval(...spread) ---- */
function test_apply_eval() {
    /* eval(str) where str comes from an array — uses OP_apply_eval */
    var parts = ["1+2"];
    var r = eval(...parts);
    assert(r === 3, "eval spread");
    print("test_apply_eval: PASS");
}

test_spread_call();
test_apply();
test_new_spread();
test_method_apply();
test_empty_spread();
test_math_max_spread();
test_apply_eval();
print("P17 apply tests: ALL PASS");
