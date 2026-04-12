/* test_jit_p41_variadic.js
 *
 * Regression test for P41.1 fast path passing b->arg_count (= 0 for variadic
 * functions) instead of the real argc to the JIT function.  When a JIT .so is
 * loaded from the disk cache the P41.1 path fires immediately, so the bug only
 * manifested on the second process run.
 *
 * We can't control whether the P41.1 path fires (it depends on var_ref_count),
 * but we CAN verify that a variadic function returns the correct result after
 * being JIT-compiled, regardless of call path.
 */

/* Drain any in-flight GCC compilations before checking results. */
if (typeof __jit_drain !== 'undefined') __jit_drain();

function assert_eq(a, b, msg) {
    if (a !== b) throw new Error((msg || "assert_eq") + ": " + a + " !== " + b);
}

/* Variadic function using arguments object — no declared parameters. */
function variadic_sum() {
    var s = 0;
    for (var i = 0; i < arguments.length; i++)
        s += arguments[i];
    return s;
}

/* Variadic list-builder (similar to earley-boyer.js sc_list). */
function variadic_list() {
    var res = [];
    for (var i = 0; i < arguments.length; i++)
        res.push(arguments[i]);
    return res;
}

/* Warm up both functions to trigger JIT compilation (threshold=200). */
var dummy = 0;
for (var i = 0; i < 220; i++) {
    dummy += variadic_sum(1, 2, 3);   /* must equal 6 */
    variadic_list(10, 20, 30);
}
if (typeof __jit_drain !== 'undefined') __jit_drain();

/* Verify results are correct after JIT kicks in. */
assert_eq(variadic_sum(), 0, "variadic_sum() 0 args");
assert_eq(variadic_sum(5), 5, "variadic_sum(5) 1 arg");
assert_eq(variadic_sum(1, 2, 3), 6, "variadic_sum(1,2,3) 3 args");
assert_eq(variadic_sum(1, 2, 3, 4, 5), 15, "variadic_sum(1,2,3,4,5) 5 args");

var lst = variadic_list("a", "b", "c");
assert_eq(lst.length, 3, "variadic_list length");
assert_eq(lst[0], "a", "variadic_list[0]");
assert_eq(lst[1], "b", "variadic_list[1]");
assert_eq(lst[2], "c", "variadic_list[2]");

var lst0 = variadic_list();
assert_eq(lst0.length, 0, "variadic_list() empty");

print("test_jit_p41_variadic: all tests passed");
