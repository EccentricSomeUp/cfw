<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: thread primitive (cfw_thread_t)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: working tree on `main` post-v0.2.0, ADR 0051
- Category: audit catalog §4 (API misuse) and §5 (concurrency and
  cancellation)
- Scope: `include/cfw/thread.h`, `src/cfw/thread.c`,
  `tests/unit/test_thread.c`

## Goals

- Verify the consume-once join/detach contract is documented and cannot
  be silently violated into double-free or leaked-handle states by the
  implementation itself.
- Verify the `fn`/`arg` trampoline handoff cannot leak or double-free
  the parameter block on any create/join/detach path.
- Verify exit-status round-tripping (`int` through `intptr_t`/`DWORD`)
  is lossless.
- Verify thread objects carry no hidden allocation or ownership the
  caller cannot see.

## Method

- Line-by-line review of `src/cfw/thread.c` against pthread and
  Win32 `CreateThread` semantics, including every failure path
  (`malloc`, `pthread_create`/`CreateThread`, `pthread_join`,
  `WaitForSingleObject`, `GetExitCodeThread`, `pthread_detach`,
  `CloseHandle`).
- Unit tests with real threads: exit-code round-trip (positive,
  negative, NULL out-param), detached-thread completion signaled via
  `cfw_cond_t` with a deadline bound, and a 4-thread mutex-guarded
  counter.
- Debug and sanitizer presets via the required verification commands.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| A-001 | info | thread.c | Trampoline frees the `{fn,arg}` block exactly once on every path: create failure frees it in the caller; after `fn` returns the trampoline frees it. No path double-frees or leaks | verified by review |
| A-002 | info | thread.h | Join/detach consume-once is a documented contract (UB on violation), matching pthread semantics; the implementation stores a plain handle with no internal state that could mask misuse | accepted: consistent with platform threading contracts |
| A-003 | low | thread.c | POSIX `pthread_create` failure maps `ENOMEM`→`NOMEM`, all else→`UNAVAILABLE`; `pthread_join`/`detach` failures map to `INTERNAL` | accepted: failure classes are coarse but honest; no silent success |
| A-004 | info | thread.c | Windows exit code is `DWORD`; `(int)code` conversion is implementation-defined for codes > INT_MAX, but `cfw_thread_fn` returns `int` so only int-range values are ever produced | accepted |
| A-005 | info | thread.c | Joinable-but-never-joined threads leak joinable state until exit/exit of process — same as raw pthreads | accepted: documented in header |

## Verification

```text
cmake --preset debug && cmake --build --preset debug
./build/debug/tests/cfw_unit_tests --filter thread   # 3/3 pass
ctest --preset debug --output-on-failure             # all pass
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure        # all pass
```

## Follow-up

- TSan pass on the thread/sync suites remains outstanding (runtime
  unavailable in this environment; see the sync audit).
- Windows path is compile-reviewed only, consistent with the platform
  matrix status.
