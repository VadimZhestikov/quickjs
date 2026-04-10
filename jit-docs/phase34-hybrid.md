# Phase 34 — JIT + qjsc Hybrid Module Format

## Motivation

The existing JIT pipeline compiles *hot* functions at runtime using background GCC,
caching results in `~/.cache/qjs-jit/`.  This is ideal for long-running processes but
has three limitations:

1. **Startup latency** — GCC compilation takes 2–5 s per function; the first run of a
   fresh script is entirely interpreted.
2. **Cache dependency** — the cache lives on the host filesystem; deployment packages must
   either bundle it or re-warm on the target.
3. **Source distribution** — using `qjsc` to produce bytecode-only modules requires
   shipping the interpreter-only bytecode blob, with no JIT benefit.

Phase 34 introduces a **hybrid module format** (`--jit-hybrid`) that solves all three by
combining bytecode and JIT-compiled C into a single `.so` file that can be imported
directly via `import './mod.so'` — with no `.js` source file required at runtime.

---

## Concept

```
mod.js  (source)
    │
    ▼  qjsc --jit-hybrid -o mod.c mod.js
    │
mod.c   (generated C: bytecode blob + JIT function bodies + dispatch table)
    │
    ▼  gcc -O2 -shared -fPIC -DCONFIG_JIT -I<quickjs> -o mod.so mod.c
    │
mod.so  (self-contained: bytecode + native code + js_init_module entry point)
    │
    ▼  import { add, mul } from './mod.so'    ← no mod.js needed
    │
    └─ functions run at tier 2 (JIT) immediately, no warm-up required
```

---

## Generated C file structure

`qjsc --jit-hybrid` produces a single `.c` file with five sections:

### 1. Bytecode blob

```c
static const uint8_t _bc[] = { 0x01, 0x07, … };  /* serialised module bytecode */
static const uint32_t _bc_size = 246;
```

The bytecode is serialised with `JS_WriteObject` using the same strip settings as
normal `qjsc` output (source stripped by default; `--keep-source` preserves it).

### 2. JIT function bodies (`#ifdef CONFIG_JIT`)

One `JSValue __jit_f_<hash>(…)` function per eligible inner function:

```c
#ifdef CONFIG_JIT
JSValue __jit_f_39aff7310a2a1d1e(JSContext *ctx, JSValue this_val,
                                  int argc, JSValue *argv,
                                  JSValue *cpool, JSVarRef **var_refs)
{
    static JSJITICEntry _ic0 = { 0, 0, 0 };
    …
}
#endif /* CONFIG_JIT */
```

Functions that contain `OP_eval` or other unsupported opcodes are silently omitted
(interpreter handles them transparently).  The module body (`<eval>`) is always
excluded because it uses module-specific opcodes (`OP_define_export`, etc.).

### 3. Dispatch table

```c
#ifdef CONFIG_JIT
static const struct { uint64_t hash; JSJITFunc func; } _jit_table[] = {
    { 0x39aff7310a2a1d1eULL, __jit_f_39aff7310a2a1d1e },
    { 0xabcdef0123456789ULL, __jit_f_abcdef0123456789 },
};
#define _JIT_TABLE_COUNT 2
#endif
```

### 4. Install callback

```c
#ifdef CONFIG_JIT
static void _install_cb(JSFunctionBytecode *b, void *opaque)
{
    uint64_t h = js_jit_hash_bytecode_pub(b);
    for (int i = 0; i < _JIT_TABLE_COUNT; i++)
        if (_jit_table[i].hash == h) {
            js_jit_fb_set_func(b, _jit_table[i].func, NULL, 2);
            return;
        }
}
#endif
```

### 5. `js_init_module` entry point

```c
JSModuleDef *js_init_module(JSContext *ctx, const char *name)
{
    JSValue obj = JS_ReadObject(ctx, _bc, _bc_size, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(obj)) return NULL;
#ifdef CONFIG_JIT
    {
        JSFunctionBytecode *b = js_jit_module_get_bc(obj);
        if (b) js_jit_walk_bytecodes(b, _install_cb, NULL);
    }
#endif
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(obj);
    JS_FreeValue(ctx, obj);
    return m;
}
```

When `CONFIG_JIT` is defined, `_install_cb` walks all bytecodes in the module and
sets `tier = 2` on each function found in `_jit_table`.  When `CONFIG_JIT` is not
defined, the `.so` compiles cleanly and runs using the interpreter only — pure
bytecode fallback with no code changes required.

---

## Usage

### Step 1 — Generate hybrid C

```sh
./qjsc --jit-hybrid -o mod.c mod.js
```

Strip flags work exactly as in normal `qjsc` usage:

| Flag | `toString()` result | Source in bytecode |
|---|---|---|
| (default) | `function f() { [native code] }` | stripped (JS_STRIP_SOURCE) |
| `--keep-source` | `function f(a,b) { return a+b; }` | preserved |
| `-s` | `function f() { [native code] }` | fully stripped (JS_STRIP_DEBUG) |

### Step 2 — Compile to `.so`

```sh
# With JIT support (CONFIG_JIT):
gcc -O2 -shared -fPIC -DCONFIG_JIT -I/path/to/quickjs -o mod.so mod.c

# Without JIT support (pure-bytecode fallback):
gcc -O2 -shared -fPIC                -I/path/to/quickjs -o mod.so mod.c
```

### Step 3 — Import at runtime

```js
// runner.js
import { add, mul, fact } from './mod.so';
console.log(add(2, 3));   // → 5, executed at JIT tier 2
console.log(mul(4, 5));   // → 20, executed at JIT tier 2
```

```sh
./qjs -m runner.js        # no mod.js needed
```

---

## Behaviour

### JIT tier after load

After `js_init_module` returns:

| Function type | `js_jit_fb_get_tier()` | Execution path |
|---|---|---|
| Supported (no eval, no module opcodes) | 2 | JIT-compiled native code |
| Unsupported (`OP_eval`, unknown opcode) | 0 | QuickJS interpreter |
| Module body (`<eval>`) | 0 | QuickJS interpreter |

### `Function.prototype.toString()`

When the bytecode is serialised without source (default), exported functions return:

```js
import { add } from './mod.so';
add.toString();  // → "function add() {\n    [native code]\n}"
```

With `--keep-source`:

```js
add.toString();  // → "function add(a, b) { return a + b; }"
```

### Unsupported functions

Functions that cannot be JIT-compiled (`OP_eval`, etc.) are silently excluded from the
dispatch table.  The module still loads and the excluded functions execute via the
interpreter, producing correct results.

```js
export function add(a, b)  { return a + b; }   // → JIT (tier 2)
export function evalf(s)   { return eval(s); }  // → interpreter (tier 0)
```

---

## Public API (`quickjs-jit.h`)

Phase 34 adds the following functions to the public JIT embedding API:

```c
/* P34.1 — stable bytecode hash (same value used for cache filenames) */
uint64_t js_jit_hash_bytecode_pub(JSFunctionBytecode *b);

/* P34.2 — walk all bytecodes reachable from b (inner functions, deduplicated) */
void js_jit_walk_bytecodes(JSFunctionBytecode *b,
                            void (*cb)(JSFunctionBytecode *, void *),
                            void *opaque);

/* P34.3 — generate JIT C source for a single bytecode; caller owns the string */
char *js_jit_gen_c_str(JSContext *ctx, JSFunctionBytecode *b,
                        uint64_t bc_hash,
                        char *fname_out, size_t fname_sz,
                        int *unsupported);

/* P34.4 — get the module body bytecode from a JS_TAG_MODULE JSValue */
JSFunctionBytecode *js_jit_module_get_bc(JSValue module_val);

/* P34.6 — call a JIT-compiled function directly by bytecode pointer
 *          (var_refs=NULL; suitable for non-closure functions) */
JSValue js_jit_call_fb(JSContext *ctx, JSFunctionBytecode *b,
                        JSValue this_val, int argc, JSValue *argv);
```

---

## Test suite (`jit_tests/P34/`)

| Test | What it verifies |
|---|---|
| `test_p34_1.c` | `js_jit_hash_bytecode_pub()` matches `~/.cache/qjs-jit/<hash>.so` filename |
| `test_p34_2.c` | `js_jit_walk_bytecodes()` visits each bytecode exactly once (siblings + deep nesting) |
| `test_p34_3.c` | `js_jit_gen_c_str()` produces valid, compilable C with hash in symbol name |
| `test_p34_4.sh` | End-to-end `qjsc --jit-hybrid`: generates C, compiles, loads, calls, no source needed |
| `test_p34_5.c` | After `dlopen` + `js_init_module`: inner functions at tier 2, module body at tier 0 |
| `test_p34_6.c` | `js_jit_call_fb()` invokes JIT native code directly, returns correct values |
| `test_p34_7.c` | Eval-using function excluded from dispatch table (tier 0); interpreter fallback works |
| `test_p34_8.c` | `Function.prototype.toString()`: `[native code]` by default, source preserved with `--keep-source` |

Run all tests from `quickjs/`:

```sh
make CONFIG_JIT=y -C jit_tests/P34 run
```

Build requirements: `CONFIG_JIT=y` build already done (`make CONFIG_JIT=y`).
Tests P34.5–P34.7 need `-rdynamic` (handled by the Makefile).
