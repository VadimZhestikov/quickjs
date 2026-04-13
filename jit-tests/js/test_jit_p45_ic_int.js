/* P45 — IC val_tag: INT-valued property optimisation
 * Tests that get_field on INT/float64/string properties produces correct
 * results when the JIT IC hit path skips JS_DupValue for INT-tagged values.
 * val_tag is recorded in JSJITICEntry at IC fill time; the IC hit path
 * avoids unnecessary DupValue for INT properties (which have no refcount).
 */

/* ---- Basic INT property access ---- */
function sum_int_prop(n) {
    var o = {x: 7};
    var s = 0;
    for (var i = 0; i < n; i++) s += o.x;
    return s;
}
var r1 = sum_int_prop(100);
if (r1 !== 700) throw new Error("sum_int_prop: " + r1);

/* ---- Float64 property: must not be treated as INT ---- */
function sum_float_prop(n) {
    var o = {x: 1.5};
    var s = 0;
    for (var i = 0; i < n; i++) s += o.x;
    return s;
}
var r2 = sum_float_prop(100);
if (Math.abs(r2 - 150.0) > 1e-9) throw new Error("sum_float_prop: " + r2);

/* ---- String property: must not be treated as INT ---- */
function concat_str_prop(n) {
    var o = {name: "hi"};
    var s = "";
    for (var i = 0; i < n; i++) s += o.name;
    return s;
}
var r3 = concat_str_prop(5);
if (r3 !== "hihihihihi") throw new Error("concat_str_prop: " + r3);

/* ---- Property that changes type: INT → float ---- */
function prop_type_change() {
    var o = {v: 1};
    var s1 = 0;
    for (var i = 0; i < 200; i++) s1 += o.v;  /* IC fills with INT */
    o.v = 2.5;                                  /* change to float */
    var s2 = 0;
    for (var i = 0; i < 10; i++) s2 += o.v;   /* must still be correct */
    return [s1, s2];
}
var r4 = prop_type_change();
if (r4[0] !== 200) throw new Error("prop_type_change s1: " + r4[0]);
if (Math.abs(r4[1] - 25.0) > 1e-9) throw new Error("prop_type_change s2: " + r4[1]);

/* ---- Multiple INT properties ---- */
function multi_int_prop(n) {
    var o = {a: 3, b: 5, c: 11};
    var s = 0;
    for (var i = 0; i < n; i++) s += o.a + o.b + o.c;
    return s;
}
var r5 = multi_int_prop(100);
if (r5 !== 1900) throw new Error("multi_int_prop: " + r5);

/* ---- INT property in arithmetic expression ---- */
function arith_int_prop(n) {
    var o = {base: 10, step: 3};
    var v = o.base;
    for (var i = 0; i < n; i++) v += o.step;
    return v;
}
var r6 = arith_int_prop(100);
if (r6 !== 310) throw new Error("arith_int_prop: " + r6);

/* ---- Zero INT property ---- */
function zero_int_prop(n) {
    var o = {z: 0};
    var s = 0;
    for (var i = 0; i < n; i++) s += o.z + i;
    return s;
}
var r7 = zero_int_prop(10);
if (r7 !== 45) throw new Error("zero_int_prop: " + r7);

/* ---- Negative INT property ---- */
function neg_int_prop(n) {
    var o = {x: -5};
    var s = 0;
    for (var i = 0; i < n; i++) s += o.x;
    return s;
}
var r8 = neg_int_prop(100);
if (r8 !== -500) throw new Error("neg_int_prop: " + r8);

/* ---- Polymorphic IC: two different shapes ---- */
function poly_int_prop(n) {
    var o1 = {x: 2};
    var o2 = {y: 0, x: 3};  /* different shape */
    var s = 0;
    for (var i = 0; i < n; i++) {
        s += o1.x;
        s += o2.x;
    }
    return s;
}
var r9 = poly_int_prop(100);
if (r9 !== 500) throw new Error("poly_int_prop: " + r9);

/* ---- INT property comparison (used in loop condition via get_field) ---- */
function int_prop_cmp() {
    var o = {limit: 50};
    var s = 0;
    for (var i = 0; i < o.limit; i++) s += i;
    return s;
}
var r10 = int_prop_cmp();
if (r10 !== 1225) throw new Error("int_prop_cmp: " + r10);

print("P45 IC val_tag tests passed");
