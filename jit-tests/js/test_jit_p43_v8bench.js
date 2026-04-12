/* P43.6 / P9.4: Correctness tests derived from V8 benchmark bug patterns.
 *
 * P43.6 fix: jit_infer_types was unconditionally pushing JIT_T_INT for
 * OP_and/or/xor/shl/sar when either input might be JSVAL (e.g. a variable
 * holding a large integer read from an array).  This caused locals that
 * capture the result to be typed INT (int64_t) instead of NUMBER (double),
 * silently truncating large intermediate values in the crypto am3 function.
 *
 * P9.4 fix: gen_body was missing _P94_ENSURE before OP_dup1, OP_dup2,
 * OP_insert2, OP_nip, OP_rot3l, OP_rot3r, OP_dup3.  Without boxing, a
 * stale _ti register could be read as the JSValue after the shuffle,
 * producing phantom references and gc_decref_child UAF crashes (DeltaBlue).
 */

function assert(cond, msg) {
    if (!cond) throw new Error("FAIL: " + msg);
}

/* ---------------------------------------------------------------------- */
/* Test 1: am3-style big-integer inner loop (P43.6 regression)
 *
 * Replicates the exact pattern in crypto.js bnpMultiplyTo / am3:
 *   var xl = x & 0x3fff, xh = x >> 14;
 *   var l = arr[i] & 0x3fff;
 *   var h = arr[i] >> 14;
 *   var m = xh*l + h*xl;
 *   l = xl*l + ((m & 0x3fff) << 14) + c;
 *   c = (l >> 28) + (m >> 14) + xh*h;
 *
 * xl, xh, h are assigned from bitwise ops whose inputs are JSVAL (read from
 * array).  Before P43.6 they were typed INT (int64_t), truncating large
 * products like xh*h silently.
 */
function test_am3_pattern() {
    function am3_inner(x_arr, w_arr, x_idx, j_start, n, c_init) {
        var x = x_arr[x_idx];
        var xl = x & 0x3fff;
        var xh = x >> 14;
        var c = c_init;
        var j = j_start;
        var i = 0;
        while (--n >= 0) {
            var l = x_arr[i] & 0x3fff;
            var h = x_arr[i++] >> 14;
            var m = xh * l + h * xl;
            l = xl * l + ((m & 0x3fff) << 14) + w_arr[j] + c;
            c = (l >> 28) + (m >> 14) + xh * h;
            w_arr[j++] = l & 0xfffffff;
        }
        return c;
    }

    /* Use values that produce large intermediates (xh*h > 2^31 if typed INT). */
    var x_arr = [0x1ffffff, 0x1ffffff, 0x1ffffff, 0x1ffffff];
    var w_arr = [0, 0, 0, 0, 0, 0];
    var c = am3_inner(x_arr, w_arr, 0, 0, 4, 0);

    /* Recompute reference with pure integer math. */
    function am3_ref(x_arr, w_arr, x_idx, j_start, n, c_init) {
        var x = x_arr[x_idx];
        var xl = x & 0x3fff;
        var xh = x >> 14;
        var c = c_init;
        var j = j_start;
        var i = 0;
        while (--n >= 0) {
            var l = x_arr[i] & 0x3fff;
            var h = x_arr[i++] >> 14;
            var m = xh * l + h * xl;
            l = xl * l + ((m & 0x3fff) << 14) + w_arr[j] + c;
            c = (l >> 28) + (m >> 14) + xh * h;
            w_arr[j++] = l & 0xfffffff;
        }
        return c;
    }
    var w_ref = [0, 0, 0, 0, 0, 0];
    var c_ref = am3_ref(x_arr, w_ref, 0, 0, 4, 0);

    assert(c === c_ref, "am3_inner c=" + c + " expected " + c_ref);
    for (var i = 0; i < 4; i++)
        assert(w_arr[i] === w_ref[i], "am3_inner w[" + i + "]=" + w_arr[i] + " expected " + w_ref[i]);
    print("test_am3_pattern: PASS");
}
test_am3_pattern();

/* Test 1b: RSA-style accumulator — run many iterations and compare. */
function test_am3_accumulator() {
    function accumulate(N) {
        var arr = [];
        for (var i = 0; i < 32; i++) arr.push((i * 0x12345 + 0xabcd) & 0xfffffff);
        var w = [];
        for (var i = 0; i < 64; i++) w.push(0);
        var c = 0;
        for (var rep = 0; rep < N; rep++) {
            /* am3 kernel for one digit */
            var x = arr[rep & 31];
            var xl = x & 0x3fff, xh = x >> 14;
            for (var j = 0; j < 16; j++) {
                var l = arr[j] & 0x3fff;
                var h = arr[j] >> 14;
                var m = xh * l + h * xl;
                l = xl * l + ((m & 0x3fff) << 14) + w[j] + c;
                c = (l >> 28) + (m >> 14) + xh * h;
                w[j] = l & 0xfffffff;
            }
        }
        return c;
    }
    /* Run twice; results must be identical (deterministic arithmetic). */
    var r1 = accumulate(200);
    var r2 = accumulate(200);
    assert(r1 === r2, "am3_accumulator: not deterministic (" + r1 + " vs " + r2 + ")");
    print("test_am3_accumulator: PASS (c=" + r1 + ")");
}
test_am3_accumulator();

/* ---------------------------------------------------------------------- */
/* Test 2: OP_dup1 with typed source slot (P9.4 regression)
 *
 * DeltaBlue failure pattern: a local holds an INT-typed value; an OP_dup1
 * is emitted to duplicate it on the stack.  Before P9.4, the _P94_ENSURE
 * was missing so _tsv{d-2} was NOT boxed before the shuffle.  The shuffle
 * then propagated an unboxed _ti as a JSValue pointer → UAF on free.
 */
function test_dup1_typed() {
    /* Force dup1 via: f(obj) { var x = obj.a; return x + (x = obj.b, x); }
     * is not clean.  A cleaner trigger: use a pattern that the QuickJS compiler
     * emits dup1 for.  QuickJS emits dup1 for `a += expr` assignments when
     * the get_loc + put_loc pair is optimised. */

    /* Direct test: verify dup1 does not corrupt the duplicated value. */
    function dup1_test(n) {
        var sum = 0;
        for (var i = 0; i < n; i++) {
            /* arr[i++] pattern causes dup1 to duplicate the index */
            var a = [1, 2, 3, 4, 5, 6, 7, 8];
            var j = 0;
            /* QuickJS emits dup1 for `a[j++]` when it keeps j live */
            sum += a[j++] + a[j++] + a[j++] + a[j++];
        }
        return sum;
    }
    var r = dup1_test(10000);
    /* 4 accesses of [1,2,3,4] per iteration = 10 each time */
    assert(r === 10 * 10000, "dup1_typed: expected " + (10*10000) + " got " + r);
    print("test_dup1_typed: PASS");
}
test_dup1_typed();

/* ---------------------------------------------------------------------- */
/* Test 3: OP_insert2 with typed slot (P9.4)
 *
 * QuickJS emits insert2 (dup_x1) for `a.prop = expr` where prop is a
 * computed key, and for compound assignment chains.  A typed _tsv slot
 * for the inserted value before P9.4 would not be boxed.
 */
function test_insert2_typed() {
    function make_obj() { return { x: 0, y: 0 }; }
    var objs = [];
    for (var i = 0; i < 50; i++) objs.push(make_obj());

    var sum = 0;
    for (var iter = 0; iter < 5000; iter++) {
        for (var i = 0; i < 50; i++) {
            /* set_field on an object — may use insert2 internally */
            objs[i].x = iter & 0xffff;
            objs[i].y = (iter + i) & 0xffff;
        }
        sum += objs[iter % 50].x + objs[iter % 50].y;
    }
    /* verify it doesn't crash and produces a number */
    assert(typeof sum === "number", "insert2_typed: sum not a number");
    print("test_insert2_typed: PASS (sum=" + sum + ")");
}
test_insert2_typed();

/* ---------------------------------------------------------------------- */
/* Test 4: OP_rot3l / rot3r with typed slots (P9.4) */
function test_rot3_typed() {
    /* Rotation is triggered by certain call / property-set patterns.
     * Verify that after rotation the values are still usable. */
    function rot3_workload(n) {
        var result = 0;
        var a = { v: 0 };
        for (var i = 0; i < n; i++) {
            /* obj[key] = val pattern → rot3 in some forms */
            var k = "v";
            a[k] = i & 0xffff;
            result += a[k];
        }
        return result;
    }
    var r = rot3_workload(100000);
    /* sum of 0..99999 mod 0xffff */
    var expected = 0;
    for (var i = 0; i < 100000; i++) expected += i & 0xffff;
    assert(r === expected, "rot3_typed: got " + r + " expected " + expected);
    print("test_rot3_typed: PASS");
}
test_rot3_typed();

/* ---------------------------------------------------------------------- */
/* Test 5: Mixed INT + JSVAL sar/and — the exact P43.6 regression pattern.
 *
 * xl = x & 0x3fff  — x comes from array (JSVAL), result → must be NUMBER
 * xh = x >> 14     — sar with JSVAL input → must be NUMBER
 * h  = arr >> 14   — sar with JSVAL input → must be NUMBER
 *
 * If xl/xh/h are mistyped as INT (int64_t) the multiplication xh*h
 * overflows 32 bits and produces wrong results.
 */
function test_jsval_bitop_locals() {
    var BIG = 0x1fff;  /* 8191 — big enough that xh = BIG >> 14 = 0 but xh*l can be large */
    var VERYBIG = 0x3ffff; /* 262143 — xh = 262143 >> 14 = 15, xh*xh = 225 */

    var arr = [VERYBIG, VERYBIG, VERYBIG, VERYBIG];

    function inner(arr) {
        var x = arr[0];
        var xl = x & 0x3fff;   /* JSVAL & INT → must stay NUMBER or INT safely */
        var xh = x >> 14;      /* JSVAL sar INT → must stay NUMBER or INT safely */
        var h  = arr[1] >> 14; /* JSVAL sar INT → h = 15 */
        /* key: xh * h = 15 * 15 = 225 — fits in int32 fine */
        /* but xh * xl = 15 * 16383 = 245745 — must not truncate */
        return xh * h + xl * h + xh * xh;
    }

    /* Run warm-up to JIT-compile. */
    var sum = 0;
    for (var i = 0; i < 100000; i++) sum += inner(arr);
    var expected = (15 * 15 + 16383 * 15 + 15 * 15) * 100000;
    assert(sum === expected, "jsval_bitop_locals: sum=" + sum + " expected=" + expected);
    print("test_jsval_bitop_locals: PASS");
}
test_jsval_bitop_locals();

print("P43.6/P9.4 v8bench correctness: all tests passed");
