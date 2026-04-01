# Phase 1.1 — Skeleton Files

## Files created

| File | Purpose |
|---|---|
| `quickjs-jit.h` | Public interface: `JSJITFunc`, `JSJITRuntime`, `js_jit_rt`, lifecycle API |
| `quickjs-jit.c` | Implementation: vtable wrappers, stub compile/free functions |
| `jit-tests/` | Per-phase JS test files |
| `jit-docs/` | Per-phase technical notes |

## Design decisions

### Separate translation unit
`quickjs-jit.c` is compiled separately and linked with `quickjs.o`.  It
includes `quickjs.h` (the public header) rather than the full internals of
`quickjs.c`.  This keeps the build clean: internal symbols that must be
reachable are forward-declared in `quickjs-jit.c`.

### JSJITRuntime vtable
Generated C code (compiled by TCC or GCC at runtime) cannot reference
`static inline` functions — the compiler that built `qjs` may have inlined
them completely, leaving no symbol to link against.  The vtable pattern
solves this: every runtime call goes through `__jit_rt`, a single
`const JSJITRuntime *` symbol registered with each `TCCState`.

Critical wrappers:
- `jit_rt_dup` → `JS_DupValue` (static inline in quickjs.h)
- `jit_rt_free` → `JS_FreeValue` (static inline in quickjs.h)

### JSJITFunc calling convention
```c
typedef JSValue (*JSJITFunc)(JSContext *ctx,
                             JSValue    this_val,
                             int        argc,
                             JSValue   *argv,
                             JSValue   *cpool,
                             JSVarRef **var_refs);
```
Matches the arguments already available in `JS_CallInternal` at the point
where the hot-path dispatcher fires, so zero marshalling overhead.

## Build
```sh
make CONFIG_JIT=y        # skeleton compiles, stubs prevent any JIT from firing
make CONFIG_JIT=y test   # full test suite must still pass
./qjs jit-tests/test_jit_skeleton.js
```
