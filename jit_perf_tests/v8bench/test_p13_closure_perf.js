/* P13 perf: measure closure creation and invocation in hot loops */
var N = 100000;

/* Bench 1: counter closure — OP_fclosure + captured local mutation */
function bench_counter_closure() {
    function makeCounter() {
        var n = 0;
        return function() { return n++; };
    }
    var s = 0;
    for (var i = 0; i < N; i++) {
        var c = makeCounter();
        s += c() + c() + c();  /* 0+1+2 = 3 per iter */
    }
    return s;
}

/* Bench 2: arg capture — OP_fclosure capturing an argument */
function bench_arg_capture() {
    function makeAdder(x) {
        return function(y) { return x + y; };
    }
    var add5  = makeAdder(5);
    var add10 = makeAdder(10);
    var s = 0;
    for (var i = 0; i < N; i++) {
        s += add5(i) + add10(i);
    }
    return s;
}

/* Bench 3: shared closure — two closures sharing one captured local */
function bench_shared() {
    function makePair() {
        var x = 0;
        return { inc: function() { x++; }, get: function() { return x; } };
    }
    var s = 0;
    for (var i = 0; i < N; i++) {
        var p = makePair();
        p.inc(); p.inc();
        s += p.get();
    }
    return s;
}

var t0 = Date.now();
var r1 = bench_counter_closure();
var t1 = Date.now();
print("counter_closure(" + N + " iter): " + (t1 - t0) + " ms  result=" + r1);

var t2 = Date.now();
var r2 = bench_arg_capture();
var t3 = Date.now();
print("arg_capture(" + N + " iter): " + (t3 - t2) + " ms  result=" + r2);

var t4 = Date.now();
var r3 = bench_shared();
var t5 = Date.now();
print("shared_closure(" + N + " iter): " + (t5 - t4) + " ms  result=" + r3);
