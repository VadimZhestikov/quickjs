/* P16 perf: measure delete in hot loop */
var N = 1000000;

function bench_delete() {
    var sum = 0;
    for (var i = 0; i < N; i++) {
        var o = {x: i, y: i + 1};
        delete o.x;
        sum += o.y || 0;
    }
    return sum;
}

var t0 = Date.now();
var r = bench_delete();
var t1 = Date.now();
print("delete(" + N + " iter): " + (t1 - t0) + " ms  result=" + r);
