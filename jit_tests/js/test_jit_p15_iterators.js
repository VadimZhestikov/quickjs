/* JIT P15 — for-in / for-of / iterator correctness tests */
function assert(v, msg) { if (!v) throw new Error("FAIL: " + (msg||"assertion")); }

/* ---- for-in: basic key enumeration ---- */
function test_for_in_basic() {
    var obj = {a:1, b:2, c:3};
    var keys = [];
    for (var k in obj) {
        keys.push(k);
    }
    keys.sort();
    assert(keys.length === 3,         "for-in: key count");
    assert(keys[0] === "a",           "for-in: key a");
    assert(keys[1] === "b",           "for-in: key b");
    assert(keys[2] === "c",           "for-in: key c");
    print("test_for_in_basic: PASS");
}

/* ---- for-in: sum of values ---- */
function test_for_in_sum() {
    var obj = {x:10, y:20, z:30};
    var s = 0;
    for (var k in obj) {
        s += obj[k];
    }
    assert(s === 60, "for-in sum");
    print("test_for_in_sum: PASS");
}

/* ---- for-in: empty object ---- */
function test_for_in_empty() {
    var count = 0;
    for (var k in {}) {
        count++;
    }
    assert(count === 0, "for-in empty");
    print("test_for_in_empty: PASS");
}

/* ---- for-in: hot loop (many iterations) ---- */
function test_for_in_hot() {
    var obj = {};
    for (var i = 0; i < 200; i++) obj["k"+i] = i;
    var s = 0;
    for (var k in obj) {
        s += obj[k];
    }
    /* sum 0..199 = 19900 */
    assert(s === 19900, "for-in hot sum");
    print("test_for_in_hot: PASS");
}

/* ---- for-of: basic array iteration ---- */
function test_for_of_basic() {
    var arr = [10, 20, 30];
    var s = 0;
    for (var v of arr) {
        s += v;
    }
    assert(s === 60, "for-of sum");
    print("test_for_of_basic: PASS");
}

/* ---- for-of: empty array ---- */
function test_for_of_empty() {
    var count = 0;
    for (var v of []) {
        count++;
    }
    assert(count === 0, "for-of empty");
    print("test_for_of_empty: PASS");
}

/* ---- for-of: string iteration ---- */
function test_for_of_string() {
    var chars = [];
    for (var c of "hello") {
        chars.push(c);
    }
    assert(chars.length === 5, "for-of string length");
    assert(chars.join("") === "hello", "for-of string value");
    print("test_for_of_string: PASS");
}

/* ---- for-of: hot loop ---- */
function test_for_of_hot() {
    var arr = [];
    for (var i = 0; i < 200; i++) arr.push(i);
    var s = 0;
    for (var v of arr) {
        s += v;
    }
    assert(s === 19900, "for-of hot sum");
    print("test_for_of_hot: PASS");
}

/* ---- for-of: break exits iterator cleanly ---- */
function test_for_of_break() {
    var arr = [1, 2, 3, 4, 5];
    var s = 0;
    for (var v of arr) {
        s += v;
        if (v === 3) break;
    }
    assert(s === 6, "for-of break sum");
    print("test_for_of_break: PASS");
}

/* ---- for-in nested with for-of ---- */
function test_nested_for() {
    var obj = {a:[1,2], b:[3,4]};
    var s = 0;
    for (var k in obj) {
        for (var v of obj[k]) {
            s += v;
        }
    }
    assert(s === 10, "nested for-in/of");
    print("test_nested_for: PASS");
}

/* ---- for-of: many short loops (allocation pressure) ---- */
function test_for_of_many_loops() {
    var s = 0;
    for (var i = 0; i < 200; i++) {
        for (var v of [1, 2, 3]) {
            s += v;
        }
    }
    assert(s === 1200, "for-of many loops");
    print("test_for_of_many_loops: PASS");
}

test_for_in_basic();
test_for_in_sum();
test_for_in_empty();
test_for_in_hot();
test_for_of_basic();
test_for_of_empty();
test_for_of_string();
test_for_of_hot();
test_for_of_break();
test_nested_for();
test_for_of_many_loops();
print("P15 iterator tests: ALL PASS");
