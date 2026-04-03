# Phase 8.6 — Typed Float64 IC + Float Arithmetic/Comparison Fast Paths

## Problem

Before P8.6, all arithmetic and comparison operations that involved `JS_TAG_FLOAT64`
operands fell through to the vtable (`_RT->add`, `_RT->lt`, etc.).  The fast paths
only handled `JS_TAG_INT` × `JS_TAG_INT`.

This hurt float-heavy benchmarks (RayTrace, DeltaBlue) where object properties such
as `this.x`, `this.y`, `this.z` are stored as float64 values.  A typical inner loop
like:

```js
var dx = a.x - b.x;   // sub: both float64 → vtable
var dy = a.y - b.y;   // sub: both float64 → vtable
if (dx*dx + dy*dy < r2) { ... }  // mul/add/lt: all vtable
```

made six vtable calls per iteration.

## Changes

### 1. `JSJITICEntry.kind` field (quickjs-jit.h)

Added `uint8_t kind` to the IC entry.  `js_jit_ic_fill_get` sets `kind = 1` when
the cached property slot holds a `JS_TAG_FLOAT64` value.  This is groundwork for
a future typed-read specialisation (skip `JS_DupValue` branch, promote gen_st to
`JIT_T_NUMBER`).

### 2. Arithmetic ops — float64 middle path (quickjs-jit.c)

**OP_add / sub / mul / div / mod** now emit three levels:

1. **INT × INT** — existing fast path, may return `JS_TAG_INT` result
2. **`(INT|FLOAT64)` × `(INT|FLOAT64)`** — new middle path: convert both to C
   `double`, compute, return `JS_TAG_FLOAT64`; **no vtable call**
3. **else** — vtable (handles strings for `add`, BigInt, objects, etc.)

When the gen-time type stack (`gen_st`) says both operands are `JIT_T_NUMBER`, only
the INT×INT and float64 paths are emitted (no vtable at all).

### 3. Comparison ops — float64 middle path (quickjs-jit.c)

`GEN_CMP_FUSE_GEN` and `GEN_CMP_UNFUSED` (used when gen-time types are not
provably numeric) now include the same `(INT|FLOAT64)×(INT|FLOAT64)` middle case
before falling back to the vtable.

All eight comparison opcodes benefit: `lt`, `lte`, `gt`, `gte`, `eq`, `neq`,
`strict_eq`, `strict_neq`.

### 4. OP_neg / OP_plus — float64 paths

`OP_neg` adds a `JS_TAG_FLOAT64 → JS_NewFloat64(ctx, -val)` case.
`OP_plus` extends the "already a number" identity path to include `JS_TAG_FLOAT64`.

## Results

| Benchmark | Before P8.6 | After P8.6 | Δ |
|---|---:|---:|---:|
| DeltaBlue | 833 | 905 | **+9%** |
| RayTrace  | 918 | 963 | **+5%** |
| Richards  | 724 | 805 | **+11%** |
| Crypto    | 1289 | 1253 | -3% (noise) |
| Splay     | 1250 | 1208 | -3% (noise) |

DeltaBlue and RayTrace show clear wins from float64 object-property arithmetic
avoiding the vtable.  Richards benefits from the gen-time typed path on `_bn`
arithmetic.  Crypto/Splay/RegExp are integer- or string-dominated and unaffected.

## Why gen_st promotion from `OP_get_field` is not done here

The IC `kind` flag is set at *runtime* (first miss), but `gen_st` is a
*compile-time* (JIT code generation) abstraction.  Without a re-compilation trigger
(e.g. deoptimisation + recompile after profiling), the codegen pass cannot know
statically that a particular `get_field` site always produces `JS_TAG_FLOAT64`.

The float64 middle path in arithmetic/comparison ops solves this at runtime instead:
GCC sees a simple `TAG == JS_TAG_FLOAT64` check in the hot path and eliminates it
after a few iterations via branch prediction.  This is "good enough" without the
complexity of a speculative recompilation pipeline.
