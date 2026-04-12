/* Regression test for P10.3 warm-cache correctness bug.
 *
 * Bug: When a JIT-compiled caller uses the P10.3 direct-call path to invoke
 * a JIT-compiled callee, the callee receives the caller's _ca[] array as
 * argv.  Old .so files (compiled without mutated_arg_mask protection) wrote
 * to argv[i] directly (OP_put_arg) — on return the caller also freed the
 * same JSValue, causing a double-free / heap corruption.
 *
 * Fix: jit_p103_safe flag — set only when the .so exports __jit_cv_HASH
 * equal to JIT_CODEGEN_VERSION.  js_jit_check_and_extract returns 0 (falls
 * back to safe js_jit_call) if jit_p103_safe==0.
 *
 * This test verifies that a callee which mutates its argument (by walking
 * a linked list: `lst = lst.next`) produces correct results when called
 * many times from a JIT-compiled outer loop, both before and after JIT
 * compilation of both functions.
 */

'use strict';

var passed = 0;
var failed = 0;

function assert(cond, msg) {
    if (!cond) {
        print('FAIL: ' + msg);
        failed++;
    } else {
        passed++;
    }
}

/* Build a linked list of length n: {val:n-1} -> {val:n-2} -> ... -> null */
function makeList(n) {
    var lst = null;
    for (var i = 0; i < n; i++) {
        lst = {val: i, next: lst};
    }
    return lst;
}

/* Reverse a linked list.  CRITICAL: mutates `lst` argument each iteration
 * (lst = lst.next) — this generates OP_put_arg / OP_set_arg in bytecode.
 * With P10.3 direct calls, old .so files would corrupt the caller's argv. */
function listReverse(lst) {
    var acc = null;
    while (lst !== null) {
        acc = {val: lst.val, next: acc};
        lst = lst.next;
    }
    return acc;
}

/* Count list length, also mutating its argument. */
function listLength(lst) {
    var n = 0;
    while (lst !== null) {
        n++;
        lst = lst.next;
    }
    return n;
}

/* Outer loop: calls listReverse many times so JIT compiles both functions
 * and (after warmup) uses the P10.3 direct-call path. */
function runAll(reps) {
    var ok = true;
    for (var i = 0; i < reps; i++) {
        var lst = makeList(8);
        var rev = listReverse(lst);
        var len = listLength(rev);
        if (len !== 8) { ok = false; break; }
        /* Verify the reversed order: original was 7->6->5->4->3->2->1->0->null
         * (makeList builds in reverse), reversed back to 0->1->2->3->4->5->6->7. */
        var cur = rev;
        var idx = 0;
        while (cur !== null) {
            if (cur.val !== idx) { ok = false; break; }
            cur = cur.next;
            idx++;
        }
        if (!ok) break;
    }
    return ok;
}

/* Run twice: once to warm the JIT, once to exercise the compiled path. */
var warm = runAll(200);
assert(warm, 'listReverse produced wrong result during warm-up');

var hot = runAll(50);
assert(hot, 'listReverse produced wrong result in hot (JIT) path');

/* Spot-check a single reversal to catch value corruption. */
var lst5 = makeList(5);
var rev5 = listReverse(lst5);
assert(listLength(rev5) === 5, 'reversed length 5');
var cur = rev5;
for (var i = 0; i < 5; i++) {
    assert(cur !== null && cur.val === i, 'reversed[' + i + '] = ' + (cur ? cur.val : 'null'));
    if (cur) cur = cur.next;
}

if (failed === 0) {
    print('PASS (' + passed + ' assertions)');
} else {
    print('FAIL (' + failed + '/' + (passed + failed) + ' assertions failed)');
    throw new Error('test failed');
}
