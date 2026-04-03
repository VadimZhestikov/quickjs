# Phase 9 — Stackless IR, CF Structuring, Variable Names, Typed Temporaries

## Overview

Phase 9 moves the JIT code generator from "opcode translator" to "decompiler":
instead of emitting goto-spaghetti C with an explicit value stack, it reconstructs
the original structure of the JavaScript — named variables, real loops, real
conditionals — giving GCC the information it needs to apply its full optimisation
arsenal (LICM, vectorisation, loop unrolling, CSE across the full expression tree).

**Dependencies between sub-phases:**
```
P9.1 (variable names)  ─────────────────── independent, do first
P9.2 (stackless)       ─────────────────── independent of P9.3
  └──► P9.4 (typed temps)                  requires P9.2
P9.2 ──► P9.3 (CF structuring)             benefits from stackless locals being named
```

Suggested order: P9.1 → P9.2 → P9.3 → P9.4

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

- [ ] **P9.2-A** Add `stack_depth_tab` pre-pass (~60 lines):
  Before the main `while (pc < bc_len)` loop in `gen_body()`, allocate
  `int8_t *stack_depth_tab = calloc(bc_len, 1)` and do a linear simulation:
  - Track `int depth = 0`
  - At each label target (`scan_is_target`): reset to 0
  - For each opcode: apply its net stack effect (push count − pop count)
  - Store `stack_depth_tab[pc] = depth` before applying the effect
  This gives the stack depth BEFORE each opcode.

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
**Estimated effort:** ~5 days  **Risk:** medium-high  **Files:** `quickjs-jit.c`

### Background

`gen_body()` currently emits `_L{pc}:;` goto-labels and `goto _L{pc};` for all
branches.  GCC's vectoriser, LICM, and loop unroller require canonical `while`/`for`
form.  Since QuickJS only generates reducible CFGs from structured JavaScript (no
`goto` in JS source), dominator-based structuring always succeeds.

### Tasks

#### P9.3-A — Proper CFG (extend `JSJITScanResult`) (~150 lines)

- [ ] Add `JITBlock` struct:
  ```c
  typedef struct JITBlock {
      int start_pc, end_pc;    // [start, end) — exclusive
      int succ[2];             // successor block indices; -1 = none/exit
      int n_pred;
      int *preds;              // predecessor block indices (malloc'd)
      int idom;                // immediate dominator block index
      int is_loop_header;      // 1 if a back-edge targets this block
      int loop_exit;           // block index of loop exit (-1 if not header)
  } JITBlock;
  ```
- [ ] Extend `js_jit_scan()` to fill a `JITBlock` array alongside `sr->targets`:
  split at every branch opcode and every label target; record successor and
  predecessor edges.

#### P9.3-B — Dominator tree (~100 lines)

- [ ] Implement the iterative Cooper-Harvey-Kennedy algorithm:
  ```
  idom[entry] = entry
  repeat until stable:
    for b in reverse-post-order (skip entry):
      new_idom = first processed predecessor of b
      for each other predecessor p of b:
        if idom[p] is computed: new_idom = intersect(new_idom, p)
      idom[b] = new_idom
  ```
- [ ] Mark back-edges: edge (A→B) is a back-edge iff B dominates A.
- [ ] Set `is_loop_header` for targets of back-edges.
- [ ] Compute loop extents: natural loop of header H = all blocks that can reach
  the latch without leaving H's dominance subtree.

#### P9.3-C — If/else join point detection (~80 lines)

- [ ] For each block B with two successors (conditional branch):
  - Compute immediate post-dominator using the dominator tree of the reversed CFG,
    or simpler: find the lowest common dominator of the two successors.
  - Record as `join_block[B]`.
- [ ] Classify: if one successor == join_block → if-only (no else).
  If both differ from join → if-else.

#### P9.3-D — Recursive structured emitter (~400 lines)

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
- [ ] Fallback: any block/edge not matched by the above → emit the current goto-label
  output (never fails, always correct).
- [ ] Guard: if dominator computation or structuring fails for a function (shouldn't
  happen with well-formed QuickJS bytecode), fall back silently to the old linear
  emitter.

#### P9.3-E — Integration and testing

- [ ] Wire `emit_region` into `gen_body()`: compute CFG + dominators once, then
  call `emit_region(entry_block, exit_block)` instead of the `while (pc < bc_len)` loop.
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
| P9.1 Variable names | Human-readable generated C | 1 day | low | no change |
| P9.2 Stackless | No `_s[]`; named `_tsv{N}` | 3 days | medium | +30–60% |
| P9.3 CF structuring | `if`/`while`/`for` in output | 5 days | medium-high | +40–100% |
| P9.4 Typed temps | Zero tag-checks for float64 | 2 days | medium | +30–80% on float benchmarks |
| **Total** | | **~11 days** | | **~2–3× overall** |

## Testing checklist (run after each sub-phase)

- [ ] `make CONFIG_JIT=y test` — full test suite passes
- [ ] `make CONFIG_JIT=y JIT_THRESHOLD_GCC=2 qjs && (cd jit_perf_tests/v8bench && ../../qjs --jit-aot run_qjs.js)` — 3 runs, record best
- [ ] Manual spot-check: `./qjs --jit-dump script.js` — generated C looks correct
- [ ] ASAN build: `make CONFIG_JIT=y CONFIG_ASAN=y qjs && ./qjs --jit-aot tests/test_closure.js` — no memory errors
