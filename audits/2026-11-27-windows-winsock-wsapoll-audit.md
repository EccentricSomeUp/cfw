<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: Windows Winsock + WSAPoll backend (ADR 0072)

- Date: 2026-11-27
- Auditor: Devin (agent)
- Commit: a7f8064
- Category: memory safety, integer/bounds, API misuse/ownership,
  platform behavior, denial-of-service, supply-chain
- Scope: `src/cfw/net.c` (Windows branch), `src/cfw/fd.c`,
  `src/cfw/pollset.c` (WSAPoll backend), `src/cfw/private/winsock.h`,
  `src/cfw/http_client.c`, `src/cfw/ws_client.c` (stub removal),
  `cmake/cfw_platform_detection.cmake`, `CMakeLists.txt` (ws2_32)

## Goals

- Verify the new Winsock/WSAPoll code preserves the ownership, bounds,
  and error-mapping contracts of the POSIX backends.
- Verify no POSIX-only assumptions leak into the shared code paths now
  compiled on Windows.

## Method

- Line-by-line review of the Windows branches against the POSIX
  reference implementations they mirror.
- Audit-catalog checks:
  - Memory safety: `cfw_addr_store`/`cfw_addr_to_sockaddr` keep the
    same length validation (`0 < len <= CFW_ADDR_STORAGE_CAPACITY`)
    before `memcpy`; `_Static_assert` still bounds
    `sockaddr_storage` to the public storage.
  - Integer/bounds: `send`/`recv` reject `size > INT32_MAX` before the
    narrowing `int` cast required by the Winsock signatures; `accept`/
    `getsockname`/`getpeername`/`getsockopt` use `int` addrlen per the
    Winsock ABI.
  - API misuse/ownership: `cfw_fd_t` ownership rules unchanged;
    `INVALID_SOCKET` maps to the invalid handle value; accepted sockets
    are set nonblocking before being published, matching the net.h
    contract.
  - Platform behavior: `WSAGetLastError` is read immediately after each
    failing call; `WSAEWOULDBLOCK`/`WSAEINPROGRESS` map to
    `CFW_ERROR_WOULDBLOCK`, preserving nonblocking connect semantics.
    `WSAStartup(2.2)` runs once per process via `InitOnceExecuteOnce`;
    `WSACleanup` is deliberately never called.
  - Denial-of-service: WSAPoll has no `FD_SETSIZE` ceiling; registration
    arrays grow geometrically like the `poll()` backend.
  - Supply-chain: no new dependencies; `ws2_32` is the OS socket
    library already implied by Windows support.
- `cmake --preset debug && cmake --build --preset debug` and
  `ctest --preset debug --output-on-failure` on Linux to confirm the
  POSIX paths are unaffected.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| W-001 | medium | all Windows socket paths | The Winsock/WSAPoll code is compile-checked on `windows-latest` CI but has never executed on a Windows host; runtime behavior (nonblocking connect completion, WSAPoll edge cases, `WSAGetLastError` ordering) is unverified. | deferred — requires a Windows test run once `cfw_server_t`/tests are ported; tracked in platform matrix |
| W-002 | low | src/cfw/net.c `cfw_net_send`/`cfw_net_recv` | `size > INT32_MAX` is rejected as `CFW_ERROR_INVALID_ARGUMENT`; callers needing larger transfers must chunk — same practical limit class as POSIX `ssize_t`. | accepted |
| W-003 | info | src/cfw/private/winsock.h | `WSAStartup` is invoked lazily per process and `WSACleanup` never runs; a long-lived process that unloads ws2_32 dynamically is out of scope. | accepted |
| W-004 | info | src/cfw/pollset.c WSAPoll backend | `WSAPoll` requires Windows 8+/Server 2012+; the pinned `_WIN32_WINNT` floor (0x0A00) already exceeds that. | accepted |

## Verification

```text
cmake --preset debug && cmake --build --preset debug   # clean
ctest --preset debug --output-on-failure               # 6/6 passed
# Windows: CI windows compile check (configure + build, MSVC /W4 /WX)
```

## Follow-up

- Port `cfw_server_t`/`cfw_http1_conn_t` (wake channel, direct
  `recv`/`send` sites) so Windows tests can execute end-to-end; then
  enable the Windows `ctest` run in CI.
- Re-evaluate IOCP (ADR 0011) if WSAPoll scalability proves
  insufficient for server workloads.
