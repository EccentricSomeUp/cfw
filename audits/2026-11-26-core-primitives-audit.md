<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: core primitives and platform internals

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: 2615692 (range 68c9a47..2615692)
- Category: audit catalog 1 (memory safety), 2 (integer/bounds), 4 (API
  misuse), 7 (portability)
- Scope: `src/cfw/*.c`, `src/cfw/private/{pollset,net}.c`,
  `include/cfw/*.h`, `tests/unit/*`

## Goals

- Detect memory-safety, bounds, ownership, and API-misuse defects in the
  primitives added since repository bootstrap.
- Verify checked-arithmetic and overflow paths fail closed.
- Confirm platform fallback paths do not leak descriptors or truncate sizes.

## Method

- Manual line-by-line review of all translation units added in the range.
- Boundary-value reasoning for `i64` checked arithmetic, arena alignment,
  buffer growth, and pollset event fan-out.
- `cmake --preset debug` / `sanitizers` builds plus `ctest` on both.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| A-001 | High | `src/cfw/private/net.c` `cfw_net_accept` | `accept4`'s value-result `addrlen` was passed uninitialized; the kernel could read a too-small buffer size and fail `EINVAL`, or truncate the peer address | Fixed: initialize `len = sizeof(ss)` before the call |
| A-002 | Medium | `src/cfw/private/net.c` `cfw_net_socket` | In the non-`SOCK_NONBLOCK` fallback, `fd->handle` was assigned before the `fcntl` check, leaving the wrapper owning a closed descriptor on failure | Fixed: assign `fd->handle` only after `fcntl` succeeds |
| A-003 | Low | `src/cfw/private/pollset.c` `cfw_pollset_wait` (epoll) | `max_events * sizeof(struct epoll_event)` could overflow `size_t` on 32-bit targets | Fixed: reject `max_events > SIZE_MAX / sizeof(*ready)` |
| A-004 | Info | `src/cfw/log.c` rate limiter | Window reset under concurrency can rarely drop or admit an extra message at the boundary | Accepted: documented as approximate; logging must not rely on exact counts |
| A-005 | Info | `src/cfw/arena.c` | Remainder of a non-head block is stranded until reset; oversized dedicated blocks persist across resets | Accepted: documented bump-allocator semantics |

## Verification

```text
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure   # 90+ cases, all pass
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure # all pass under ASan+UBSan
```

## Follow-up

- Re-audit when `send`/`recv` wrappers, kqueue, and IOCP backends land.
- Fuzz the HTTP parser once Phase 3 begins (audit catalog 3).
