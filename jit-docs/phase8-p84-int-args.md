# Phase 8.4 — Integer Argument Fast-Path

Commit `b01183c`.

---

## Motivation

After P8.1 (integer locals) and P8.2 (direct self-recursive calls), `fib(n)`
still read `n` via `_DUP(argv[0])` on every `OP_get_arg0`.  For integer values
`_DUP` is a no-op (no refcount change), but every call still required a memory
load through the `argv` pointer — which GCC could not CSE across the
self-recursive call because `argv` is a pointer parameter that the callee could
theoretically alias.

With P8.4, `n` is extracted into a local `int32_t _ai[0]` at function entry.
GCC sees it is not aliased by any pointer and keeps it in a CPU register across
all three reads within the function body, eliminating three per-call memory
loads in the hot path.

---

## Design

### Variables emitted in the preamble

```c
int32_t _ai[N];      /* N = min(arg_count, 32) */
uint32_t _aim = 0;   /* bitmask: bit i set iff argv[i] is JS_TAG_INT */
```

### Extraction at function entry

For each argument `i` from 0 to `N-1`:

```c
if (i < argc && JS_VALUE_GET_TAG(argv[i]) == JS_TAG_INT) {
    _ai[i] = JS_VALUE_GET_INT(argv[i]);
    _aim |= (1u << i);
}
```

If an argument is not `JS_TAG_INT` (object, string, float), `_aim` bit `i`
remains clear and the argument falls back to the `argv[i]` path on every read.
No deopt, no second function body — the same code handles both cases.

### GEN_GET_ARG (read)

```c
_s[_sp++] = (i < argc && (_aim >> i & 1))
            ? JS_MKVAL(JS_TAG_INT, _ai[i])   /* register copy, no memory load */
            : (i < argc ? _DUP(argv[i]) : JS_UNDEFINED);
```

### GEN_PUT_ARG / GEN_SET_ARG (write)

If the argument is overwritten, both `_ai[i]` and `argv[i]` are kept consistent:

```c
/* put_arg: pop and store */
if (i < argc) {
    JSValue _t = _s[--_sp];
    if (JS_VALUE_GET_TAG(_t) == JS_TAG_INT) {
        _ai[i] = JS_VALUE_GET_INT(_t);
        _aim |= (1u << i);
    } else {
        _aim &= ~(1u << i);   /* arg became non-int; future reads use argv[i] */
    }
    _FREE(argv[i]); argv[i] = _t;
} else _FREE(_s[--_sp]);
```

### Limit

Only arguments 0..31 are covered by the `uint32_t` bitmask.  Arguments ≥ 32
fall back to the original `_DUP(argv[i])` path unchanged.  Functions with more
than 32 arguments are rare in practice.

---

## Interaction with P8.2 (self-recursive calls)

When `fib` calls itself via the P8.2 direct call:

```c
JSValue _r = __jit_f_<hash>(ctx, JS_UNDEFINED, _n, &_s[_sp-_n], cpool, var_refs);
```

The child receives `argv = &_s[...]` which points into the parent's evaluation
stack.  The values there are `JS_MKVAL(JS_TAG_INT, n-1)` etc. — always
`JS_TAG_INT`.  So in the recursive frame, `_aim |= 1` is set at entry and
`_ai[0]` is extracted from the integer immediately, avoiding a memory load from
the parent's stack on subsequent reads.

---

## Performance

Measurements: Linux 6.6.87.2 WSL2 x86-64, GCC -O2, `--jit-aot` warm cache,
min of 3 runs.  Baseline = `qjs_interp` (JIT disabled binary).

```
Benchmark      Interp    JIT P8.3    JIT P8.4   Speedup vs interp   Delta
────────────────────────────────────────────────────────────────────────────
fib(38)        4401 ms    1800 ms    1343 ms      3.3×               +0.6×
sum_loop×20     465 ms     165 ms     166 ms      2.8×               same
arr_sum×1000    213 ms     210 ms     188 ms      1.1×               slight
```

`fib` gain is entirely from P8.4: the argument `n` is read three times per
recursive call; keeping it in a register rather than reloading from the stack
saves ~25% of the total call cost at fib(38) depth.

`sum_loop` is unaffected — it has no arguments in the hot path (the loop
variable `i` is a P8.1 int local).

---

## Edge cases

| Scenario | Behaviour |
|---|---|
| `fib(1.5)` (float arg) | `_aim` bit 0 clear; every `get_arg0` does `_DUP(argv[0])` — correct |
| `fib(null)` | Same; `JS_VALUE_GET_TAG(null) != JS_TAG_INT` → fallback |
| `arg_count == 0` | No `_ai[]` or `_aim` emitted at all |
| `put_arg` rewrites arg to non-int | Bit cleared; subsequent reads fall back to `argv[i]` |
| `argc < N` (fewer args than declared) | Guard `i < argc` prevents out-of-bounds |
