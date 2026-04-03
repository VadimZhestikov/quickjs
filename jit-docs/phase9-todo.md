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

- [x] Add field to `JSFunctionBytecode`:
  ```c
  uint16_t *stack_depth_tab;   /* [bc_len] depth before each opcode; 0xffff=unreachable */
  ```
- [x] In `compute_stack_size()`, save the existing `stack_level_tab` BFS result into
  `b->stack_depth_tab` instead of freeing it.  Free in `free_function_bytecode()`.
- [x] Add accessor in `quickjs-jit.h`:
  ```c
  const uint16_t *js_jit_fb_get_stack_depth_tab(JSFunctionBytecode *b);
  ```

### P9.0-B — Control flow annotation table

At each label patch site the compiler knows `(branch_pc, target_pc, kind)`.  Record
this into a side-table stored in `JSFunctionBytecode`.

- [x] Add to `quickjs-jit.h`:
  ```c
  typedef enum { JIT_CF_WHILE_LOOP=0, JIT_CF_DOWHILE_LOOP=1, JIT_CF_FOR_LOOP=2,
                 JIT_CF_FORIN_LOOP=3, JIT_CF_IF=4 } JSJITCFKind;
  typedef struct { uint32_t header_pc; uint32_t exit_pc; uint8_t kind; } JSJITCFAnnotation;
  ```
  (Actual enum names differ from original plan — uses `header_pc`/`exit_pc` instead
  of `branch_pc`/`target_pc` to better reflect while/do-while semantics.)
- [x] Add fields to `JSFunctionBytecode`:
  ```c
  JSJITCFAnnotation *cf_annotations;
  int                cf_annotation_count;
  ```
- [x] In `quickjs.c`, record CF annotations at parse time in a `DynBuf jit_cf_raw`
  on `JSFunctionDef` (9-byte records: `uint8 kind + int32 header_label + int32 exit_label`).
  4 call sites: for-in/of (`label_next`/`label_break`), while (`label_cont`/`label_break`),
  do-while (`label1`/`label_break`), for (`label_test`/`label_break`).
  After `resolve_labels()`, labels are resolved to final PCs and transferred to
  `JSFunctionBytecode::cf_annotations`.
- [x] Add accessor:
  ```c
  const JSJITCFAnnotation *js_jit_fb_cf_annotations(JSFunctionBytecode *b, int *count_out);
  ```
- [x] Free in `free_function_bytecode()`.

### P9.0-C — Tests

- [x] `make CONFIG_JIT=y test` — verify no leaks, no crashes.
- [x] Manual check: `QJS_JIT_KEEP_C=1 ./qjs --jit-aot` — `cf_annotations` populated;
  P9.3 structured emission emits `while(1) {` for while/do-while bodies.

### Definition of done
`JSFunctionBytecode` carries `stack_depth_tab` and `cf_annotations`.
No existing tests regress. ✓ P9.0-A complete. ✓ P9.0-B complete.

---

## P9.1 — Variable Names
**Estimated effort:** ~1 day  **Risk:** low  **Files:** `quickjs.c`, `quickjs-jit.c`

### Background

`JSFunctionBytecode::vardefs[i].var_name` holds the original JS identifier as a
`JSAtom`.  Layout: `vardefs[0..arg_count-1]` = arguments,
`vardefs[arg_count..arg_count+var_count-1]` = locals.  Names are present in all
non-stripped builds (stripped only when embedder sets `JS_STRIP_DEBUG`).

### Tasks

- [x] **P9.1-A** Add accessors in `quickjs.c` + declarations in `quickjs-jit.h`:
  ```c
  JSAtom js_jit_fb_get_local_atom(JSFunctionBytecode *b, int local_idx);
  JSAtom js_jit_fb_get_arg_atom  (JSFunctionBytecode *b, int arg_idx);
  const char *js_jit_atom_get_str(JSRuntime *rt, char *buf, int buf_size, JSAtom atom);
  ```

- [x] **P9.1-B** In `js_jit_gen_c()`, build `varnames[]` via `jit_build_varnames()`:
  - For each local `i` and each arg `j`, call `js_jit_atom_get_str`, validate ASCII,
    produce `_jsv_{name}_{i}` / `_jsi_{name}_{i}` / `_jsd_{name}_{i}`.
  - Non-ASCII or `JS_ATOM_NULL` falls back to numeric index.
  - `LNAME(idx)` / `ANAME(idx)` macros thread names into gen_preamble/gen_body.

- [x] **P9.1-C** Replaced all `_l[%d]`, `_li[%d]`, `_ld[%d]` array-style declarations
  in `gen_preamble()` with individual named variables.  `gen_body()` uses LNAME/ANAME
  macros throughout.  Argument names use `_jai_{name}_{i}` format.

- [x] **P9.1-D** Run `make test` and `make CONFIG_JIT=y test`. ✓

### Definition of done
Generated C for a function `function dot(ax,ay,bx,by)` contains individual named
variables (`_jai_ax_0`, `_jai_ay_1`, `_jsv_s_4`, etc.) instead of `_l[N]`/`_ld[N]`.
✓ P9.1 complete — verified with `QJS_JIT_KEEP_C=1` spot-check.

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

- [x] **P9.2-A** Obtain `stack_depth_tab` from the bytecode object (P9.0 prerequisite):
  ```c
  const uint16_t *sdt = js_jit_fb_get_stack_depth_tab(b);
  int d = (sdt && sdt[pc] != 0xffff) ? (int)sdt[pc] : 0;
  ```
  No simulation needed in the JIT — the compiler already filled the table.
  (Previously this was a ~60-line pre-pass; P9.0-A eliminates it.)

- [x] **P9.2-B** Replace preamble stack array with named temporaries:
  ```c
  // Before:  JSValue _s[N]; int _sp=0;
  // After:   JSValue _tsv0=JS_UNDEFINED; ... JSValue _tsv{N-1}=JS_UNDEFINED; int _sp=0;
  ```
  `_sp` is retained for the `_ex:` exception cleanup path only.

- [x] **P9.2-C** Each opcode emission uses `d = sdt[pc]` (depth before opcode) as a
  local `int` and formats `_tsv%d` with `d`, `d-1`, `d-2` etc.  No macro needed.

- [x] **P9.2-D** Updated every opcode case in `gen_body()` to use `_tsv{N}`:
  - **Pushes**: `_tsv{d} = X; _sp = d+1;`
  - **Pops (single)**: read `_tsv{d-1}`, set `_sp = d-1`
  - **Pops (two)**: `_b = _tsv{d-1}`, `_a = _tsv{d-2}`, result → `_tsv{d-2}`
  - **Peek (no pop)**: `_tsv{d-1}`
  - **Calls**: temp array `_ca{pc}[N] = {_tsv{d-N}, ...}` for argument passing

- [x] **P9.2-E** Exception cleanup in `gen_footer()` uses unrolled:
  ```c
  if(_sp>N-1){_FREE(_tsv{N-1});}  // for each slot from stack_size-1 down to 0
  ```
  Stack indexing is entirely static; `_sp` is only written for cleanup purposes.

- [x] **P9.2-F** `make CONFIG_JIT=y test` passes. V8bench score: 825–974 (confirmed
  no regression vs P8.6 baseline; DeltaBlue no longer fails "Projection 2").
  Bug fixed: `OP_sub` general path had `_b`/`_a` operands swapped (computed
  `right - left` instead of `left - right`); fixed by matching the add/mul/div
  parameter order (`d-1, d-2, ...`).

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

- [x] At the top of `gen_body()`, retrieve the annotation table built by P9.0-B:
  ```c
  int n_cf = 0;
  const JSJITCFAnnotation *cf_annots = js_jit_fb_cf_annotations(b, &n_cf);
  typedef struct { uint32_t header_pc, exit_pc; } P93Loop;
  P93Loop p93_active[16];
  int p93_depth = 0;
  ```
  Linear scan O(n_cf) per label emission to detect loop headers — no lookup array
  needed since n_cf is small (one entry per loop).

#### P9.3-B — Inline structured emitter (no recursion needed)

- [x] The existing linear `while (pc < bc_len)` loop is kept.  P9.3 augments it
  with loop-detection at label and goto sites:

  **At label emission** (`_L{pc}:;`): scan `cf_annots` for entries where
  `kind ∈ {JIT_CF_WHILE_LOOP, JIT_CF_DOWHILE_LOOP}` and `header_pc == pc`;
  if found, emit `while(1) {` and push `{header_pc, exit_pc}` onto `p93_active`.

  **At `OP_goto`/`OP_goto8`/`OP_goto16`**: if target == `p93_active[top].header_pc`,
  emit `} /* while */` (close loop, pop stack); if target == `p93_active[top].exit_pc`,
  emit `break;`.

  **At `OP_if_false`/`OP_if_true`**: if the branch target == current loop's `exit_pc`,
  emit `break;` (exit condition) instead of `goto _L{N}`.

  **Fallback**: any goto not matching a structured loop emits `goto _L{N};` as before.

- [x] For-loops (`JIT_CF_FOR_LOOP`) and for-in/of (`JIT_CF_FORIN_LOOP`) are left as
  goto-spaghetti intentionally: QJS bytecode lays out the increment block BEFORE
  the body in physical order, so a `while(1){}` wrapper would put the body outside
  the loop. Annotation records for these are stored but ignored by P9.3-B.

- [x] Guard: if `cf_annotations` is NULL (stripped build), `p93_depth` stays 0 and
  all emission falls through to existing `goto _L{N}` paths — no change.

- [x] Stack depth limit: `p93_active[16]` handles up to 16 nested while/do-while loops.

#### P9.3-C — Integration and testing

- [x] P9.3 augmentations wired into existing `gen_body()` linear emission loop.
- [x] `make CONFIG_JIT=y test` — passes (test_bjson.js ASan failure is pre-existing
  and unrelated to JIT; all other 8 test files exit 0).
- [x] Verified with `QJS_JIT_KEEP_C=1 ../../qjs --jit-aot run_qjs.js`: 30 of the
  generated C files contain `while(1) {` blocks.
- [x] V8bench cold run: **872**, warm run: **987** (vs P9.2 baseline ~874).

### Definition of done
Generated C files for while/do-while functions contain `while(1) { ... }` blocks.
30+ functions in V8bench are structured. ✓ P9.3 complete.
Cold: 872, Warm: 987 (interpreter baseline: 798).

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

- [x] `make CONFIG_JIT=y test` — full test suite passes (P9.0-A + P9.1)
- [x] `make CONFIG_JIT=y JIT_THRESHOLD_GCC=2 qjs && (cd jit_perf_tests/v8bench && ../../qjs --jit-aot run_qjs.js)` — Score: 871 (no regression vs P8.6)
- [x] Manual spot-check: `QJS_JIT_KEEP_C=1 ./qjs --jit-warmup script.js` — named vars confirmed
- [x] `make CONFIG_JIT=y test` — passes after P9.2 implementation
- [x] V8bench after P9.2: Score 825–974 (warm cache), no DeltaBlue failures
- [x] V8bench after P9.3: cold 872, warm 987 — improvement over P9.2 (825–974)
- [x] `while(1)` structured emission verified: 30 functions in V8bench JIT output contain `while(1) {`
- [ ] ASAN build: `make CONFIG_JIT=y CONFIG_ASAN=y qjs && ./qjs --jit-aot tests/test_closure.js` — no memory errors
