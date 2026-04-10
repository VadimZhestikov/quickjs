"use strict";
/* test_jit_p31p32.js — JIT P31/P32
 *   P31: need_home_object — class methods using super.prop / super.method()
 *   P32: is_derived_ctor  — derived class constructors (super() call, new_target)
 */

function assert(cond, msg) {
    if (!cond) throw new Error("FAIL: " + msg);
}
function assertEq(a, b, msg) {
    if (a !== b) throw new Error("FAIL: " + msg + " — got " + JSON.stringify(a) + " expected " + JSON.stringify(b));
}

/* ============================================================
 * P31 — need_home_object: super.method() in class methods
 * ============================================================ */

class Animal {
    constructor(name) { this.name = name; }
    speak() { return this.name + " makes a sound"; }
    toString() { return "Animal(" + this.name + ")"; }
}

class Dog extends Animal {
    speak() {
        return super.speak() + " (woof)";  /* uses home_object for super */
    }
    toString() {
        return "Dog<" + super.toString() + ">";  /* super in toString */
    }
}

for (let i = 0; i < 200; i++) {
    let d = new Dog("Rex");
    assertEq(d.speak(), "Rex makes a sound (woof)", "super.method() basic");
    assertEq(d.toString(), "Dog<Animal(Rex)>", "super.toString()");
}

/* super.prop (property access, not method call) */
class Base {
    get info() { return "base-info"; }
}
class Derived extends Base {
    get info() { return super.info + "-derived"; }
}
for (let i = 0; i < 200; i++) {
    let d = new Derived();
    assertEq(d.info, "base-info-derived", "super.prop getter");
}

/* Method with super + arguments */
class Shape {
    area(scale) { return this.w * this.h * scale; }
}
class Square extends Shape {
    constructor(size) { super(); this.w = size; this.h = size; }
    area(scale) {
        let base = super.area(scale);  /* super.method with args */
        return base;
    }
}
for (let i = 0; i < 200; i++) {
    let s = new Square(4);
    assertEq(s.area(2), 32, "super.method with args");
}

/* Chain: 3-level inheritance with super at each level */
class A {
    greet(name) { return "Hello " + name; }
}
class B extends A {
    greet(name) { return super.greet(name) + " from B"; }
}
class C extends B {
    greet(name) { return super.greet(name) + " from C"; }
}
for (let i = 0; i < 200; i++) {
    let c = new C();
    assertEq(c.greet("World"), "Hello World from B from C", "3-level super chain");
}

/* super in static method */
class StaticBase {
    static create(v) { return { type: "base", v }; }
}
class StaticDerived extends StaticBase {
    static create(v) {
        let obj = super.create(v);
        obj.type = "derived";
        return obj;
    }
}
for (let i = 0; i < 200; i++) {
    let o = StaticDerived.create(i);
    assertEq(o.type, "derived", "super in static method");
    assertEq(o.v, i, "super static value");
}

/* super in method assigned to variable (home_object must survive) */
class Counter {
    constructor(n) { this.n = n; }
    inc() { return this.n + 1; }
}
class DoubleCounter extends Counter {
    inc() { return super.inc() * 2; }
}
for (let i = 0; i < 200; i++) {
    let dc = new DoubleCounter(i);
    assertEq(dc.inc(), (i + 1) * 2, "super in subclass method");
}

/* ============================================================
 * P32 — is_derived_ctor: derived class constructors
 * ============================================================ */

/* Basic derived ctor: super() call, 'this' initialized properly */
class Vehicle {
    constructor(make, model) {
        this.make = make;
        this.model = model;
    }
    describe() { return this.make + " " + this.model; }
}
class Car extends Vehicle {
    constructor(make, model, year) {
        super(make, model);      /* OP_call_constructor with new_target=Car */
        this.year = year;
    }
    describe() { return this.year + " " + super.describe(); }
}
for (let i = 0; i < 200; i++) {
    let c = new Car("Toyota", "Camry", 2020 + (i % 5));
    assertEq(c.make, "Toyota", "derived ctor this.make");
    assertEq(c.model, "Camry", "derived ctor this.model");
    assertEq(c.year, 2020 + (i % 5), "derived ctor this.year");
    assertEq(c.describe(), (2020 + (i%5)) + " Toyota Camry", "derived ctor describe");
}

/* instanceof chain preserved with new_target */
for (let i = 0; i < 200; i++) {
    let c = new Car("Honda", "Civic", 2021);
    assert(c instanceof Car, "instanceof Car");
    assert(c instanceof Vehicle, "instanceof Vehicle");
}

/* Multi-level derived ctor */
class Person {
    constructor(name) { this.name = name; }
}
class Employee extends Person {
    constructor(name, company) {
        super(name);
        this.company = company;
    }
}
class Manager extends Employee {
    constructor(name, company, reports) {
        super(name, company);
        this.reports = reports;
    }
    summary() { return this.name + "@" + this.company + "(" + this.reports + ")"; }
}
for (let i = 0; i < 200; i++) {
    let m = new Manager("Alice", "Acme", i + 1);
    assertEq(m.name, "Alice", "3-level derived ctor name");
    assertEq(m.company, "Acme", "3-level derived ctor company");
    assertEq(m.reports, i + 1, "3-level derived ctor reports");
    assertEq(m.summary(), "Alice@Acme(" + (i+1) + ")", "3-level derived summary");
    assert(m instanceof Manager, "instanceof Manager");
    assert(m instanceof Employee, "instanceof Employee");
    assert(m instanceof Person, "instanceof Person");
}

/* Derived ctor with default args */
class Widget {
    constructor(id) { this.id = id; }
}
class Button extends Widget {
    constructor(id, label = "Click") {
        super(id);
        this.label = label;
    }
}
/* Note: default args means has_simple_params=false, so this runs interpreted.
 * But it must still work correctly. */
for (let i = 0; i < 200; i++) {
    let b1 = new Button(i);
    let b2 = new Button(i, "Submit");
    assertEq(b1.id, i, "derived ctor default label id");
    assertEq(b1.label, "Click", "derived ctor default label");
    assertEq(b2.label, "Submit", "derived ctor explicit label");
}

/* Derived ctor: return value semantics (object → use it; undefined → use this) */
class Singleton {
    constructor() { this.type = "singleton"; }
}
class SingletonFactory extends Singleton {
    constructor() {
        super();
        this.factory = true;
    }
}
for (let i = 0; i < 200; i++) {
    let sf = new SingletonFactory();
    assert(sf instanceof SingletonFactory, "factory instanceof");
    assert(sf instanceof Singleton, "factory parent instanceof");
    assertEq(sf.factory, true, "factory field");
    assertEq(sf.type, "singleton", "parent field");
}

/* new.target inside derived ctor */
class NewTargetBase {
    constructor() { this.targetName = new.target.name; }
}
class NewTargetDerived extends NewTargetBase {
    constructor() { super(); }
}
for (let i = 0; i < 200; i++) {
    let obj = new NewTargetDerived();
    assertEq(obj.targetName, "NewTargetDerived", "new.target.name in derived ctor");
}

/* ============================================================
 * P31+P32 combined: derived class with super in both ctor and method
 * ============================================================ */

class Animal2 {
    constructor(name, sound) {
        this.name = name;
        this.sound = sound;
    }
    speak() { return this.name + " says " + this.sound; }
}
class Dog2 extends Animal2 {
    constructor(name) {
        super(name, "woof");   /* P32: derived ctor super() */
        this.tricks = [];
    }
    learn(trick) { this.tricks.push(trick); }
    perform() {
        return super.speak() + " and does " + this.tricks.join(", ");  /* P31: super.method */
    }
}
for (let i = 0; i < 200; i++) {
    let d = new Dog2("Buddy");
    d.learn("sit");
    d.learn("stay");
    assertEq(d.speak(), "Buddy says woof", "combined speak");
    assertEq(d.perform(), "Buddy says woof and does sit, stay", "combined perform");
    assert(d instanceof Dog2, "combined instanceof Dog2");
    assert(d instanceof Animal2, "combined instanceof Animal2");
}

print("test_jit_p31p32: all tests passed");
