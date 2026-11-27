<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: cfw_server_t Windows port (ADR 0074)

- Date: 2026-11-27
- Auditor: Devin (agent)
- Commit: 37d3932
- Category: memory safety, integer/bounds, API misuse/ownership,
  platform behavior, denial-of-service
- Scope: `src/cfw/server.c` (platform wrappers, wake channel,
  recv/send call sites), wake-channel helper

## Goals

- Verify the Windows port preserves the server's ownership, bounds,
and shutdown semantics and introduces no new attack surface.

## Method

- Line-by-line review of the platform wrappers and the wake-channel
  helper against the POSIX paths they replace.
- Audit-catalog checks:
  - Memory safety: wrappers pass caller buffers through unchanged;
    `INT32_MAX` clamp prevents `size_t`→`int` truncation on Winsock
    `recv`/`send` lengths (reads/writes at most the requested size).
  - Integer/bounds: no new length arithmetic; `capacity - done` is
    already bounded by the staged-buffer logic.
  - API misuse/ownership: `cfw_fd_t` ownership unchanged — the wake
    helper initializes both ends, closes partial state on failure, and
    hands ownership to `server->wakeup[]` only on success.
  - Platform behavior: `WSAGetLastError` is read immediately after the
    failing call inside `cfw_server_sock_wouldblock`; would-block
    classification matches POSIX `EAGAIN`/`EWOULDBLOCK`.
  - Denial-of-service: connection/timeout/body bounds are unchanged
    (platform-neutral code); the wake listener binds `127.0.0.1:0`
    (ephemeral, loopback-only) and is closed immediately after `accept`
    during `cfw_server_init`.
- `cmake --preset debug && cmake --build --preset debug` and
  `ctest --preset debug --output-on-failure` on Linux confirm POSIX
  behavior is unchanged.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| S-001 | low | `cfw_server_wake_channel` | The loopback listener exists only inside `cfw_server_init` and accepts the first connection; a same-host process racing the bind could theoretically occupy the ephemeral port, causing init failure (fail-closed, retryable). | accepted — loopback-only, init-time, no data exposure |
| S-002 | medium | all Windows server paths | Compile-checked on `windows-latest` but never executed on Windows; the wake-channel handshake and WSAPoll readiness under real load are unverified. | deferred — enable Windows `ctest` once tests are ported |
| S-003 | info | `cfw_server_sock_recv`/`send` | `INT32_MAX` clamp means a single call can never exceed 2 GiB; all current callers use far smaller buffers. | accepted |

## Verification

```text
cmake --preset debug && cmake --build --preset debug   # clean
ctest --preset debug --output-on-failure               # 6/6 passed
# Windows: CI windows compile check (configure + build, MSVC /W4 /WX)
```

## Follow-up

- Port the test suite (or a Windows-compatible subset) so `ctest` can
  run on `windows-latest`; then revisit S-002.
- Windows TLS backend decision remains open (ADR 0072).
