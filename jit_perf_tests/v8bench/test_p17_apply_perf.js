/* P17 perf: measure spread/apply in hot loop */
var N = 100000;

function sum3(a, b, c) { return a + b + c; }
function sum_n(n, arr) {
    var s = 0;
    for (var i = 0; i < n; i++) s += arr[i];
    return s;
}

/* OP_apply: sum3(...[a,b,c]) */
function bench_apply_spread() {
    var s = 0;
    var arr = [1, 2, 3];
    for (var i = 0; i < N; i++) {
        s += sum3(...arr);
    }
    return s;
}

/* OP_apply via Function.prototype.apply */
function bench_apply_method() {
    var s = 0;
    var arr = [10, 20, 30];
    for (var i = 0; i < N; i++) {
        s += sum3.apply(null, arr);
    }
    return s;
}

var t0 = Date.now();
var r1 = bench_apply_spread();
var t1 = Date.now();
print("apply_spread(" + N + " iter): " + (t1 - t0) + " ms  result=" + r1);

var t2 = Date.now();
var r2 = bench_apply_method();
var t3 = Date.now();
print("apply_method(" + N + " iter): " + (t3 - t2) + " ms  result=" + r2);
