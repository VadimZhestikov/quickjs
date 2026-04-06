/* P15 perf: for-in / for-of overhead in hot loops */
var N = 100000;

/* Bench 1: for-in over a fixed object (10 keys) */
function bench_for_in() {
    var obj = {a:1,b:2,c:3,d:4,e:5,f:6,g:7,h:8,i:9,j:10};
    var s = 0;
    for (var iter = 0; iter < N; iter++) {
        for (var k in obj) {
            s += obj[k];
        }
    }
    return s;
}

/* Bench 2: for-of over a fixed array (10 elements) */
function bench_for_of() {
    var arr = [1,2,3,4,5,6,7,8,9,10];
    var s = 0;
    for (var iter = 0; iter < N; iter++) {
        for (var v of arr) {
            s += v;
        }
    }
    return s;
}

/* Bench 3: for-of over a string */
function bench_for_of_string() {
    var str = "hello";
    var s = 0;
    for (var iter = 0; iter < N; iter++) {
        for (var c of str) {
            s += c.length;
        }
    }
    return s;
}

var t0 = Date.now();
var r1 = bench_for_in();
var t1 = Date.now();
print("for_in(" + N + " iter, 10-key obj): " + (t1 - t0) + " ms  result=" + r1);

var t2 = Date.now();
var r2 = bench_for_of();
var t3 = Date.now();
print("for_of(" + N + " iter, 10-el arr): " + (t3 - t2) + " ms  result=" + r2);

var t4 = Date.now();
var r3 = bench_for_of_string();
var t5 = Date.now();
print("for_of_string(" + N + " iter, 5-char): " + (t5 - t4) + " ms  result=" + r3);
