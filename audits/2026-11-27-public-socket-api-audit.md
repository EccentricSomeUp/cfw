<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: public socket API promotion (ADR 0067)

- Date: 2026-11-27
- Auditor: Devin (agent)
- Commit: <pending — audit written pre-commit; range covers the
  net.[ch] promotion diff>
- Category: memory safety, integer/bounds, API misuse/ownership,
  platform behavior, denial-of-service
- Scope: `include/cfw/net.h`, `src/cfw/net.c`, `src/cfw/server.c`
  (consumer migration), `tests/unit/test_net.c` and socket-using
  tests, `CMakeLists.txt`, `tests/CMakeLists.txt`

## Goals

- Verify the private→public promotion introduces no memory-safety,
  bounds, ownership, or ABI regressions.
- Verify the new `cfw_net_send`/`cfw_net_recv`/`cfw_net_peer_addr`/
  `cfw_net_so_error` surface matches the documented contracts.
- Verify the opaque `cfw_addr_t` storage boundary cannot be driven
  out of bounds by callers.

## Method

- Full line-by-line review of the moved `src/cfw/net.c` against its
  previous private form, focusing on the new `cfw_addr_store` /
  `cfw_addr_to_sockaddr` conversions that replaced direct
  `sockaddr_storage` member access.
- Audit-catalog checks: memory safety (opaque storage copies are
  length-validated against `CFW_ADDR_STORAGE_CAPACITY` before
  `memcpy`; `_Static_assert` guarantees `sockaddr_storage` fits),
  integer/bounds (`uint32_t length` checked nonzero and ≤ capacity
  before use; `socklen_t` truncation guarded by the capacity check),
  API misuse/ownership (fd ownership rules unchanged from ADR 0011;
  `cfw_net_accept` still invalidates nothing on the listener and
  only writes `out` on success), platform behavior (Windows stubs
  return `CFW_ERROR_UNAVAILABLE`; POSIX errno mapping unchanged),
  DoS (nonblocking semantics preserved; `cfw_addr_resolve` remains
  documented blocking/init-time-only).
- `ctest --preset debug` and `ctest --preset sanitizers` (ASan+UBSan)
  full suites, including the extended `test_net` cases exercising
  `cfw_net_send`/`cfw_net_recv`/`cfw_net_peer_addr`/`cfw_net_so_error`
  over loopback TCP.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| A-001 | low | src/cfw/net.c `cfw_addr_store` | A peer-supplied `socklen_t` from `accept`/`getpeername` is trusted after the ≤128 capacity check; kernel-returned lengths are bounded by `sockaddr_storage` size, so no overflow is possible. | accepted (no change needed) |
| A-002 | info | include/cfw/net.h | `cfw_net_socket` still exposes raw `domain`/`type`/`protocol` constants; documented as platform constants and returning `CFW_ERROR_IO`/`UNAVAILABLE` for invalid combos rather than UB. | accepted |
| A-003 | info | src/cfw/net.c `cfw_addr_resolve` | Returns only the first getaddrinfo result per family; multi-address fallback (e.g. IPv6→IPv4 happy eyeballs) is deferred to the HTTP client design. | deferred |

## Verification

```text
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure   → 6/6 passed
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure → 5/5 passed
```

## Follow-up

- HTTP client (next ADR) will consume `cfw_addr_resolve` +
  `cfw_net_*`; revisit A-003 if connect fallback proves necessary.
- Windows/Winsock backend remains pending (ADR 0011); the public
  header already returns `CFW_ERROR_UNAVAILABLE` there.
