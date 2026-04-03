# Phase 9 — Stackless IR, CF Structuring, Variable Names, Typed Temporaries

## Overview

Phase 9 moves the JIT code generator from "opcode translator" to "decompiler":
instead of emitting goto-spaghetti C with an explicit value stack, it reconstructs
the original structure of the JavaScript — named variables, real loops, real
conditionals — giving GCC the information it needs to apply its full optimisation
arsenal (LICM, vectorisation, loop unrolling, CSE across the full expression tree).

### Compiler-assisted approach

Rather than re-deriving structure from bytecode (dominator analysis, stack simulation),
Phase 9 taps the information the quickjs.c compiler already has at compile time:

- `compute_stack_size()` already walks every opcode tracking depth — it just doesn't
  save the per-PC table.  One new field + ~10 lines makes it available to the JIT.
- The label patch sites (`emit_label` / `patch_label`) know the exact `(branch_pc →
  target_pc, kind)` for every `if`, `for`, `while`, and `switch` at the moment they
  are resolved.  A compact side-table records this for the JIT.

This eliminates ~350 lines of dominator/CFG analysis from the JIT and reduces P9.3
from ~5 days to ~2 days, at the cost of ~60 lines in `quickjs.c`.

**Dependencies between sub-phases:**
```
P9.0 (quickjs.c annotations)  ── do first; P9.2 and P9.3 depend on it
P9.1 (variable names)         ── independent of P9.0, do in parallel
P9.2 (stackless)              ── requires P9.0 (stack_depth_tab field)
  └──► P9.4 (typed temps)        requires P9.2
P9.3 (CF structuring)         ── requires P9.0 (CF annotation table)
```

Suggested order: P9.0 + P9.1 (parallel) → P9.2 → P9.3 → P9.4

---

## P9.0 — Compiler-side Annotations in `quickjs.c`
**Estimated effort:** ~1 day  **Risk:** low  **Files:** `quickjs.c`, `quickjs-jit.h`

### P9.0-A — Per-PC stack depth table

`compute_stack_size()` (~line 35128 in `quickjs.c`) already simulates the opcode
stream to find the maximum stack depth.  Extend it to save the per-PC depths.

- [ ] Add field to `JSFunctionBytecode`:
  ```c
  int8_t *stack_depth_tab;   /* [bc_len] depth before each opcode; NULL if stripped */
  ```
- [ ] In `compute_stack_size()`, allocate `js_malloc(ctx, bc_len)` and store depth
  before applying each opcode's net effect.  Free in `js_free_function_def()` and
  `free_function_bytecode()`.
- [ ] Add accessor in `quickjs-jit.h`:
  ```c
  static inline int8_t *js_jit_fb_stack_depth_tab(JSFunctionBytecode *b) {
      return b->stack_depth_tab;
  }
  ```

### P9.0-B — Control flow annotation table

At each label patch site the compiler knows `(branch_pc, target_pc, kind)`.  Record
this into a side-table stored in `JSFunctionBytecode`.

- [ ] Add to `quickjs-jit.h`:
  ```c
  typedef enum { JIT_CF_LOOP=0, JIT_CF_IF=1, JIT_CF_ELSE=2, JIT_CF_SWITCH=3 } JSJITCFKind;
  typedef struct { uint32_t branch_pc, target_pc; uint8_t kind; } JSJITCFAnnotation;
  ```
- [ ] Add fields to `JSFunctionBytecode`:
  ```c
  JSJITCFAnnotation *cf_annotations;
  int                cf_annotation_count;
  ```
- [ ] In `quickjs.c`, at the three label-patch sites (loop back-edge, if/else forward
  jump, switch case jump), append to a `DynBuf` that is stored into
  `cf_annotations` after bytecode finalisation.
- [ ] Add accessor:
  ```c
  JSJITCFAnnotation *js_jit_fb_cf_annotations(JSFunctionBytecode *b, int *count_out);
  ```
- [ ] Free in `free_function_bytecode()`.

### P9.0-C — Tests

- [ ] `make CONFIG_JIT=y test` — verify no leaks, no crashes.
- [ ] Manual check: `--jit-dump` on a function with a `for` loop; confirm
  `stack_depth_tab` non-NULL and `cf_annotations` contains a `JIT_CF_LOOP` entry.

### Definition of done
`JSFunctionBytecode` carries `stack_depth_tab` and `cf_annotations` after compilation.
No existing tests regress.

---

## P9.1 — Variable Names
**Estimated effort:** ~1 day  **Risk:** low  **Files:** `quickjs.c`, `quickjs-jit.c`

### Background

`JSFunctionBytecode::vardefs[i].var_name` holds the original JS identifier as a
`JSAtom`.  Layout: `vardefs[0..arg_count-1]` = arguments,
`vardefs[arg_count..arg_count+var_count-1]` = locals.  Names are present in all
non-stripped builds (stripped only when embedder sets `JS_STRIP_DEBUG`).

### Tasks

- [ ] **P9.1-A** Add accessors in `quickjs.c` + declarations in `quickjs-jit.h`:
  ```c
  JSAtom js_jit_fb_get_local_atom(JSFunctionBytecode *b, int local_idx);
  JSAtom js_jit_fb_get_arg_atom  (JSFunctionBytecode *b, int arg_idx);
  ```
  Implementation: `b->vardefs[b->arg_count + idx].var_name` and `b->vardefs[idx].var_name`.

- [ ] **P9.1-B** In `gen_body()`, before the main opcode loop, build a name table:
  - For each local `i` and each arg `j`, call `JS_AtomToCString`, validate ASCII,
    produce `_jsv_{name}_{i}` / `_jsi_{name}_{i}` / `_jsd_{name}_{i}`.
  - Non-ASCII or `JS_ATOM_NULL` falls back to `_jsv_{i}`.
  - Store in `char **local_cnames` and `char **arg_cnames` (freed after codegen).

- [ ] **P9.1-C** Replace all `_l[%d]`, `_li[%d]`, `_ld[%d]` format strings in
  `gen_preamble()` and `gen_body()` with `%s` using `local_cnames[i]`.
  Likewise for argument names wherever `_a[%d]` / `_ai[%d]` / `_aim` are used.

- [ ] **P9.1-D** Run `make test` and `make CONFIG_JIT=y test`.

### Definition of done
Generated C for a function `function dot(ax,ay,bx,by)` contains
`double _jsd_ax_0, _jsd_ay_1, _jsd_bx_2, _jsd_by_3;` instead of `double _ld[4];`.

---

## P9.2 — Stackless Value Stack
**Estimated effort:** ~3 days  **Risk:** medium  **Files:** `quickjs-jit.c`

### Background

The value stack `JSValue _s[N]` is an array accessed through a pointer `_sp`.
GCC cannot eliminate its loads/stores because the pointer may alias other memory.
For all JIT-eligible functions, the stack depth is provably 0 at every basic-block
boundary (for-in/for-of, which carry iterator state across loop headers, are already
excluded from the JIT).  Therefore no spilling is needed — the transform is purely
local within each basic block.

### Tasks

- [ ] **P9.2-A** Obtain `stack_depth_tab` from the bytecode object (P9.0 prerequisite):
  ```c
  int8_t *stack_depth_tab = js_jit_fb_stack_depth_tab(b);
  assert(stack_depth_tab != NULL);  /* set by compute_stack_size() */
  ```
  No simulation needed in the JIT — the compiler already filled the table.
  (Previously this was a ~60-line pre-pass; P9.0-A eliminates it.)

- [ ] **P9.2-B** Replace preamble stack array with named temporaries:
  ```c
  // Before
  jit_buf_printf(cb, "    JSValue _s[%d];\n    int _sp=0;\n", stack_size);
  // After
  for (int d = 0; d < stack_size; d++)
      jit_buf_printf(cb, "    JSValue _tsv%d;\n", d);
  ```

- [ ] **P9.2-C** Macro helpers for use inside opcode cases:
  ```c
  // depth at current pc from stack_depth_tab[pc]
  #define _TS_TOP(tab,pc)    _tsv ## tab[pc]   // won't work directly — see note
  ```
  In practice, each opcode emission passes `stack_depth_tab[pc]` as a local `int _d`
  and formats `_tsv%d` with `_d`, `_d-1`, `_d-2` etc.

- [ ] **P9.2-D** Update every opcode case in `gen_body()` to use `_tsv{N}`:
  - **Pushes**: `jit_buf_printf(cb, "    _tsv%d = X;\n", _d);` where `_d` is the
    pre-opcode depth.
  - **Pops (single)**: read `_tsv%d` with `_d-1`.
  - **Pops (two)**: `_b = _tsv{_d-1}`, `_a = _tsv{_d-2}`.
  - **Peek (no pop)**: `_tsv{_d-1}`.
  Priority order for cases: arithmetic → comparisons → get_loc/put_loc →
  get_arg/put_arg → get_field/put_field → calls → control flow → rest.

- [ ] **P9.2-E** Remove the `int _sp = 0;` declaration from the generated code.
  The runtime variable `_sp` disappears entirely — all indexing is now static.

- [ ] **P9.2-F** Verify `make test`.  Then run V8bench and compare to pre-P9.2 baseline.

- [ ] **P9.2-G** Typed variant declaration for typed gen_st slots:
  When `local_type` or `gen_st` at a given depth slot is `JIT_T_NUMBER`, also declare
  `double _tsd{N}` alongside `JSValue _tsv{N}`.  Arithmetic/comparison ops that emit
  the float64 path use `_tsd{N}` directly; JSValue ops use `_tsv{N}` (with a
  `_tsv{N} = JS_NewFloat64(ctx, _tsd{N})` boxing step when needed).
  This is the prerequisite for P9.4.

### Definition of done
The generated C for any compiled function contains no `_s[]` or `_sp` references.
GCC `-S` output shows XMM register usage for float operations without stack-to-memory
spills between consecutive arithmetic ops.

---

## P9.3 — Control Flow Structuring
**Estimated effort:** ~2 days  **Risk:** medium  **Files:** `quickjs-jit.c`  **Requires:** P9.0

### Background

`gen_body()` currently emits `_L{pc}:;` goto-labels and `goto _L{pc};` for all
branches.  GCC's vectoriser, LICM, and loop unroller require canonical `while`/`for`
form.  The P9.0-B annotation table records the original control flow structure directly
from the compiler — no dominator analysis or CFG reconstruction is needed.

### Tasks

#### P9.3-A — Load CF annotation table from bytecode object (~10 lines)

- [ ] At the top of `gen_body()`, retrieve the annotation table built by P9.0-B:
  ```c
  int n_cf;
  JSJITCFAnnotation *cf = js_jit_fb_cf_annotations(b, &n_cf);
  ```
  Build two lookup arrays indexed by `branch_pc` for O(1) access during emission:
  - `cf_by_branch[pc]` → pointer to annotation (or NULL)
  - `cf_loop_headers[]` → sorted array of loop header PCs (target_pc of LOOP entries)

  No CFG construction, no dominator tree, no post-dominator analysis needed —
  the compiler already recorded the structure in P9.0-B.

#### P9.3-B — Recursive structured emitter (~200 lines)

- [ ] Replace the `while (pc < bc_len)` linear loop with:
  ```c
  static void emit_region(GenCtx *g, int block_idx, int end_block_idx);
  ```
- [ ] Implement cases:
  - **Loop header**: `emit "while(1){" → emit_region(body, header) → emit "}"` +
    record the loop on a stack so inner `goto header` → `continue` and
    `goto exit` → `break`.
  - **If/else**: emit condition block, `"if(_cond){"`, emit then-region to join,
    optional `"}else{"`, emit else-region to join, `"}"`, continue from join.
  - **If-only**: same without else arm.
  - **Switch (`OP_switch`)**: `"switch(val){"` + iterate cases + `"}"`.
  - **Sequential**: emit block contents, advance to single successor.
- [ ] `break` / `continue` detection: a `goto` whose target is the current loop
  header → emit `continue;`; target outside the loop → emit `break;`.
  Multi-level labelled break → emit `goto _L{N};` (fallback).
- [ ] Fallback: any branch PC not found in `cf_by_branch` → emit the existing
  `goto _L{pc};` label output (never fails, always correct).
- [ ] Guard: if `cf_annotations` is NULL (stripped build or very old bytecode),
  fall back silently to the old linear goto emitter.

#### P9.3-C — Integration and testing

- [ ] Wire `emit_region` into `gen_body()`: load annotation table (P9.3-A), then
  call `emit_region(0, bc_len)` instead of the `while (pc < bc_len)` loop.
- [ ] Run `make test`.
- [ ] Verify with `--jit-dump` that a JS `for` loop produces `while(1){...break;}` or
  `for(...){}` in the generated C.
- [ ] Run V8bench and compare to P9.2 baseline.

### Definition of done
A JS function containing `for (let i=0; i<n; i++) s += a[i]*b[i]` generates C with a
recognisable `while` or `for` loop.  GCC `-S` output shows a vectorised loop body
(e.g. `vmovupd`, `vaddpd` instructions) when operands are typed float64.

---

## P9.4 — Typed Stack Temporaries
**Estimated effort:** ~2 days  **Risk:** medium  **Files:** `quickjs-jit.c`  **Requires:** P9.2

### Background

P9.2 introduces `JSValue _tsv{N}` for every stack slot.  P9.4 adds a typed variant
`double _tsd{N}` for slots provably holding `JS_TAG_FLOAT64`, enabling the IC float
read (P8.6 `kind=1`) to push a raw `double` directly — no JSValue allocation or tag
check in the arithmetic that follows.

### Tasks

- [ ] **P9.4-A** Track typed slot assignments in the gen_st second-pass:
  When an IC get_field hit fires and `_ic{pc}.kind == 1`, record `gen_st[gen_sp] =
  JIT_T_NUMBER` at the push point.  In P9.2 the corresponding declaration becomes
  `double _tsd{N}` instead of `JSValue _tsv{N}`.

- [ ] **P9.4-B** Add `js_jit_ic_read_f64(JSValue obj, uint32_t slot) → double`
  in `quickjs.c` / `quickjs-jit.h`:
  ```c
  double js_jit_ic_read_f64(JSValue obj, uint32_t slot) {
      return JS_VALUE_GET_FLOAT64(JS_VALUE_GET_OBJ(obj)->prop[slot].u.value);
  }
  ```

- [ ] **P9.4-C** Update `OP_get_field` codegen to emit two paths based on kind at
  the push site:
  ```c
  // kind==0: existing path — JSValue _tsv{d} = ic_read(ctx, _o, slot)
  // kind==1: typed path   — double _tsd{d}  = js_jit_ic_read_f64(_o, slot)
  // (runtime branch on _ic{pc}.kind — GCC eliminates after first fill)
  ```
  Record `JIT_T_NUMBER` in gen_st for the slot to propagate to arithmetic.

- [ ] **P9.4-D** In arithmetic ops (OP_add/sub/mul/div), when gen_st says the
  operand slots are typed (`_tsd{N}`): emit `_tsd{N} OP _tsd{M}` directly with
  no `JS_VALUE_GET_TAG` check at all — the type is statically guaranteed.

- [ ] **P9.4-E** Boxing on use: when a typed slot `_tsd{N}` must be passed as a
  JSValue (function call argument, return value, store to non-typed slot), emit:
  ```c
  JSValue _tsv{N} = JS_NewFloat64(ctx, _tsd{N});
  ```

- [ ] **P9.4-F** Run `make test`.  Run V8bench and compare to P9.3 baseline.

### Definition of done
For a function `dot(a,b) { return a.x*b.x + a.y*b.y + a.z*b.z; }` with float64
properties, the generated C contains only `double` arithmetic and a single
`JS_NewFloat64` at the return — no `JS_VALUE_GET_TAG` checks inside the body.

---

## Milestone summary

| Sub-phase | Deliverable | Effort | Risk | Expected V8bench |
|---|---|---:|---|---:|
| P9.0 Compiler annotations | `stack_depth_tab` + `cf_annotations` in bytecode | 1 day | low | no change |
| P9.1 Variable names | Human-readable generated C | 1 day | low | no change |
| P9.2 Stackless | No `_s[]`; named `_tsv{N}` | 3 days | medium | +30–60% |
| P9.3 CF structuring | `if`/`while`/`for` in output | 2 days | medium | +40–100% |
| P9.4 Typed temps | Zero tag-checks for float64 | 2 days | medium | +30–80% on float benchmarks |
| **Total** | | **~9 days** | | **~2–3× overall** |

## Testing checklist (run after each sub-phase)

- [ ] `make CONFIG_JIT=y test` — full test suite passes
- [ ] `make CONFIG_JIT=y JIT_THRESHOLD_GCC=2 qjs && (cd jit_perf_tests/v8bench && ../../qjs --jit-aot run_qjs.js)` — 3 runs, record best
- [ ] Manual spot-check: `./qjs --jit-dump script.js` — generated C looks correct
- [ ] ASAN build: `make CONFIG_JIT=y CONFIG_ASAN=y qjs && ./qjs --jit-aot tests/test_closure.js` — no memory errors
