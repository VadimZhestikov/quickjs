# Phase 3 — TCC Tier-1 JIT Compilation

## Overview

Phase 3 connects the C code generator (Phase 2) to `libtcc`, producing a
native function pointer the first time a function reaches `JIT_THRESHOLD_TCC`
calls.

## Implementation

### js_jit_new_tcc() — Phase 3.1

Creates and configures a `TCCState`:

```c
TCCState *s = tcc_new();
tcc_set_output_type(s, TCC_OUTPUT_MEMORY);   // in-process execution
tcc_set_error_func(s, NULL, jit_tcc_error);  // silent; check rc
tcc_add_include_path(s, JIT_INCLUDE_DIR);    // find quickjs.h
tcc_add_symbol(s, "js_jit_rt", &js_jit_rt); // vtable symbol
```

`JIT_INCLUDE_DIR` is set at compile time by the Makefile to the quickjs
source directory.  This lets the generated C file `#include "quickjs.h"`.

### js_jit_compile_tcc() — Phase 3.2–3.3

```
js_jit_is_eligible(b)  → bail if not eligible
js_jit_fb_jit_no_compile(b) → bail if already marked
js_jit_fb_get_func(b)  → bail if already compiled (race guard)

js_jit_gen_c(b, &cb, fname, ...)  → generate C source
tcc_new() + configure
tcc_compile_string(s, cb.buf)      → parse + codegen
tcc_relocate(s, TCC_RELOCATE_AUTO) → mmap + patch relocations
tcc_get_symbol(s, fname)           → native function pointer
js_jit_fb_set_func(b, f, s, 1)    → atomic RELEASE store
```

The `TCCState *s` is kept alive (stored as `jit_handle`) until
`free_function_bytecode()` calls `js_jit_free_bytecode()` which calls
`tcc_delete(handle)`.  `tcc_delete` frees the mmap'd executable memory.

### Race guard (Phase 3.3)

Two threads can race to compile the same function.  The guard is:
```c
if (js_jit_fb_jit_no_compile(b)) return;
if (js_jit_fb_get_func(b) != NULL) return;
```

In the worst case both threads compile simultaneously; the second
`set_func` call is silently lost (the first TCCState is leaked).  For
Phase 3 this is acceptable; Phase 4 will add a proper CAS spin.

## Build

```sh
make CONFIG_JIT=y JIT_THRESHOLD_TCC=2 qjs  # threshold=2 for quick testing
./qjs jit-tests/test_jit_tcc.js
make CONFIG_JIT=y JIT_THRESHOLD_TCC=2 test  # full suite
```

## Symbols registered with TCC

| Symbol | Points to |
|---|---|
| `js_jit_rt` | `const JSJITRuntime js_jit_rt` (vtable) |

All other QuickJS API functions (`JS_NewInt32`, `JS_ToBool`, etc.) are
public ABI and resolved automatically by TCC's linker from the parent
process image.

## Performance note

TCC compiles at roughly 30 MB/s, so a typical 50-instruction function
takes < 1 µs to compile.  The compilation happens synchronously on the
calling thread; no GIL is held during TCC work.
