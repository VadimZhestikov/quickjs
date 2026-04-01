/*
 * run_qjs.js — V8 benchmark suite runner for QuickJS
 *
 * Run from the v8bench/ directory:
 *   cd jit_perf_tests/v8bench
 *   ../../qjs run_qjs.js
 *
 * QuickJS differences from V8 shell:
 *   - load() → __loadScript()  (global in qjs CLI)
 *   - no window / document / alert / setTimeout
 *   - base.js RunSuites already skips setTimeout when window is absent
 */

/* Compatibility shims */
var load = __loadScript;
var alert = print;          /* earley-boyer.js defines sc_alert() which calls alert */

/* Load benchmark suite framework and all benchmarks */
load('base.js');
load('richards.js');
load('deltablue.js');
load('crypto.js');
load('raytrace.js');
load('earley-boyer.js');
load('regexp.js');
load('splay.js');

/* Runner callbacks */
var success = true;

function PrintResult(name, result) {
    print(name + ': ' + result);
}

function PrintError(name, error) {
    print('ERROR ' + name + ': ' + error);
    success = false;
}

function PrintScore(score) {
    if (success) {
        print('----');
        print('Score (version ' + BenchmarkSuite.version + '): ' + score);
    }
}

BenchmarkSuite.RunSuites({
    NotifyResult: PrintResult,
    NotifyError:  PrintError,
    NotifyScore:  PrintScore
});
