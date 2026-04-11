/* P43.2: Quadrimorphic IC correctness tests.
 * Exercises get_field and put_field with 1, 2, 3, and 4 distinct object shapes
 * at the same call site, verifying that all shapes hit the IC correctly.
 */

function assert(cond, msg) {
    if (!cond) throw new Error("FAIL: " + msg);
}

/* Test 1: get_field quadrimorphic — 4 distinct shapes at one get site.
 * Each shape has a different set of properties, so each object's 'val'
 * property is at a different slot index in the shape's prop array. */
function test_get_quad() {
    function getVal(o) { return o.val; }

    // Warm up with 4 shapes to promote IC to quadrimorphic.
    var o1 = { val: 10 };                  // shape S1: {val}
    var o2 = { x: 1, val: 20 };           // shape S2: {x, val}
    var o3 = { x: 1, y: 2, val: 30 };     // shape S3: {x, y, val}
    var o4 = { x: 1, y: 2, z: 3, val: 40 }; // shape S4: {x, y, z, val}

    // After each call the IC state should advance: mono→bi→tri→quad.
    assert(getVal(o1) === 10, "quad-get S1 first call");
    assert(getVal(o2) === 20, "quad-get S2 second call");
    assert(getVal(o3) === 30, "quad-get S3 third call");
    assert(getVal(o4) === 40, "quad-get S4 fourth call");

    // Verify all 4 shapes still hit correctly after IC is fully populated.
    for (var i = 0; i < 100; i++) {
        assert(getVal(o1) === 10, "quad-get S1 warm loop");
        assert(getVal(o2) === 20, "quad-get S2 warm loop");
        assert(getVal(o3) === 30, "quad-get S3 warm loop");
        assert(getVal(o4) === 40, "quad-get S4 warm loop");
    }
}
test_get_quad();

/* Test 2: put_field quadrimorphic — 4 distinct shapes at one put site. */
function test_put_quad() {
    function setVal(o, v) { o.val = v; }

    var o1 = { val: 0 };
    var o2 = { x: 1, val: 0 };
    var o3 = { x: 1, y: 2, val: 0 };
    var o4 = { x: 1, y: 2, z: 3, val: 0 };

    setVal(o1, 11);
    setVal(o2, 22);
    setVal(o3, 33);
    setVal(o4, 44);
    assert(o1.val === 11, "quad-put S1");
    assert(o2.val === 22, "quad-put S2");
    assert(o3.val === 33, "quad-put S3");
    assert(o4.val === 44, "quad-put S4");

    for (var i = 0; i < 100; i++) {
        setVal(o1, i);
        setVal(o2, i+1);
        setVal(o3, i+2);
        setVal(o4, i+3);
        assert(o1.val === i,   "quad-put S1 warm");
        assert(o2.val === i+1, "quad-put S2 warm");
        assert(o3.val === i+2, "quad-put S3 warm");
        assert(o4.val === i+3, "quad-put S4 warm");
    }
}
test_put_quad();

/* Test 3: 5th shape triggers megamorphic; slow path still correct. */
function test_mega() {
    function getVal(o) { return o.val; }

    var o1 = { val: 1 };
    var o2 = { a: 1, val: 2 };
    var o3 = { a: 1, b: 2, val: 3 };
    var o4 = { a: 1, b: 2, c: 3, val: 4 };
    var o5 = { a: 1, b: 2, c: 3, d: 4, val: 5 }; // 5th shape → megamorphic

    assert(getVal(o1) === 1, "mega S1");
    assert(getVal(o2) === 2, "mega S2");
    assert(getVal(o3) === 3, "mega S3");
    assert(getVal(o4) === 4, "mega S4");
    assert(getVal(o5) === 5, "mega S5 triggers mega");

    // After megamorphic, all shapes still return correct values via slow path.
    for (var i = 0; i < 20; i++) {
        assert(getVal(o1) === 1, "mega S1 post-mega");
        assert(getVal(o2) === 2, "mega S2 post-mega");
        assert(getVal(o3) === 3, "mega S3 post-mega");
        assert(getVal(o4) === 4, "mega S4 post-mega");
        assert(getVal(o5) === 5, "mega S5 post-mega");
    }
}
test_mega();

/* Test 4: EarleyBoyer-style cons-cell traversal with 3 shapes — previously
 * would go megamorphic with bimorphic IC (n==3 after 3rd shape). With quad
 * IC, all 3 shapes stay in the IC. */
function test_earleyboyer_cons() {
    // Simulate cons cells like EarleyBoyer: {car, cdr} / {car, cdr, tag} / {car}
    function getCar(c) { return c.car; }
    function getCdr(c) { return c.cdr; }

    function makeCons(car, cdr)       { return { car, cdr }; }
    function makeTagged(car, cdr, tag) { return { tag, car, cdr }; } // different shape
    function makeLeaf(car)             { return { car }; }

    var hits = 0;
    for (var i = 0; i < 200; i++) {
        var c1 = makeCons(i, i+1);
        var c2 = makeTagged(i+2, i+3, "t");
        var c3 = makeLeaf(i+4);

        // getCar is called with 3 distinct shapes: {car,cdr}, {tag,car,cdr}, {car}
        assert(getCar(c1) === i,   "cons car c1");
        assert(getCar(c2) === i+2, "tagged car c2");
        assert(getCar(c3) === i+4, "leaf car c3");

        assert(getCdr(c1) === i+1, "cons cdr c1");
        assert(getCdr(c2) === i+3, "tagged cdr c2");
    }
    print("test_earleyboyer_cons: PASS (" + (i*3) + " getCar calls, " + (i*2) + " getCdr calls)");
}
test_earleyboyer_cons();

print("P43.2 quadrimorphic IC: all tests passed");
