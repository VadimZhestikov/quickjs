/*
 * run_node.js — V8 benchmark suite runner for Node.js
 *
 * Run from the v8bench/ directory:
 *   cd jit_perf_tests/v8bench
 *   node run_node.js
 *
 * Node differences from the original V8 shell:
 *   - no load() builtin → use require('fs') + vm.runInThisContext()
 *   - no alert() → console.log / process.stdout.write
 *   - no print() → console.log
 *   - performance.now() is available in Node ≥ 16 (used by base.js via Date.now())
 */

'use strict';

const fs   = require('fs');
const path = require('path');
const vm   = require('vm');

const DIR = __dirname;

/* Shims expected by the benchmark suite */
global.alert = console.log;
global.print = console.log;

/* load(filename) — evaluates a file in the global context, like the V8 shell */
global.load = function load(filename) {
    const fullPath = path.resolve(DIR, filename);
    const src = fs.readFileSync(fullPath, 'utf8');
    vm.runInThisContext(src, { filename: fullPath });
};

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
    console.log(name + ': ' + result);
}

function PrintError(name, error) {
    console.log('ERROR ' + name + ': ' + error);
    success = false;
}

function PrintScore(score) {
    if (success) {
        console.log('----');
        console.log('Score (version ' + BenchmarkSuite.version + '): ' + score);
    }
}

BenchmarkSuite.RunSuites({
    NotifyResult: PrintResult,
    NotifyError:  PrintError,
    NotifyScore:  PrintScore
});
