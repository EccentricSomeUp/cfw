<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0051: thread creation primitive (cfw_thread_t)

## Status

Accepted. Amends the thread-spawn deferral in ADR 0050.

## Context

ADR 0050 added `cfw_mutex_t`/`cfw_cond_t` and deferred `cfw_thread_t`
spawn/join "until a concrete in-tree consumer exists". Two developments
justify revisiting that deferral now rather than later:

- The synchronization primitives are only half a story: every
  application that wants a worker thread must still write its own
  `pthread_create`/`CreateThread` glue, including the `void*`↔`int`
  trampoline and the detached-thread resource rules — the same class
  of platform divergence the framework exists to absorb.
- Keeping the API surface minimal is cheap *now*: the design space
  (stack sizes, cancellation, TLS, thread pools) can still grow
  additively, whereas retrofitting spawn semantics onto an existing
  primitive later would be a breaking change.

## Decision

New module `cfw/thread.h` + `src/cfw/thread.c`:

- `cfw_thread_fn` — `int (*)(void *arg)`: the entry point. Its return
  value is the thread's exit status, retrievable via
  `cfw_thread_join`.
- `cfw_thread_create(cfw_thread_t *, cfw_thread_fn, void *arg)` —
  spawns a joinable thread with platform-default stack size. Returns
  `CFW_ERROR_NOMEM` on allocation failure, `CFW_ERROR_UNAVAILABLE` if
  the OS refuses the thread, `CFW_ERROR_INVALID_ARGUMENT` for NULL
  arguments.
- `cfw_thread_join(cfw_thread_t *, int *exit_code)` — blocks until the
  thread exits; `exit_code` may be NULL. Joining consumes the thread:
  the object must not be joined or detached again (undefined
  behavior), matching pthread/Win32 contract style. A consumed object
  may be reused by a subsequent `cfw_thread_create`.
- `cfw_thread_detach(cfw_thread_t *)` — releases joinability; the
  thread runs to completion and its resources are reclaimed by the
  OS. Same consume-once rule as join.

`cfw_thread_t` uses the same fixed inline storage pattern as the sync
primitives (64 bytes, `static_assert`-guarded) — no allocation is
visible to the caller; the small trampoline argument block is the only
internal heap use.

Backends:

- POSIX: `pthread_create`/`pthread_join`/`pthread_detach`.
- Windows: `CreateThread`/`WaitForSingleObject`+`GetExitCodeThread`/
  `CloseHandle` for detach. Compile-reviewed only; Windows remains a
  non-target per the platform matrix.

Explicitly out of scope (each would need its own ADR/use case):
custom stack sizes, cancellation/termination, thread-local storage,
thread naming, affinity, and any thread-pool abstraction. The server
remains single-threaded; this primitive serves application-owned
concurrency only.

## Consequences

- Additive public API (Tier 2); no existing symbol changes.
- A thread that is neither joined nor detached leaks its joinable
  state until process exit — documented in the header, same as raw
  pthread semantics.
- Exit codes are `int`; values are passed through `intptr_t`/`DWORD`
  losslessly on all supported platforms.
- Concurrency audit (catalog §5) and API-misuse audit (§4) apply;
  results recorded under `audits/`.
