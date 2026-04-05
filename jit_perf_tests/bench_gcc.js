/*
 * bench_gcc.js — GCC tier-2 JIT benchmark
 *
 * Strategy:
 *   1. Call each benchmark function 5 times to trigger GCC (threshold=2).
 *   2. Busy-wait up to 8 s for GCC to finish compiling all functions.
 *   3. Run a few warm calls to confirm JIT is active.
 *   4. Measure elapsed time.
 *
 * Run with (QuickJS JIT):
 *   ./qjs jit_perf_tests/bench_gcc.js
 *   (Build with: make CONFIG_JIT=y JIT_THRESHOLD_GCC=2 qjs)
 * Run with (Node.js — measures Node's own JIT, busy-wait is a no-op):
 *   node jit_perf_tests/bench_gcc.js
 */

/* Node.js shim: QuickJS has print() built-in; Node uses console.log */
if (typeof print === 'undefined') var print = console.log.bind(console);

/* ---- benchmark functions ---- */

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

/* ---- step 1: trigger GCC (>= threshold=2 calls each) ---- */
for (let i = 0; i < 5; i++) {
    fib(20);
    sum_loop(1000);
    sum_sq(1000);
    count_primes(100);
    arr_sum(arr10k);
}

/* ---- step 2: busy-wait up to 8 s for GCC ---- */
const wait_start = performance.now();
/* Spin calling each function once per 50ms iteration until 8s elapsed.
   Each spin also re-triggers fib recursion to ensure it's hot. */
let spin = 0;
while (performance.now() - wait_start < 8000) {
    fib(15); sum_loop(1000); sum_sq(1000); count_primes(50); arr_sum(arr10k);
    spin++;
}

/* ---- step 3 + 4: measure ---- */
function bench(name, iters, fn) {
    /* A few calls to warm instruction cache after JIT swap */
    for (let i = 0; i < 3; i++) fn();
    const t0 = performance.now();
    for (let i = 0; i < iters; i++) fn();
    const ms = (performance.now() - t0).toFixed(2);
    print(`  ${name.padEnd(32)} ${ms.padStart(8)} ms`);
}

print("=".repeat(50));
print("GCC tier-2 benchmark (threshold=2, warmed)");
print("=".repeat(50));
bench("fib(30) x1",      1,  () => fib(30));
bench("sum_loop(1e6) x20", 20, () => sum_loop(1000000));
bench("sum_sq(1e6) x20",   20, () => sum_sq(1000000));
bench("count_primes(3000) x10", 10, () => count_primes(3000));
bench("arr_sum(10000) x1000",  1000, () => arr_sum(arr10k));
print("=".repeat(50));
