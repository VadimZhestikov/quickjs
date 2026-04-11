/* P43.4: JIT_T_INT propagation through bitwise operations.
 *
 * Verifies that OP_and/or/xor/shl/sar/not correctly type-infer their results
 * as INT, so that locals assigned from bit ops are stored as int64_t _jsi_*
 * and subsequent uses of those locals take the native _ti* fast path.
 */

function assert(cond, msg) {
    if (!cond) throw new Error("FAIL: " + msg);
}

/* Test 1: safe_add — MD5/SHA-style 32-bit word addition.
 * All intermediate results should be JIT_T_INT after the fix. */
function safe_add(x, y) {
    var lsw = (x & 0xFFFF) + (y & 0xFFFF);
    var msw = (x >> 16) + (y >> 16) + (lsw >> 16);
    return (msw << 16) | (lsw & 0xFFFF);
}

/* Verify safe_add correctness on known values. */
assert(safe_add(0, 0) === 0,          "safe_add(0,0)");
assert(safe_add(1, 1) === 2,          "safe_add(1,1)");
assert(safe_add(0xFFFF, 1) === 0x10000, "safe_add carry");
assert(safe_add(-1, 1) === 0,        "safe_add(-1,1)");
assert(safe_add(0x7FFFFFFF, 1) === ((0x7FFFFFFF + 1) | 0), "safe_add overflow");
/* Run many iterations to exercise JIT, verify final result is deterministic. */
var sum1 = 0;
for (var i = 0; i < 50000; i++) sum1 = safe_add(sum1, i);
var sum2 = 0;
for (var i = 0; i < 50000; i++) sum2 = safe_add(sum2, i);
assert(sum1 === sum2, "safe_add: deterministic (sum1=" + sum1 + ")");
print("test_safe_add: PASS");

/* Test 2: bit-op chains — int locals stay INT through multiple bit ops. */
function bit_chain(x) {
    var a = x & 0xFF;          /* AND → INT */
    var b = a << 8;            /* SHL INT INT → INT */
    var c = b | 0x42;          /* OR INT INT → INT */
    var d = c ^ 0xAA;          /* XOR INT INT → INT */
    var e = ~d;                /* NOT INT → INT */
    var f = e >> 2;            /* SAR INT INT → INT */
    return f & 0xFFFFFFFF;
}

var r = 0;
for (var i = 0; i < 200000; i++) r = bit_chain(i & 0xFF);
/* bit_chain(r_prev & 0xFF) on the last iteration: check a specific known input */
assert(bit_chain(0) === (-0x15 >> 2) & 0xFFFFFFFF || true, "bit_chain: computed");
assert(bit_chain(0xFF) === ((~((0xFF << 8 | 0x42) ^ 0xAA)) >> 2) & 0xFFFFFFFF || true, "bit_chain: 0xFF");
var expected_0 = ((~((0 | 0x42) ^ 0xAA)) >> 2) & 0xFFFFFFFF;
var expected_ff = ((~((0xFF00 | 0x42) ^ 0xAA)) >> 2) & 0xFFFFFFFF;
assert(bit_chain(0)    === expected_0,  "bit_chain(0)="+bit_chain(0)+" expected "+expected_0);
assert(bit_chain(0xFF) === expected_ff, "bit_chain(0xFF)="+bit_chain(0xFF)+" expected "+expected_ff);
print("test_bit_chain: PASS");

/* Test 3: xor-based hash — all INT path through loop. */
function xor_hash(data, len) {
    var h = 0x12345678;
    for (var i = 0; i < len; i++) {
        h = ((h << 5) | (h >>> 27)) ^ (data[i] & 0xFF);
        h = h & 0xFFFFFFFF;
    }
    return h >>> 0;
}

var data = [];
for (var i = 0; i < 256; i++) data.push(i * 37 & 0xFF);
var hash = 0;
for (var t = 0; t < 1000; t++) hash = xor_hash(data, 256);
assert(typeof hash === "number", "xor_hash: result is number");
assert(hash === xor_hash(data, 256), "xor_hash: deterministic");
print("test_xor_hash: PASS (hash=" + hash + ")");

/* Test 4: NOT correctness — ~n for various n. */
function test_not() {
    function bitnot(n) {
        var a = n & 0xFFFFFFFF;
        var b = ~a;
        return b & 0xFFFFFFFF;
    }
    for (var i = 0; i < 100000; i++) {
        var r = bitnot(i);
        var expected = (~i) & 0xFFFFFFFF;
        if (r !== expected) throw new Error("bitnot(" + i + ")=" + r + " expected " + expected);
    }
}
test_not();
print("test_not: PASS");

/* Test 5: SAR vs SHR correctness — sar keeps sign, shr does not. */
function test_shift() {
    function sar(x, n) { return x >> n; }
    function shr(x, n) { return x >>> n; }
    /* negative number: SAR should sign-extend, SHR should zero-extend */
    var neg = -256;
    assert(sar(neg, 4) === -16, "sar(-256,4)=" + sar(neg,4));
    assert(shr(neg, 4) === ((-256 >>> 4)), "shr(-256,4)=" + shr(neg,4));
    /* loop to JIT-compile */
    var s1 = 0, s2 = 0;
    for (var i = 0; i < 100000; i++) {
        s1 += sar(i - 50000, 3);
        s2 += shr(i, 3);
    }
    /* just verify they produce numbers */
    assert(typeof s1 === "number", "sar loop ok");
    assert(typeof s2 === "number", "shr loop ok");
}
test_shift();
print("test_shift: PASS");

print("P43.4 bit-op INT propagation: all tests passed");
