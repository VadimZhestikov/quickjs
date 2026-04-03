# Phase 8.5 — Dense Array Element Fast Path

Commit `09ef837`.

---

## Motivation

The old `OP_get_array_el` / `OP_put_array_el` codegen called:

```c
_RT->get_array_el(ctx, _o, _idx)
_RT->set_array_el(ctx, _o, _idx, _v)
```

These vtable entries (`jit_rt_get_array_el` / `jit_rt_set_array_el`) called
`JS_ValueToAtom(ctx, idx)` — converting the integer index to a `JSAtom` by
interning it in the atom table — and then `JS_GetProperty` / `JS_SetProperty`,
which walks the shape hash chain.

The interpreter's `GET_ARRAY_EL_INLINE` macro takes a completely different path
for dense arrays (`JS_CLASS_ARRAY` with integer index in bounds):

```c
val = JS_DupValue(ctx, p->u.array.u.values[idx]);   /* direct slot read */
```

No atom conversion, no hash chain — one pointer dereference.  P8.5 brings the
JIT up to the same fast path.

---

## Implementation

### Helper functions (quickjs.c)

Two new functions added after the IC helpers:

```c
/* Fast array read: returns 1 (hit, *out set) or 0 (miss, *out unchanged).
 * Handles only JS_CLASS_ARRAY with integer index in [0, count). */
int js_jit_array_get(JSContext *ctx, JSValue obj, uint32_t idx, JSValue *out)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    if (p->class_id != JS_CLASS_ARRAY)  return 0;
    if (idx >= (uint32_t)p->u.array.count) return 0;
    *out = JS_DupValue(ctx, p->u.array.u.values[idx]);
    return 1;
}

/* Fast array write: returns 1 (hit, val consumed) or 0 (miss, val untouched).
 * Only overwrites existing slots; array extension falls to slow path. */
int js_jit_array_set(JSContext *ctx, JSValue obj, uint32_t idx, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    if (p->class_id != JS_CLASS_ARRAY)  return 0;
    if (idx >= (uint32_t)p->u.array.count) return 0;
    set_value(ctx, &p->u.array.u.values[idx], val);   /* frees old, stores new */
    return 1;
}
```

Declared in `quickjs-jit.h` so generated JIT code can call them directly.

### Generated code — get

```c
{ JSValue _idx = _s[--_sp], _o = _s[--_sp];
  JSValue _r;
  if (js_likely(JS_VALUE_GET_TAG(_o) == JS_TAG_OBJECT
             && JS_VALUE_GET_TAG(_idx) == JS_TAG_INT)
      && js_jit_array_get(ctx, _o, (uint32_t)JS_VALUE_GET_INT(_idx), &_r))
      ; /* fast hit */
  else
      _r = _RT->get_array_el(ctx, _o, _idx);
  _FREE(_o); _FREE(_idx); _CHK(_r); _s[_sp++] = _r; }
```

### Generated code — put

```c
{ JSValue _v = _s[--_sp], _idx = _s[--_sp], _o = _s[--_sp];
  int _ret;
  if (js_likely(JS_VALUE_GET_TAG(_o) == JS_TAG_OBJECT
             && JS_VALUE_GET_TAG(_idx) == JS_TAG_INT)
      && js_jit_array_set(ctx, _o, (uint32_t)JS_VALUE_GET_INT(_idx), _v))
      _ret = 0;   /* fast hit — _v consumed inside js_jit_array_set */
  else
      _ret = _RT->set_array_el(ctx, _o, _idx, _v);   /* slow path — also consumes _v */
  _FREE(_o); _FREE(_idx); if (_ret < 0) goto _ex; }
```

### Ownership

The `js_jit_array_set` contract on `val`:
- **Hit (returns 1)**: `val` is stored in `p->u.array.u.values[idx]` via `set_value`,
  which frees the old value and takes ownership of `val`.
- **Miss (returns 0)**: `val` is untouched.  The generated code passes it to the slow path
  (`_RT->set_array_el`) which consumes it via `JS_SetPropertyValue`.

For integer indices (the fast path), `_idx` is `JS_TAG_INT` — an immediate —
so `_FREE(_idx)` is a no-op.

---

## Slow path coverage

Cases that fall through to `_RT->get/set_array_el`:

| Condition | Reason |
|---|---|
| `obj` tag ≠ `JS_TAG_OBJECT` | String/number property access — `JS_GetPropertyValue` handles correctly |
| `idx` tag ≠ `JS_TAG_INT` | String index like `a["key"]` — needs atom interning |
| `p->class_id ≠ JS_CLASS_ARRAY` | Arguments object, typed array, plain object |
| `idx >= p->u.array.count` | Out-of-bounds read (returns `undefined`) |
| Out-of-bounds write (`idx == count`) | Array extension — needs length update, checks `extensible` etc. |

Array extension (appending at `idx == count`) intentionally falls to the slow
path — the interpreter's fast extension path requires checking `p->fast_array`,
`p->extensible`, the length property writability, and the backing size.  This
complexity is not yet inlined; the slow path handles it correctly.

---

## Performance

Measurements: Linux 6.6.87.2 WSL2 x86-64, GCC -O2, `--jit-aot`, min of 3 runs.

```
Benchmark           Interp    JIT P8.3   JIT P8.5   Speedup   Notes
───────────────────────────────────────────────────────────────────────
arr_sum(10k)×1000   213 ms    210 ms     188 ms      1.1×     int array, warm JIT
```

The gain is modest because the JIT function itself has overhead (JSValue stack
operations, `_FREE` calls for int immediates that are no-ops but still code).
The key benefit is eliminating `JS_ValueToAtom` and the shape hash walk on every
element access — replacing them with two tag checks + one class_id check + one
bounds check + one pointer dereference.

For workloads where array access is the dominant cost (e.g., tight numerical
loops over large arrays), P8.5 brings the JIT closer to interpreter parity.
Full parity would require inlining the `JSObject` struct layout into the
generated code — blocked by `JSObject` being an opaque type in `quickjs.h`.

---

## Future work (P8.6 direction)

If `JSObject` internals are exposed to generated code (or if a richer
`quickjs-jit.h` exports the array offset constants), the fast path could be
fully inlined in GCC-compiled code:

```c
/* hypothetical inline version */
JSObject *_p = JS_VALUE_GET_OBJ(_o);
if (_p->class_id == JS_CLASS_ARRAY && _ui < (uint32_t)_p->u.array.count)
    _r = JS_DupValue(ctx, _p->u.array.u.values[_ui]);
else
    _r = _RT->get_array_el(ctx, _o, _idx);
```

GCC could then CSE `_p`, hoist bounds checks out of loops, and potentially
auto-vectorise element copies.  This would close the remaining gap vs the
interpreter's `GET_ARRAY_EL_INLINE`.
