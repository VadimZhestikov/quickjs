/*
 * bench_aot.js — benchmark for --jit-aot / --jit-warmup precompiled mode
 *
 * With --jit-aot, all functions are GCC-compiled (or loaded from cache)
 * before execution starts.  No busy-wait warm-up needed.
 *
 * Run with (QuickJS):
 *   ./qjs --jit-warmup jit_perf_tests/bench_aot.js   # populate cache
 *   ./qjs --jit-aot    jit_perf_tests/bench_aot.js   # measure (cache hit)
 *   ./qjs              jit_perf_tests/bench_aot.js   # interpreter baseline
 * Run with (Node.js):
 *   node jit_perf_tests/bench_aot.js
 */

/* Node.js shim: QuickJS has print() built-in; Node uses console.log */
if (typeof print === 'undefined') var print = console.log.bind(console);

function fib(n) {
    if (n <= 1) return n;
    return fib(n-1) + fib(n-2);
}

function sum_loop(n) {
    let s = 0;
    for (let i = 0; i < n; i++) s += i;
    return s;
}

function sum_sq(n) {
    let s = 0;
    for (let i = 1; i <= n; i++) s += i * i;
    return s;
}

function count_primes(n) {
    let count = 0;
    for (let i = 2; i <= n; i++) {
        let ok = true;
        for (let j = 2; j * j <= i; j++) { if (i % j === 0) { ok = false; break; } }
        if (ok) count++;
    }
    return count;
}

function arr_sum(arr) {
    let s = 0;
    for (let i = 0; i < arr.length; i++) s += arr[i];
    return s;
}

const arr10k = new Array(10000).fill(1).map((_, i) => i);

function bench(name, iters, fn) {
    /* warm instruction cache */
    for (let i = 0; i < 3; i++) fn();
    const t0 = performance.now();
    for (let i = 0; i < iters; i++) fn();
    const ms = (performance.now() - t0).toFixed(2);
    print(`  ${name.padEnd(32)} ${ms.padStart(8)} ms`);
}

print("=".repeat(50));
bench("fib(30) x1",              1,    () => fib(30));
bench("sum_loop(1e6) x20",      20,    () => sum_loop(1000000));
bench("sum_sq(1e6) x20",        20,    () => sum_sq(1000000));
bench("count_primes(3000) x10", 10,    () => count_primes(3000));
bench("arr_sum(10000) x1000",   1000,  () => arr_sum(arr10k));
print("=".repeat(50));
