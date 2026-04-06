/* P14 perf: try/catch/finally overhead in hot loops */
var N = 100000;

/* Bench 1: hot loop with try body that never throws (nip_catch path) */
function bench_try_no_throw() {
    var s = 0;
    for (var i = 0; i < N; i++) {
        try {
            s += i;
        } catch(e) {
            s += 10000;
        }
    }
    return s;
}

/* Bench 2: hot loop with try+finally (gosub/ret path) */
function bench_try_finally() {
    var s = 0;
    var fin = 0;
    for (var i = 0; i < N; i++) {
        try {
            s += i;
        } finally {
            fin++;
        }
    }
    return s + fin;
}

/* Bench 3: hot loop that throws each iteration (catch handler path) */
function bench_catch_thrown() {
    var s = 0;
    for (var i = 0; i < N; i++) {
        try {
            throw i;
        } catch(e) {
            s += e;
        }
    }
    return s;
}

var t0 = Date.now();
var r1 = bench_try_no_throw();
var t1 = Date.now();
print("try_no_throw(" + N + " iter): " + (t1 - t0) + " ms  result=" + r1);

var t2 = Date.now();
var r2 = bench_try_finally();
var t3 = Date.now();
print("try_finally(" + N + " iter): " + (t3 - t2) + " ms  result=" + r2);

var t4 = Date.now();
var r3 = bench_catch_thrown();
var t5 = Date.now();
print("catch_thrown(" + N + " iter): " + (t5 - t4) + " ms  result=" + r3);
