# Phase 4 — GCC Tier-2 Background Compilation

**Commit:** `f690cc5`
**Goal:** Compile hot functions with GCC -O2 without blocking the JS thread.

---

## Overview

Phase 4 replaces TCC with a background worker that forks GCC as an external process.
The main thread generates C source synchronously (fast), then hands the job to a pthread
worker and returns immediately.  When GCC finishes, the compiled function is installed
atomically.  The interpreter continues running the function until the JIT version is
ready.

```
Main thread                          Worker pthread
───────────                          ──────────────
gen_body() → C source  (~100 µs)
enqueue(job)  ────────────────────► dequeue(job)
return                               write .c to /tmp/qjs_jit_XXXXXX.c
interpret...                         fork + exec gcc -O2 -shared -fPIC  (~2–5 s)
interpret...                         waitpid
interpret...                         dlopen(.so)
interpret...                         dlsym("__jit_f_<addr>")
interpret...                         atomic_store(RELEASE) → b->jit_func = f
next call: atomic_load(ACQUIRE) → f  ← JIT now active
jit_func(ctx, ...)
```

---

## Data structures

### JITGCCJob

```c
typedef struct JITGCCJob {
    JSFunctionBytecode *b;     /* target bytecode; used for atomic install */
    char               *c_src; /* malloc'd C source (freed after gcc exits) */
    char                fname[64]; /* dlsym symbol: "__jit_f_<hex_ptr>" */
    struct JITGCCJob   *next;  /* singly-linked FIFO queue */
} JITGCCJob;
```

### Worker state (module-level static)

```c
static struct {
    pthread_t      thread;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    JITGCCJob      *head, *tail;  /* FIFO queue */
    int             stop;         /* set by js_jit_free() */
    int             started;      /* thread created? */
    int             ref_count;    /* incremented per JSRuntime */
} jit_worker;
```

A single worker thread is shared across all `JSRuntime` instances in the process
(guarded by `ref_count`).

---

## Compilation pipeline

### 1. C source generation (main thread, synchronous)

Called from `js_jit_queue_gcc(ctx, b)`:

```c
/* 1. Run type inference */
uint8_t *local_type = jit_infer_types(bc, bc_len, var_count, stack_size);

/* 2. Scan for branch targets and unsupported opcodes */
JSJITScanResult sr;
if (scan_body(bc, bc_len, op_sz, &sr) < 0) {
    js_jit_fb_set_no_compile(b);
    free(local_type);
    return;
}

/* 3. Generate C into a JSJITCodeBuf */
JSJITCodeBuf cb = {0};
gen_preamble(&cb, b, rt, var_count, arg_count, stack_size);
gen_body(&cb, bc, bc_len, &sr, op_sz, op_sz_count,
         var_count, arg_count, stack_size,
         &unsupported, local_type);

/* 4. Enqueue */
JITGCCJob *job = malloc(sizeof *job);
job->b     = b;
job->c_src = cb.buf;
snprintf(job->fname, sizeof job->fname, "__jit_f_%p", (void *)bc);
enqueue_job(job);
```

`js_jit_fb_set_no_compile(b)` is set before enqueueing to prevent duplicate jobs if
the function is called again while GCC is still running.

### 2. Worker thread loop

```c
static void *jit_worker_thread(void *arg)
{
    for (;;) {
        pthread_mutex_lock(&jit_worker.lock);
        while (!jit_worker.head && !jit_worker.stop)
            pthread_cond_wait(&jit_worker.cond, &jit_worker.lock);
        if (jit_worker.stop && !jit_worker.head) {
            pthread_mutex_unlock(&jit_worker.lock);
            break;
        }
        JITGCCJob *job = dequeue_job();
        pthread_mutex_unlock(&jit_worker.lock);

        jit_compile_gcc_job(job);   /* blocking */

        free(job->c_src);
        free(job);
    }
    return NULL;
}
```

### 3. GCC invocation (jit_compile_gcc_job)

```c
/* Write C source to unique temp file */
char c_path[64], so_path[64];
int fd = mkstemps(c_path, 2);   /* /tmp/qjs_jit_XXXXXX.c */
write(fd, job->c_src, strlen(job->c_src));
close(fd);

snprintf(so_path, sizeof so_path, "%.*s.so", (int)strlen(c_path)-2, c_path);

/* Fork + exec GCC */
pid_t pid = fork();
if (pid == 0) {
    /* child */
    execl("/usr/bin/gcc", "gcc",
          "-O2", "-shared", "-fPIC",
          "-DCONFIG_JIT",
          "-I", JIT_INCLUDE_DIR,   /* so generated C can #include "quickjs.h" */
          "-o", so_path,
          c_path,
          (char *)NULL);
    _exit(127);
}
waitpid(pid, &status, 0);   /* main worker blocks here, ~2–5 s */
unlink(c_path);             /* remove .c; keep .so alive until dlopen */
```

The generated C needs `#include "quickjs.h"` for `JSValue` macros, so
`JIT_INCLUDE_DIR` (the quickjs source directory) is passed as `-I`.

### 4. dlopen + atomic install

```c
void *handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
unlink(so_path);    /* unlink immediately — kernel keeps .so mapped until dlclose */

if (!handle) {
    js_jit_fb_set_no_compile(job->b);   /* mark as uncompilable */
    return;
}

JSJITFunc f = (JSJITFunc)dlsym(handle, job->fname);
if (!f) {
    dlclose(handle);
    js_jit_fb_set_no_compile(job->b);
    return;
}

js_jit_fb_set_func(job->b, f, handle, /*tier=*/2);
/* ↑ atomic RELEASE store — main thread sees new jit_func on next ACQUIRE load */
```

---

## Thread safety

| Access | How it's protected |
|---|---|
| `b->jit_func` (install) | `__ATOMIC_RELEASE` store in `js_jit_fb_set_func` |
| `b->jit_func` (read) | `__ATOMIC_ACQUIRE` load in `JS_CallInternal` |
| `b->jit_no_compile` | Set before enqueueing (main thread); read after (worker) |
| Job queue head/tail | `jit_worker.lock` mutex |
| Worker sleep | `jit_worker.cond` condition variable |

No mutex is taken on the hot path (`JS_CallInternal`).  The only synchronisation is
the ACQUIRE/RELEASE pair on `jit_func`.

### Shutdown sequence

`js_jit_free()` is called from `JS_FreeRuntime()`:

```
set jit_worker.stop = 1
pthread_cond_signal  → wakes worker
pthread_join         → waits for all pending jobs to complete
```

This guarantees no worker thread is running when GC begins freeing bytecode objects.

---

## Cleanup

When a `JSFunctionBytecode` is freed (`js_jit_fb_free()`):

```c
if (jit_tier == 2 && handle)
    dlclose(handle);   /* decrements .so refcount; unmaps when zero */
jit_func  = NULL;
jit_handle = NULL;
jit_tier  = 0;
```

---

## Performance (Phase 4, no typed variables)

Without Phase 5 typed variables, the generated C still boxes/unboxes every `JSValue`
on every loop iteration.  GCC -O2 cannot eliminate the tag checks because it cannot
prove that the tag is constant across a loop iteration.

```
Benchmark           Interp min    GCC min    Speedup
──────────────────────────────────────────────────
fib(30) ×1          130 ms        161 ms     0.81×   ← regression (vtable recursion)
sum_loop(1e6) ×20  1250 ms       1451 ms     0.86×   ← regression (boxing overhead)
sum_sq(1e6) ×20     723 ms        696 ms     1.04×   ← marginal win
count_primes ×10     9.2 ms        9.2 ms    1.01×
arr_sum ×1000       426 ms        441 ms     0.97×
```

**Why GCC tier-2 alone ≈ interpreter:**

1. `JSValue` boxing in every loop iteration — `s += i` requires unbox, add, re-box per cycle
2. GCC cannot hoist tag checks out of loops (no alias/type proof)
3. `fib` recursion goes through `_RT->call` vtable (3 heap refcount ops per recursive call)
4. GCC compilation fork competes with the benchmark measurement window at threshold=2

Phase 4 is a **necessary foundation**, not a speedup by itself.  The speedups come in
Phase 5 (typed locals) and Phase 6 (IC + comparison fusion) which leverage GCC's
optimiser on unboxed C types.
