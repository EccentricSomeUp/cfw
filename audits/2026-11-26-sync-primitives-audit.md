<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: sync primitives (cfw_mutex_t / cfw_cond_t)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: working tree on `main` post-v0.2.0
- Category: audit catalog §4 (API misuse) and §5 (concurrency and
  cancellation)
- Scope: `include/cfw/sync.h`, `src/cfw/sync.c`,
  `tests/unit/test_sync.c`, CMake `Threads::Threads` linkage,
  `cmake/cfw-config.cmake.in`

## Goals

- Verify the mutex/condvar wrappers preserve the underlying primitive
  contracts (non-recursive lock, mutex-held cond wait, mutex held on
  every wait return path).
- Verify timed waits use the monotonic `cfw_timepoint_t` domain
  correctly on both backends.
- Verify storage sizing/alignment cannot silently truncate platform
  primitives.
- Verify fixed-storage objects introduce no hidden lifetime or
  allocation hazards.

## Method

- Line-by-line review of `src/cfw/sync.c` against pthread and
  SRWLOCK/CONDITION_VARIABLE semantics.
- Unit tests exercising real threads: mutual exclusion under
  contention, signal and broadcast wakeup, timed-wait timeout with
  elapsed-time bounds, expired-deadline fast path, and NULL/misuse
  argument handling.
- Debug and sanitizer presets (`ASan`+`UBSan`) via the required
  verification commands; install-smoke consumer build exercises the
  `find_package(cfw)` → `Threads::Threads` propagation.
- Attempted `-fsanitize=thread` run of the sync suite; the runtime
  aborts in this environment ("unexpected memory mapping"), so TSan
  evidence is unavailable here — recorded as a follow-up.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| A-001 | low | sync.c (Windows path) | `SleepConditionVariableSRW` millisecond rounding could return FALSE up to ~1 ms before the nanosecond deadline expires; initial code re-checked `cfw_deadline_expired` and could misreport `CFW_ERROR_INTERNAL` | fixed: deadline waits now map any non-wakeup to `CFW_ERROR_TIMEOUT` |
| A-002 | info | sync.h | Spurious wakeups remain possible per underlying primitive contract | accepted: documented; callers must predicate-loop (tests demonstrate) |
| A-003 | info | sync.c | 256-byte inline storage relies on `static_assert`s against `pthread_mutex_t`/`pthread_cond_t` sizes | accepted: compile-time guard on every POSIX build; bump pre-1.0 if a platform exceeds it |
| A-004 | info | sync.c | POSIX timed waits convert monotonic deadline to `CLOCK_REALTIME` abstime; sub-millisecond clock-read skew is immaterial for timeouts | accepted: documented in code comment |
| A-005 | low | test_sync.c | `arrived` counter read under mutex; all shared state mutex-protected or join-synchronized | fixed during review (initial draft read it unlocked) |

## Verification

```text
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure      # 6/6 pass
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure # 5/5 pass
./build/debug/tests/cfw_unit_tests --filter sync  # 6/6 pass
```

## Follow-up

- Run the sync suite under ThreadSanitizer on a host where the TSan
  runtime initializes (CI or a different kernel); the mutex/condvar
  wrappers themselves are thin, but the test interleavings deserve a
  race-detector pass before 1.0.
- Windows backend is compile-reviewed only; no Windows CI leg exists.
  Verify `SRWLOCK`/`CONDITION_VARIABLE` paths when Windows support
  becomes a target (currently stubbed per platform matrix).
