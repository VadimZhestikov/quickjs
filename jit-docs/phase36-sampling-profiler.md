# Phase 36 — Sampling-Based Time Profiler (`--jit-profile-time`)

## Motivation

P35.5 added a call-count profile (`--jit-profile`).  Call counts work well for
determining *which* functions get JIT-compiled but are a poor proxy for actual
hotness once the JIT threshold is hit: `jit_call_count` stops incrementing after
the function reaches tier 2, so every tier-2 function saturates at ~100 calls
regardless of how many times it runs in the JIT path.  The result is that PGO
(`--jit-pgo`) sees nearly identical counts for all compiled functions and cannot
distinguish a 500 ms hot path from a 1 µs initialization stub.

A **sampling profiler** fixes this: a SIGPROF signal fires at a fixed rate
(default 100 Hz), the handler reads `%rip` from the saved machine context,
identifies the enclosing JIT function via a sorted address registry, and
increments its sample counter.  After execution, `time_ms ≈ samples × 10` (at
100 Hz).  This is true CPU time accumulated in the JIT-compiled code.

---

## Architecture

```
SIGPROF handler (async, signal context)
  │
  ├─► read %rip from ucontext_t
  ├─► binary-search jit_addr_registry[] (seqlock-protected)
  ├─► if found: __atomic_fetch_add(&registry[i].samples, 1, RELAXED)
  └─► return (handler must be async-signal-safe)

Main thread
  │
  ├─► js_jit_install_results()  (also cache-hit paths in js_jit_queue_gcc)
  │     └─► jit_registry_add(func_ptr, bc_hash, name)   ← seqlock write
  │
  ├─► js_jit_free_bytecode()
  │     └─► jit_registry_remove(func_ptr)                ← seqlock write
  │
  └─► js_jit_write_profile_timed(ctx, path, hz)
        ├─► Pass 1: js_jit_walk_all_modules → per-module-bytecode:
        │     hash → jit_registry_lookup_samples(hash) → time_ms
        │     emit JSON with "calls" + "time_ms" fields
        │     record hash in seen[] for dedup
        └─► Pass 2: iterate jit_addr_registry[] directly
              for entries with samples > 0 not in seen[]
              (global-script functions not in any loaded module)
              emit JSON with "calls":0 + "time_ms" + "name" from registry
```

**Signal safety**: the registry uses a **seqlock** (write increments an odd
counter, completes, increments to even; reader retries if seq is odd or changes).
Seqlocks are the standard pattern for signal-handler-readable shared data.
Sample counters are `uint32_t` incremented with `__ATOMIC_RELAXED` — no lock
needed since they are per-slot and never freed while the entry exists.

**No `dladdr()` in the signal handler**: symbol resolution is done offline
(after execution) via the registry, not in the hot signal path.

---

## Data Structures

### `jit_addr_registry` (in `quickjs-jit.c`)

```c
/* One entry per installed JIT function.
 * Sorted by func_ptr ascending for binary search in the signal handler. */
typedef struct {
    uintptr_t  func_ptr;   /* start address of JIT function              */
    uint64_t   bc_hash;    /* FNV-1a hash — key for profile output        */
    uint32_t   samples;    /* atomic sample counter (SIGPROF increments)  */
    char       name[80];   /* JS function name for profile output         */
} JITAddrEntry;

#define JIT_ADDR_REGISTRY_MAX 4096

static JITAddrEntry  jit_addr_registry[JIT_ADDR_REGISTRY_MAX];
static int           jit_addr_count = 0;   /* number of live entries      */
static volatile uint32_t jit_addr_seqlock = 0; /* seqlock (even=stable)  */
```

The fixed-size array avoids `malloc` in the signal path.  4096 slots is enough
for any realistic workload; if exceeded, `jit_registry_add` logs a warning and
skips the entry (function is simply not sampled).

### Seqlock helpers (file-static in `quickjs-jit.c`)

```c
static inline void seqlock_write_begin(void) {
    __atomic_add_fetch(&jit_addr_seqlock, 1, __ATOMIC_SEQ_CST); /* odd */
}
static inline void seqlock_write_end(void) {
    __atomic_add_fetch(&jit_addr_seqlock, 1, __ATOMIC_SEQ_CST); /* even */
}
/* Returns start seq; call seqlock_retry() after reading. */
static inline uint32_t seqlock_read_begin(void) {
    uint32_t s;
    do { s = __atomic_load_n(&jit_addr_seqlock, __ATOMIC_SEQ_CST);
    } while (s & 1); /* spin while writer holds lock */
    return s;
}
static inline int seqlock_retry(uint32_t s) {
    return s != __atomic_load_n(&jit_addr_seqlock, __ATOMIC_SEQ_CST);
}
```

### SIGPROF state (file-static in `quickjs-jit.c`)

```c
static int                  jit_sampler_hz     = 0;   /* 0 = stopped     */
static struct sigaction     jit_old_sigaction;         /* saved handler   */
static struct itimerval     jit_old_itimer;            /* saved timer     */
```

---

## Step-by-Step Plan

### P36.1 — JIT address range registry ✓ DONE
**Files:** `quickjs-jit.c`, `quickjs-jit.h`  
**Effort:** ~0.5 day  
**Risk:** low

Add the `JITAddrEntry` array and seqlock to `quickjs-jit.c`.

#### `jit_registry_add(func_ptr, bc_hash, name)` — called from install paths

```c
void jit_registry_add(uintptr_t func_ptr, uint64_t bc_hash, const char *name)
{
    int cnt = jit_addr_count;
    if (cnt >= JIT_ADDR_REGISTRY_MAX) return; /* full — not sampled */

    jit_seqlock_write_begin();

    /* Insertion sort to keep array sorted by func_ptr */
    int i = cnt;
    while (i > 0 && jit_addr_registry[i-1].func_ptr > func_ptr) {
        jit_addr_registry[i] = jit_addr_registry[i-1];
        i--;
    }
    jit_addr_registry[i].func_ptr = func_ptr;
    jit_addr_registry[i].bc_hash  = bc_hash;
    jit_addr_registry[i].samples  = 0;
    strncpy(jit_addr_registry[i].name, name ? name : "",
            sizeof(jit_addr_registry[i].name) - 1);
    jit_addr_count = cnt + 1;

    jit_seqlock_write_end();
}
```

Called unconditionally (not gated on sampler active) from four install paths:
- `js_jit_install_results()` — GCC worker result → main thread install
- `js_jit_queue_gcc()` combined.so path — function already in manifest
- `js_jit_queue_gcc()` individual cache-hit path — function loaded from `.so`
- `jit_install_combined_pass()` — post-load manifest scan

JS function name is computed early in `js_jit_queue_gcc` (before any early-return
paths) via `js_jit_fb_get_func_name(JS_GetRuntime(ctx), b)`, then propagated
through `JITGCCJob.js_name` → `JITGCCResult.js_name` → registry entry.

#### `jit_registry_remove(func_ptr)` — called from `js_jit_free_bytecode()`

```c
static void jit_registry_remove(uintptr_t func_ptr)
{
    seqlock_write_begin();
    for (int i = 0; i < jit_addr_count; i++) {
        if (jit_addr_registry[i].func_ptr == func_ptr) {
            /* Shift remaining entries left */
            for (int j = i; j < jit_addr_count - 1; j++)
                jit_addr_registry[j] = jit_addr_registry[j+1];
            jit_addr_count--;
            break;
        }
    }
    seqlock_write_end();
}
```

Hook site in `js_jit_free_bytecode()` — before `dlclose(handle)`:
```c
    /* P36.1: remove from sampling registry before releasing .so */
    if (tier == 2 && b->jit_func)
        jit_registry_remove((uintptr_t)b->jit_func);
```

#### `jit_registry_lookup_samples(bc_hash)` — called from profile writer

```c
static uint32_t jit_registry_lookup_samples(uint64_t bc_hash)
{
    /* Not called from signal context; no seqlock needed here.
     * Called only after sampler is stopped, so registry is stable. */
    for (int i = 0; i < jit_addr_count; i++)
        if (jit_addr_registry[i].bc_hash == bc_hash)
            return jit_addr_registry[i].samples;
    return 0;
}
```

#### Tests (P36.1)

`jit-tests/P36/test_p36_1.c` — C harness:
- A: `jit_registry_add` inserts entries sorted by func_ptr; count grows correctly.
- B: `jit_registry_remove` removes by func_ptr; remaining entries stay sorted.
- C: `jit_registry_lookup_samples` returns 0 for unknown hash; returns correct count after manual increment.
- D: Registry at capacity (fill 4096 entries, verify 4097th is dropped gracefully).

---

### P36.2 — SIGPROF sampler (signal handler + timer) ✓ DONE
**Files:** `quickjs-jit.c`, `quickjs-jit.h`  
**Effort:** ~1 day  
**Risk:** medium (platform-specific ucontext, async-signal-safety)

#### Signal handler

```c
#include <signal.h>
#include <ucontext.h>
#include <sys/time.h>

static void jit_sigprof_handler(int sig, siginfo_t *si, void *ctx_raw)
{
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)ctx_raw;

#if defined(__x86_64__) && defined(__linux__)
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__x86_64__) && defined(__APPLE__)
    uintptr_t rip = (uintptr_t)uc->uc_mcontext->__ss.__rip;
#else
    return; /* unsupported platform — no-op */
#endif

    /* Binary search for largest func_ptr <= rip */
    uint32_t seq;
    int found_i = -1;
    do {
        seq = seqlock_read_begin();
        int lo = 0, hi = jit_addr_count - 1, idx = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (jit_addr_registry[mid].func_ptr <= rip) {
                idx = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        found_i = idx;
    } while (seqlock_retry(seq));

    if (found_i >= 0) {
        /* Increment without lock — atomic relaxed is sufficient:
         * we don't care about ordering, only about not tearing the counter. */
        __atomic_fetch_add(&jit_addr_registry[found_i].samples, 1,
                           __ATOMIC_RELAXED);
    }
}
```

**False positives**: we identify "the JIT function whose start is closest to (but ≤) rip".  This can misattribute samples to the preceding function if `rip` is actually in non-JIT code (interpreter, libc, etc.) that happens to be loaded after a JIT .so.  For an approximate profiler this is acceptable; false positives are rare (JIT .so regions are small and sparsely mapped).  A future refinement (P36 extension) can validate using `dl_iterate_phdr` to confirm `rip` is within a known .so's text segment.

#### Sampler start/stop

```c
void js_jit_sampler_start(int hz)
{
    if (jit_sampler_hz > 0) return; /* already running */
    jit_sampler_hz = hz;

    struct sigaction sa;
    sa.sa_sigaction = jit_sigprof_handler;
    sa.sa_flags     = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, &jit_old_sigaction);

    struct itimerval it;
    it.it_interval.tv_sec  = 0;
    it.it_interval.tv_usec = 1000000 / hz;
    it.it_value             = it.it_interval;
    setitimer(ITIMER_PROF, &it, &jit_old_itimer);
}

void js_jit_sampler_stop(void)
{
    if (jit_sampler_hz == 0) return;
    setitimer(ITIMER_PROF, &jit_old_itimer, NULL);
    sigaction(SIGPROF, &jit_old_sigaction, NULL);
    jit_sampler_hz = 0;
}
```

#### Public API declarations in `quickjs-jit.h`

```c
/*
 * js_jit_sampler_start() — P36.2
 * Install SIGPROF handler at <hz> samples/second and start ITIMER_PROF.
 * Increments jit_addr_registry[].samples for each JIT function hit.
 * Must be called AFTER js_jit_install_results() has run at least once
 * (or jit_registry_add hooks will register future installs automatically).
 * hz: samples per second; 100 is a good default.
 * No-op if already running.  Not thread-safe; call from main thread only.
 */
void js_jit_sampler_start(int hz);

/*
 * js_jit_sampler_stop() — P36.2
 * Stop the SIGPROF timer and restore the previous signal handler.
 * After this returns, no further samples will be recorded.
 * Safe to call if sampler was never started (no-op).
 */
void js_jit_sampler_stop(void);
```

#### Tests (P36.2)

`jit-tests/P36/test_p36_2.c` — C harness:
- A: `js_jit_sampler_start(1000)` + `js_jit_sampler_stop()` does not crash; old signal handler is restored.
- B: Run a tight JS loop (fib(30)) with sampler active; at least one entry in the registry gains sample_count > 0.
- C: Sampler stopped before JS_FreeContext; no SIGPROF fires after stop.
- D: Double-start is a no-op (no crash, same hz preserved).

---

### P36.3 — Extended profile writer (`js_jit_write_profile` + `time_ms`) ✓ DONE
**Files:** `quickjs-jit.c`, `quickjs-jit.h`  
**Effort:** ~0.5 day  
**Risk:** low

Extend the existing `js_jit_write_profile` to include `time_ms` from the sample
registry when the sampler was active.  The format remains backwards-compatible:
`"time_ms"` is an optional field; `qjsc --jit-pgo` already reads
JSON loosely and will ignore unknown fields.

#### Modified `ProfileWalkState` and `profile_walk_cb`

```c
typedef struct {
    FILE       *f;
    int         first;
    JSRuntime  *rt;
    int         hz;        /* P36.3: sampler hz; 0 = no timing data */
    /* P36.4: track hashes output by module walk for registry dedup pass */
    uint64_t   *seen;
    int         seen_count;
    int         seen_cap;
} ProfileWalkState;

static void profile_walk_cb(JSFunctionBytecode *b, void *opaque)
{
    ProfileWalkState *st = (ProfileWalkState *)opaque;
    int calls = js_jit_fb_get_call_count(b);
    uint64_t hash = js_jit_hash_bytecode_pub(b);

    uint32_t samples  = 0;
    uint32_t time_ms  = 0;
    if (st->hz > 0) {
        samples = jit_registry_lookup_samples(hash);
        time_ms = (st->hz > 0) ? (samples * 1000u) / (uint32_t)st->hz : 0;
    }

    if (calls <= 0 && samples == 0)
        return; /* nothing recorded for this function */

    /* ... name escaping as before ... */

    if (!st->first) fprintf(st->f, ",\n");

    if (st->hz > 0) {
        fprintf(st->f,
            "  {\"hash\":\"%016llx\",\"calls\":%d,"
            "\"time_ms\":%u,\"name\":\"%s\"}",
            (unsigned long long)hash, calls, time_ms, safe_name);
    } else {
        fprintf(st->f,
            "  {\"hash\":\"%016llx\",\"calls\":%d,\"name\":\"%s\"}",
            (unsigned long long)hash, calls, safe_name);
    }
    st->first = 0;
}
```

`js_jit_write_profile` signature stays the same (passes `hz=0` internally):

```c
int js_jit_write_profile(JSContext *ctx, const char *path)
{
    /* existing implementation — passes hz=0 → no time_ms field */
    ProfileWalkState st = { f, 1, JS_GetRuntime(ctx), 0 };
    ...
}
```

New function for time-aware write:

```c
/*
 * js_jit_write_profile_timed() — P36.3
 * Like js_jit_write_profile() but also emits "time_ms" computed from
 * the sampling registry.  Must be called after js_jit_sampler_stop().
 * hz: the rate used in js_jit_sampler_start(hz) — used to convert
 *     sample counts to milliseconds.
 * Returns 0 on success, -1 if fopen fails.
 */
int js_jit_write_profile_timed(JSContext *ctx, const char *path, int hz);
```

Implementation creates `ProfileWalkState st = { f, 1, rt, hz }` and otherwise
follows the same pattern as `js_jit_write_profile`.

#### Tests (P36.3)

`jit-tests/P36/test_p36_3.c`:
- A: `js_jit_write_profile_timed` with `hz=0` produces same output as `js_jit_write_profile` (no `time_ms` field).
- B: After running a JIT-compiled module with sampler at 1000 Hz, output includes `"time_ms"` field with value > 0.
- C: Backward compatibility: old profile without `"time_ms"` is still valid input for `pgo_load()` in `qjsc.c`.

---

### P36.4 — `qjs --jit-profile-time=<file>[,Hz]` flag ✓ DONE
**Files:** `qjs.c`, `quickjs-jit.c`  
**Effort:** ~0.5 day  
**Risk:** low

#### New globals in `qjs.c`

```c
static const char *jit_profile_time_path = NULL; /* --jit-profile-time=<file> */
static int         jit_profile_time_hz   = 1000; /* default 1000 Hz           */
static char        jit_profile_time_path_buf[512];
```

#### Argument parsing

`--jit-profile-time=<file>[,Hz]` — splits on the *last* comma followed by digits.
Default Hz is 1000 (higher than P36.2's internal default because WSL2 profiling
benefits from more samples on short runs).

#### Sampler lifecycle in `eval_buf()` / `main()`

Sampler is started just before `eval_file()` is called (after all context init
and `--jit-aot` / `--jit-compile-all` option setup, but before JS execution).
Stopped after `js_std_loop()`.  Profile written after stop.

```c
/* just before eval_file() */
if (jit_profile_time_path)
    js_jit_sampler_start(jit_profile_time_hz);

/* ... eval, js_std_loop ... */

if (jit_profile_time_path)
    js_jit_sampler_stop();

if (jit_profile_time_path) {
    if (js_jit_write_profile_timed(ctx, jit_profile_time_path,
                                   jit_profile_time_hz) != 0)
        fprintf(stderr, "qjs: --jit-profile-time: failed to write '%s'\n",
                jit_profile_time_path);
}
```

#### Global-script support (registry second pass)

`js_jit_write_profile_timed` now performs two passes:

1. **Module walk** (`js_jit_walk_all_modules`): finds all module-scope bytecodes,
   outputs functions with `calls > 0` or `samples > 0`, records their hashes in
   `ProfileWalkState.seen[]`.

2. **Registry scan**: iterates `jit_addr_registry[]` directly for any entry with
   `samples > 0` whose hash is **not** in `seen[]`.  These are global-script
   functions (not in `ctx->loaded_modules`).  They carry the JS name stored in
   `JITAddrEntry.name` (populated at `jit_registry_add` time).

This means `--jit-profile-time` works correctly for both module scripts (`.mjs`)
and traditional global scripts (`.js`).

**Recommended workflow for global scripts:**
```sh
# First run: compile and cache all functions
./qjs --jit-compile-all script.js
# (or --jit-warmup for AOT combined.so)

# Second run: profile with warm cache
./qjs --jit-compile-all --jit-profile-time=prof.json,1000 script.js
```

For module files, `--jit-compile-all` or `--jit-aot` work equally well.

#### Tests (P36.4)

`jit-tests/P36/test_p36_4.sh` — shell (5 subtests):
- A: `--jit-profile-time` creates a JSON file with `"time_ms"` field.
- B: At least one `time_ms > 0` for a hot function.
- C: `--jit-profile-time=<file>,500` (Hz=500) parses correctly, profile written.
- D: **Global-script** function appears in profile via registry second pass.
- E: **Module** function appears in profile via module walk path.

---

### P36.5 — `qjsc --jit-pgo` time-aware extension
**Files:** `qjsc.c`  
**Effort:** ~0.5 day  
**Risk:** low

Extend `pgo_load()` and `pgo_opt_level` to consume `time_ms` from the profile.

#### Extended `PGOEntry`

```c
typedef struct {
    uint64_t hash;
    int      calls;
    int      time_ms;  /* P36.5: 0 if not present in profile */
} PGOEntry;
```

#### `pgo_load()` extended parser

Scan for both `"calls":` and `"time_ms":` in each JSON object.  `"time_ms"` is
optional; if absent, `time_ms=0`.

```c
/* Inside the parse loop: */
const char *tp = strstr(p, "\"time_ms\":");
if (tp && tp < next_hash) { /* only if within this JSON object */
    g_pgo_entries[g_pgo_count].time_ms = atoi(tp + 10);
}
```

`next_hash` is the position of the next `"hash":` token (used to bound the
search within one JSON object).

#### `pgo_hotness(entry)` — unified hotness metric

```c
/* Returns effective hotness for optimization level selection.
 * Prefer time_ms if available (more accurate); fall back to calls. */
static int pgo_hotness(const PGOEntry *e)
{
    if (e->time_ms > 0) {
        /* Map time_ms → synthetic "calls" equivalent for opt_level() reuse */
        if (e->time_ms >= 500) return 10000; /* → O3 */
        if (e->time_ms >=  50) return  1000; /* → O2 */
        if (e->time_ms >=   5) return   100; /* → O1 */
        return 1;                             /* → O0 */
    }
    return e->calls; /* fall back to call count */
}
```

Then in `pgo_lookup()` + walker callbacks, use `pgo_hotness(&g_pgo_entries[i])`
instead of `g_pgo_entries[i].calls` directly.

#### Tests (P36.5)

`jit-tests/P36/test_p36_5.sh` — shell, end-to-end PGO round-trip:
- A: Collect time profile with `--jit-profile-time`.
- B: `qjsc --jit-hybrid-app --jit-pgo=<time-profile>` generates C with `"O3"` pragma for hot function (time_ms ≥ 500).
- C: Profile with only `"calls"` (no `"time_ms"`) still works (backwards-compat).
- D: Profile with both fields uses `time_ms` as the primary metric.

---

### P36.6 — Makefile and test harness
**Files:** `jit-tests/P36/Makefile`, `jit-tests/P36/test_p36_*.{c,sh}`

```makefile
QJS_DIR := $(realpath ../..)
CFLAGS  := -g -O0 -DCONFIG_JIT -I$(QJS_DIR)
OBJS    := $(QJS_DIR)/.obj/quickjs.o \
            $(QJS_DIR)/.obj/quickjs-jit.o \
            $(QJS_DIR)/.obj/quickjs-libc.o \
            $(QJS_DIR)/.obj/dtoa.o \
            $(QJS_DIR)/.obj/libregexp.o \
            $(QJS_DIR)/.obj/libunicode.o \
            $(QJS_DIR)/.obj/cutils.o
LIBS    := -lm -lpthread -ldl

.PHONY: all run clean

all: /tmp/test_p36_1 /tmp/test_p36_2 /tmp/test_p36_3

/tmp/test_p36_1: test_p36_1.c
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

/tmp/test_p36_2: test_p36_2.c
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

/tmp/test_p36_3: test_p36_3.c
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

run: all
	@echo "=== P36.1: address range registry ==="
	@/tmp/test_p36_1
	@echo "=== P36.2: SIGPROF sampler ==="
	@/tmp/test_p36_2
	@echo "=== P36.3: extended profile writer ==="
	@/tmp/test_p36_3
	@echo "=== P36.4: qjs --jit-profile-time flag ==="
	@sh test_p36_4.sh $(QJS_DIR)/qjs
	@echo "=== P36.5: qjsc --jit-pgo time-aware ==="
	@sh test_p36_5.sh $(QJS_DIR)/qjs $(QJS_DIR)/qjsc
	@echo "=== ALL P36 TESTS COMPLETE ==="

clean:
	rm -f /tmp/test_p36_1 /tmp/test_p36_2 /tmp/test_p36_3
```

---

## Profile JSON Format (after P36)

```json
{
  "functions": [
    { "hash": "39aff7310a2a1d1e", "calls": 100, "time_ms": 480, "name": "fib" },
    { "hash": "abcd1234abcd1234", "calls": 1,   "time_ms": 0,   "name": "init" }
  ]
}
```

- `"calls"` and `"time_ms"` are both optional; `qjsc --jit-pgo` accepts either.
- A profile written by `--jit-profile` (P35.5-B) has `"calls"` only (no `"time_ms"`).
- A profile written by `--jit-profile-time` (P36.4) has both fields.
- `"time_ms": 0` means the function was not sampled (e.g., ran < 5ms total).

---

## PGO Threshold Tables (after P36.5)

### Call-count based (P35.5-C, unchanged)
| calls     | GCC pragma level |
|-----------|-----------------|
| ≥ 10 000  | O3              |
| ≥  1 000  | O2              |
| ≥    100  | O1              |
| ≥      1  | O0              |
| 0 / absent | skip           |

### Time-based (P36.5, preferred when `time_ms` present)
| time_ms   | GCC pragma level |
|-----------|-----------------|
| ≥ 500 ms  | O3              |
| ≥  50 ms  | O2              |
| ≥   5 ms  | O1              |
| ≥   0 ms  | O0              |
| 0 / absent | skip (if no calls either) |

---

## Platform Notes

| Platform | `ucontext_t` field | Notes |
|---|---|---|
| Linux x86-64 | `uc->uc_mcontext.gregs[REG_RIP]` | Requires `_GNU_SOURCE` |
| macOS x86-64 | `uc->uc_mcontext->__ss.__rip` | |
| Linux aarch64 | `uc->uc_mcontext.pc` | |
| Other | no-op | handler returns immediately |

`ITIMER_PROF` accounts for CPU time in user+kernel mode.  On WSL2 the timer fires
less predictably due to virtualization; sampling at 1000 Hz is recommended for
short runs (< 5s) to collect enough samples.

The seqlock assumes that the compiler does not reorder loads/stores across the
atomic operations.  `__atomic_*` with `__ATOMIC_SEQ_CST` guarantees this on
all supported compilers (GCC ≥ 4.7, Clang ≥ 3.1).

---

## Dependencies

| Dependency | Why |
|---|---|
| P35.5-A (`js_jit_write_profile`) | P36.3 extends it |
| P35.5-B (`--jit-profile`) | P36.4 follows the same `qjs.c` pattern |
| P35.5-C (`--jit-pgo` in qjsc) | P36.5 extends `pgo_load` + `PGOEntry` |
| `js_jit_install_results` hook point | P36.1 registry_add called from there |
| `js_jit_free_bytecode` hook point | P36.1 registry_remove called from there |

---

## Summary

| Sub-phase | What | Effort | Risk | Status |
|---|---|---|---|---|
| P36.1 | Address range registry (seqlock, add/remove/lookup) | 0.5 day | low | ✓ DONE |
| P36.2 | SIGPROF sampler (handler + timer + platform ucontext) | 1 day | medium | ✓ DONE |
| P36.3 | Extended profile writer (`time_ms` field) | 0.5 day | low | ✓ DONE |
| P36.4 | `qjs --jit-profile-time=<file>[,Hz]` flag | 0.5 day | low | ✓ DONE |
| P36.5 | `qjsc --jit-pgo` time-aware hotness metric | 0.5 day | low | pending |
| P36.6 | Makefile + test harness (5 test files) | 0.5 day | low | partial |

**Total: ~3.5 days**

**Files changed:**
- `quickjs-jit.c` — registry, seqlock, sampler, `js_jit_write_profile_timed`
- `quickjs-jit.h` — `js_jit_sampler_start`, `js_jit_sampler_stop`, `js_jit_write_profile_timed`
- `qjs.c` — `--jit-profile-time` flag, sampler lifecycle
- `qjsc.c` — `pgo_load` extension, `PGOEntry.time_ms`, `pgo_hotness()`
- `jit-tests/P36/` — new test directory (Makefile + 5 test files)
