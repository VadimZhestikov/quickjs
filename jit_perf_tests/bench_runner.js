/*
 * bench_runner.js — JIT performance benchmark runner
 *
 * Usage (QuickJS):
 *   ./qjs jit_perf_tests/bench_runner.js
 * Usage (Node.js):
 *   node jit_perf_tests/bench_runner.js
 *
 * Reports: benchmark name, iterations, elapsed ms, Mops/s
 * JSON output (for diffing): set env BENCH_JSON=1  (not used here; plain text)
 */

/* Shims for engines that lack QuickJS/Node built-ins */
if (typeof print === 'undefined') var print = console.log.bind(console);
if (typeof performance === 'undefined') var performance = { now: function() { return Date.now(); } };

function bench(name, warmup_iters, iters, fn) {
    /* Warm-up: trigger JIT compilation. */
    for (let i = 0; i < warmup_iters; i++) fn();
    /* Wait for GCC async compilation to finish (no-op in non-JIT builds). */
    if (typeof __jit_drain !== 'undefined') __jit_drain();

    const t0 = performance.now();
    for (let i = 0; i < iters; i++) fn();
    const elapsed = performance.now() - t0;   /* ms */

    const mops = (iters / elapsed / 1000).toFixed(3);   /* Mops/s */
    const ms   = elapsed.toFixed(2);
    print(`  ${name.padEnd(32)} ${ms.padStart(8)} ms  ${mops.padStart(8)} Mops/s`);
    return elapsed;
}

print("=".repeat(62));
print("Benchmark                        Elapsed (ms)   Throughput");
print("=".repeat(62));

/* ------------------------------------------------------------------ */
/* 1. Fibonacci (recursive) — exercises function calls + conditionals  */
/* ------------------------------------------------------------------ */
function fib(n) {
    if (n <= 1) return n;
    return fib(n-1) + fib(n-2);
}
bench("fib(30) x1", 5, 1, () => fib(30));

/* ------------------------------------------------------------------ */
/* 2. Integer sum loop — pure arithmetic + locals                      */
/* ------------------------------------------------------------------ */
function sum_loop(n) {
    let s = 0;
    for (let i = 0; i < n; i++) s += i;
    return s;
}
bench("sum_loop(1e6)", 5, 20, () => sum_loop(1000000));

/* ------------------------------------------------------------------ */
/* 3. Sum of squares — multiply inside loop                            */
/* ------------------------------------------------------------------ */
function sum_sq(n) {
    let s = 0;
    for (let i = 1; i <= n; i++) s += i * i;
    return s;
}
bench("sum_sq(1e6)", 5, 20, () => sum_sq(1000000));

/* ------------------------------------------------------------------ */
/* 4. Property read loop — get_field repeated                          */
/* ------------------------------------------------------------------ */
function prop_read(o, n) {
    let s = 0;
    for (let i = 0; i < n; i++) s += o.x;
    return s;
}
const obj_r = { x: 42 };
bench("prop_read(1e6)", 5, 20, () => prop_read(obj_r, 1000000));

/* ------------------------------------------------------------------ */
/* 5. Property write loop — put_field repeated                         */
/* ------------------------------------------------------------------ */
function prop_write(o, n) {
    for (let i = 0; i < n; i++) o.x = i;
}
const obj_w = { x: 0 };
bench("prop_write(1e6)", 5, 20, () => prop_write(obj_w, 1000000));

/* ------------------------------------------------------------------ */
/* 6. Closure counter — var_ref access                                 */
/* ------------------------------------------------------------------ */
function make_counter() {
    let n = 0;
    return function() { n++; return n; };
}
const counter = make_counter();
bench("closure_counter(1e6)", 5, 1, () => {
    for (let i = 0; i < 1000000; i++) counter();
});

/* ------------------------------------------------------------------ */
/* 7. Recursive power — tail-recursion-like                            */
/* ------------------------------------------------------------------ */
function ipow(base, exp) {
    if (exp === 0) return 1;
    return base * ipow(base, exp - 1);
}
bench("ipow(2,20) x1e5", 5, 1, () => {
    let s = 0;
    for (let i = 0; i < 100000; i++) s += ipow(2, 20);
    return s;
});

/* ------------------------------------------------------------------ */
/* 8. String building — add opcode with string operands                */
/* ------------------------------------------------------------------ */
function str_concat(n) {
    let s = "";
    for (let i = 0; i < n; i++) s += "x";
    return s.length;
}
bench("str_concat(5000)", 5, 100, () => str_concat(5000));

/* ------------------------------------------------------------------ */
/* 9. Boolean-heavy — comparisons + branches                           */
/* ------------------------------------------------------------------ */
function count_primes(n) {
    let count = 0;
    for (let i = 2; i <= n; i++) {
        let ok = true;
        for (let j = 2; j * j <= i; j++) {
            if (i % j === 0) { ok = false; break; }
        }
        if (ok) count++;
    }
    return count;
}
bench("count_primes(3000)", 5, 10, () => count_primes(3000));

/* ------------------------------------------------------------------ */
/* 10. Array element access                                            */
/* ------------------------------------------------------------------ */
function arr_sum(arr) {
    let s = 0;
    const n = arr.length;
    for (let i = 0; i < n; i++) s += arr[i];
    return s;
}
const arr = [];
for (let i = 0; i < 10000; i++) arr.push(i);
bench("arr_sum(10000) x1e3", 5, 1000, () => arr_sum(arr));

print("=".repeat(62));
