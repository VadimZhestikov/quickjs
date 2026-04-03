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

## P9.4 — Typed Stack Temporaries ✓ COMPLETE
**Actual effort:** ~3 days  **Files:** `quickjs-jit.c`  **Requires:** P9.2

### Background

P9.2 introduces `JSValue _tsv{N}` for every stack slot.  P9.4 adds a typed variant
`double _tsd{N}` for slots provably holding a numeric value, enabling arithmetic
ops to work directly on raw doubles with no JSValue tag-check or allocation.

### What was implemented (diverged from original plan)

Rather than type-tracking only via IC kind, P9.4 infers typed slots from the
gen_st[] array which already tracks integer and number types via `jit_infer_types`.

**Key design:**

- `double _tsd{N}` declared alongside `JSValue _tsv{N}` in the function preamble
- Arithmetic ops (add/sub/mul/div/mod, comparisons) check `_bn` flag:
  ```c
  int _bn = (gen_sp <= d && _GS_TOP2()>=JIT_T_NUMBER && _GS_TOP2()<=JIT_T_INT
             && _GS_TOP()>=JIT_T_NUMBER && _GS_TOP()<=JIT_T_INT);
  ```
  When `_bn`, emit `_tsd{N} OP _tsd{M}` directly — zero tag checks.
- `_P94_ENSURE(slot)`: boxes `_tsd{slot}` into `_tsv{slot}` before JSValue-consuming ops:
  ```c
  #define _P94_ENSURE(slot) do { \
      if ((slot) < d && gen_sp > (slot) && \
          gen_st[(slot)] >= JIT_T_NUMBER && gen_st[(slot)] <= JIT_T_INT) \
          jit_buf_printf(cb, "{ double _dv=_tsd%d; _tsv%d=...; }\n", ...); \
  } while(0)
  ```
  Guard `gen_st[(slot)] <= JIT_T_INT` prevents false firing on `JIT_T_SELF_FUNC (=3)`.

**Post-increment typed fast path** (prevents loop-counter corruption):
```c
// P9.4: when top is typed, keep _tsd slots valid
{ double _da=_tsd{d-1}; _tsd{d-1}=_da; _tsd{d}=_da+1.0; _sp=d+1; }
```

**Label boundary invariant**: all typed slots are boxed into `_tsv` before any branch;
label entry resets gen_st to JSVAL and gen_sp = sdt[pc].

**Bitwise ops** push `JIT_T_JSVAL` (not INT) since result lives in `_tsv`.

**Array construction** (`OP_array_from`) boxes all element slots before use.

**gen_sp drift correction**: if gen_sp > d (pop-ops missing from gen_st), reset
gen_st to JSVAL and gen_sp = d.

### Tasks

- [x] **P9.4-A** Typed slot tracking via gen_st; double _tsd{N} in preamble
- [x] **P9.4-B** `_P94_ENSURE` macro for boxing typed → JSValue when needed
- [x] **P9.4-C** Arithmetic fast paths: `_tsd += _tsd` etc.
- [x] **P9.4-D** Comparison fast paths: `GEN_CMP_FUSE_TSD` for `_tsd OP _tsd`
- [x] **P9.4-E** Label boundary boxing + gen_sp reset to sdt[pc]
- [x] **P9.4-F** `OP_post_inc/post_dec` typed fast path (prevents counter corruption)
- [x] **P9.4-G** `OP_array_from` boxes typed element slots before JS_SetPropertyUint32
- [x] **P9.4-H** All tests pass: test_loop, test_language, test_closure, test_builtin ✓
- [x] **P9.4-I** DeltaBlue warm-run gc_obj_list assertion fixed (JIT_T_SELF_FUNC guard)

### Critical bug fixed: JIT_T_SELF_FUNC
`JIT_T_SELF_FUNC = 3 >= JIT_T_NUMBER = 1`, so without the `<= JIT_T_INT` guard,
`_P94_ENSURE` would fire on functions that load their own name (e.g., a recursive
call `EqualityConstraint.superConstructor.call(...)`), boxing `_tsd{slot}=0.0`
into `_tsv{slot}` and overwriting the live function object reference.  This caused
TypeError after ~150 JIT invocations.  Fixed by checking `>= JIT_T_NUMBER && <= JIT_T_INT`.

### V8bench results

| Run | Richards | DeltaBlue | Crypto | RayTrace | EarleyBoyer | RegExp | Splay | **Score** |
|-----|----------|-----------|--------|----------|-------------|--------|-------|-----------|
| P9.3 cold | – | – | – | – | – | – | – | **872** |
| P9.3 warm | – | – | – | – | – | – | – | **987** |
| P9.4 cold #1 | 714 | 614 | 825 | 895 | 1988 | 212 | 1216 | **773** |
| P9.4 cold #2 | 628 | 512 | 934 | 750 | 905 | 323 | 1324 | **706** |
| P9.4 warm #1 | 969 | 746 | 1422 | 812 | 1052 | 405 | 1291 | **895** |
| P9.4 warm #2 | 807 | 628 | 1162 | 718 | 1116 | 317 | 1417 | **801** |
| Interp | 875 | 712 | 932 | 819 | 1320 | 339 | 2169 | **896** |

P9.4 warm runs average ~848, vs P9.3 warm 987 (regression on EarleyBoyer/Richards
but improvement on Crypto/Splay where float fast paths help).

### Definition of done ✓
All four test suites (test_loop, test_language, test_closure, test_builtin) pass.
V8bench runs cleanly cold and warm with no assertion failures.
DeltaBlue and chainTest(100) × 400 iterations: no TypeError, no gc_obj_list leak.
P9.4 complete.

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
- [x] ASAN build: `make CONFIG_JIT=y CONFIG_ASAN=y qjs && ./qjs --jit-aot tests/test_closure.js` — no memory errors
- [x] V8bench after P9.4: cold ~700-800, warm ~800-895 — no crashes cold or warm
- [x] All 4 test suites pass (test_loop, test_language, test_closure, test_builtin)
- [x] DeltaBlue chainTest(100) × 400 iterations: no TypeError, no gc_obj_list leak
