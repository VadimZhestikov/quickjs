# Phase 3 — TCC Tier-1 (Retrospective)

**Commit:** `fcbb456`  
**Status:** Removed in Phase 4.  Documented here for reference.

---

## What it was

Phase 3 wired up the first live compiler: TCC (Tiny C Compiler) 0.9.27 running
in-process.  When `jit_call_count` reached `JIT_THRESHOLD_TCC` (default 2), the hot
function's C source was compiled synchronously using `tcc_compile_string()` and the
resulting machine code was installed atomically.

```
interpret → [threshold hit] → tcc_compile_string() → tcc_relocate() → install
             (synchronous, ~50 µs)
```

The appeal was zero external process overhead: TCC lives in the same address space as
`qjs`, so compilation took microseconds rather than the 2–5 seconds that forking GCC
requires.

---

## Implementation sketch

```c
/* Phase 3 compile path (removed in Phase 4) */
TCCState *s = tcc_new();
tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
tcc_add_include_path(s, JIT_INCLUDE_DIR);
tcc_set_lib_path(s, tcc_lib_path);

/* tcc_compile_string compiles the entire C source at once */
if (tcc_compile_string(s, c_source) < 0) {
    tcc_delete(s);
    js_jit_fb_set_no_compile(b);
    return;
}

/* Relocate: allocates executable memory and patches references */
if (tcc_relocate(s, TCC_RELOCATE_AUTO) < 0) {
    tcc_delete(s);
    js_jit_fb_set_no_compile(b);
    return;
}

JSJITFunc f = (JSJITFunc)tcc_get_symbol(s, fname);
if (!f) { tcc_delete(s); js_jit_fb_set_no_compile(b); return; }

/* Store the TCCState* as the handle (must keep alive while code runs) */
js_jit_fb_set_func(b, f, (void *)s, /*tier=*/1);
```

Cleanup: `tcc_delete(s)` was called in `js_jit_fb_free()` when the bytecode object
was freed.

---

## Why TCC ≈ interpreter (≤ 1.0× speedup)

TCC produces unoptimised x86-64 with:

| Limitation | Impact |
|---|---|
| No register allocation | All C locals spilled to stack; every `JSValue _l[i]` is a memory load/store |
| No CSE | Repeated reads of `_l[i]` in one block each generate a separate load |
| No loop optimisation | No LICM, no unrolling, no vectorisation |
| No inlining | `JS_DupValue`, `JS_FreeValue`, `JS_NewInt32` all remain full calls |

The interpreter is compiled by GCC -O2 with `DIRECT_DISPATCH` (computed-goto).
GCC optimises the interpreter's entire dispatch loop as a unit, keeps hot values in
registers across opcodes, and folds the refcount operations into the surrounding code.

TCC code has none of this.  A tight inner loop that the GCC interpreter handles in
~3 instructions might take 10–15 in TCC output.

**Phase 3 v8bench result (TCC tier-1, threshold=2):**

```
Richards:    1020  (vs interpreter 920)   +11%   (noise-range win)
DeltaBlue:    874  (vs interpreter 884)   −1%    (equivalent)
Crypto:      1146  (vs interpreter 1143)  +0%
RayTrace:    1295  (vs interpreter 1322)  −2%
EarleyBoyer: 1568  (vs interpreter 1572)  −0%
RegExp:       396  (vs interpreter 402)   −1%
Splay:       2506  (vs interpreter 2491)  +1%
Score:       1065  (vs interpreter 985)   +8%    (noise-range)
```

All values within ±10% WSL2 noise.  TCC tier-1 was **statistically equivalent to the
interpreter** across all benchmarks.

---

## Why it was removed

Phase 4 replaced TCC with background GCC compilation.  At that point keeping TCC alive
added complexity with no observable speedup.  The decision was to go directly:

```
interpret → [GCC threshold] → GCC background → install tier-2
```

This simplified the codebase (no `libtcc.h` dependency, no `TCCState` lifetime
management) and let Phase 5/6 focus on generating C that GCC could optimise — with
no need to worry about TCC's limitations.

---

## Symbols registered with TCC

TCC resolves most QuickJS API functions (`JS_NewInt32`, `JS_ToBool`, `JS_DupValue`, ...)
automatically from the parent process image via `/proc/self/exe` symbol lookup.

One symbol must be registered explicitly before `tcc_compile_string`:

```c
tcc_add_symbol(s, "js_jit_rt", &js_jit_rt);
```

`js_jit_rt` is the vtable singleton used by `_RT->op(...)` in the generated C.
Without this registration, TCC's linker would fail to resolve `js_jit_rt` at
`tcc_relocate()` time.

---

## Could it come back?

Yes.  TCC tier-1 would be valuable if:
- The JIT cache (Phase 7) is not yet implemented — TCC closes the "cold start dead zone"
  between first-threshold and GCC finishing
- Phase 6.2 IC is present — TCC + IC eliminates the property-lookup hash walk, which
  gives ~1.3–1.5× on DeltaBlue/Richards even without register allocation
- Embedded targets where forking GCC is not available

Re-adding TCC requires ~50 lines: restore `#include "libtcc.h"`, the compile function,
`jit_tier==1` in `js_jit_fb_free()`, and a separate `JIT_THRESHOLD_TCC` build flag.
