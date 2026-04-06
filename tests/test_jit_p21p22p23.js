/* test_jit_p21p22p23.js — JIT P21 (spread/rest/copy), P22 (private fields), P23 (OOP/class) */

"use strict";

function assert(cond, msg) {
    if (!cond) throw new Error("FAIL: " + msg);
}
function assertEq(a, b, msg) {
    if (a !== b) throw new Error("FAIL: " + msg + " — got " + a + " expected " + b);
}

/* ---- P21: rest ---- */
function restTest(a, b, ...rest) { return rest; }
/* warm-up */
for (let i = 0; i < 200; i++) {
    let r = restTest(1, 2, 3, 4, 5);
    assertEq(r.length, 3, "rest length");
    assertEq(r[0], 3, "rest[0]");
    assertEq(r[2], 5, "rest[2]");
}
/* empty rest */
for (let i = 0; i < 200; i++) {
    let r = restTest(1, 2);
    assertEq(r.length, 0, "rest empty");
}

/* ---- P21: spread / append (for-of into array) ---- */
function spreadTest() {
    let src = [10, 20, 30];
    return [...src, 40];
}
for (let i = 0; i < 200; i++) {
    let r = spreadTest();
    assertEq(r.length, 4, "spread length");
    assertEq(r[3], 40, "spread last");
}

/* ---- P21: copy_data_properties (object spread) ---- */
function copyProps() {
    let a = {x: 1, y: 2};
    let b = {...a, z: 3};
    return b;
}
for (let i = 0; i < 200; i++) {
    let r = copyProps();
    assertEq(r.x, 1, "copy x");
    assertEq(r.y, 2, "copy y");
    assertEq(r.z, 3, "copy z");
}

/* ---- P22: private fields (basic) ---- */
class Counter {
    #count = 0;
    inc() { this.#count++; }
    get() { return this.#count; }
}
for (let i = 0; i < 200; i++) {
    let c = new Counter();
    c.inc(); c.inc(); c.inc();
    assertEq(c.get(), 3, "private field counter");
}

/* ---- P22: private field define_private_field ---- */
class Box {
    #val;
    constructor(v) { this.#val = v; }
    get() { return this.#val; }
}
for (let i = 0; i < 200; i++) {
    let b = new Box(42 + i);
    assertEq(b.get(), 42 + i, "private field box");
}

/* ---- P22: private_in (ergonomic brand checks) ---- */
class Tagged {
    #tag = true;
    static has(obj) { return #tag in obj; }
}
for (let i = 0; i < 200; i++) {
    let t = new Tagged();
    assert(Tagged.has(t), "private_in true");
    assert(!Tagged.has({}), "private_in false");
}

/* ---- P22: private methods ---- */
class WithPrivateMethod {
    #double(x) { return x * 2; }
    compute(x) { return this.#double(x); }
}
for (let i = 0; i < 200; i++) {
    let w = new WithPrivateMethod();
    assertEq(w.compute(5), 10, "private method");
}

/* ---- P23: define_method / check_brand / add_brand (via class with methods) ---- */
class Animal {
    speak() { return "generic"; }
    greet() { return "hello from " + this.speak(); }
}
class Dog extends Animal {
    speak() { return "woof"; }
}
for (let i = 0; i < 200; i++) {
    let d = new Dog();
    assertEq(d.greet(), "hello from woof", "define_method/speak");
}

/* ---- P23: get_super / get_super_value / put_super_value ---- */
class Base {
    constructor() { this.val = 10; }
    getValue() { return this.val; }
}
class Derived extends Base {
    constructor() {
        super();
        this.val = 20;
    }
    getBase() { return super.getValue(); }
    getSuper() { return super.val; }
}
for (let i = 0; i < 200; i++) {
    let d = new Derived();
    /* get_super_value: super.getValue() dispatches via super prototype */
    assertEq(d.getBase(), 20, "get_super_value method");
}

/* ---- P23: define_method_computed ---- */
function makeMethodName(prefix) { return prefix + "Method"; }
class Computed {
    [makeMethodName("my")]() { return 99; }
}
for (let i = 0; i < 200; i++) {
    let c = new Computed();
    assertEq(c.myMethod(), 99, "define_method_computed");
}

/* ---- P23: check_ctor_return ---- */
class Ctor {
    constructor(x) {
        this.x = x;
    }
}
for (let i = 0; i < 200; i++) {
    let o = new Ctor(i);
    assertEq(o.x, i, "ctor return check");
}

/* ---- P23: getters/setters (define_method with accessor flag) ---- */
class WithAccessors {
    #n = 0;
    get value() { return this.#n; }
    set value(v) { this.#n = v; }
}
for (let i = 0; i < 200; i++) {
    let w = new WithAccessors();
    w.value = i * 2;
    assertEq(w.value, i * 2, "getter/setter");
}

/* ---- P23: static methods / brand checks ---- */
class StaticExample {
    static count = 0;
    static inc() { StaticExample.count++; }
    static get() { return StaticExample.count; }
}
for (let i = 0; i < 200; i++) StaticExample.inc();
assertEq(StaticExample.get(), 200, "static method");

print("test_jit_p21p22p23: all tests passed");
