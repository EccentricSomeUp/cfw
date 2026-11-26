<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Platform matrix

This matrix records the intended support targets and the status of platform
abstractions. "Implemented" means the boundary exists and is unit-tested on
the development host; "designed" means the contract is recorded in an ADR but
not yet implemented; "planned" means no contract exists yet.

## Targets

| Platform | Status | Notes |
| --- | --- | --- |
| Linux (glibc/musl) | Primary | `clock_gettime`, `getrandom`, `epoll` planned |
| macOS | Supported | `clock_gettime`, `arc4random_buf`, `kqueue` planned |
| FreeBSD/OpenBSD/NetBSD | Supported | `clock_gettime`, `arc4random_buf`, `kqueue` planned |
| Windows (MSVC/MinGW) | Supported | `QueryPerformanceCounter`, `BCryptGenRandom`, IOCP planned |

## Compiler and language requirements

- C11 (`-std=c11`, no compiler extensions in public code)
- GCC, Clang, or MSVC-compatible C compiler
- CMake >= 3.25 as the only build system
- No third-party dependencies

## Capability status

| Capability | Linux | macOS | *BSD | Windows | Implementation |
| --- | --- | --- | --- | --- | --- |
| Monotonic clock | yes | yes | yes | yes | `cfw_time_now` (implemented) |
| Page size | yes | yes | yes | yes | `cfw_platform_page_size` (implemented) |
| CSPRNG | `getrandom` | `arc4random_buf` | `arc4random_buf` | `BCryptGenRandom` | `cfw_random_*` (implemented) |
| File metadata | `stat` | `stat` | `stat` | `_stat64` | `cfw_file_*` (implemented) |
| Descriptor close | `close` | `close` | `close` | stub: `UNAVAILABLE` | `cfw_fd_close` (POSIX; Windows deferred with IOCP) |
| Polling/event loop | `epoll` (internal) | `poll` fallback | `poll` fallback | stub | `cfw_pollset_*` internal (implemented, ADR 0014) |
| Sockets | `socket`/`accept4` | `socket`/`accept` | `socket`/`accept` | stub | `cfw_net_*` internal (implemented; numeric IPs only, no DNS) |
| HTTP/1.1 + server | POSIX only | POSIX only | POSIX only | stub: `UNAVAILABLE` | `cfw_http1*`, `cfw_server` (implemented; Windows pending IOCP) |
| Threads/mutexes | deferred | deferred | deferred | deferred | single-threaded per server; deferred |

## Portability rules

- Platform-specific code lives behind internal boundaries; public headers
  expose only portable types (`intptr_t` handles, fixed-width integers).
- Compile-time detection uses predefined macros only; runtime fallbacks
  exist where detection can be ambiguous (e.g. endianness).
- Endianness-sensitive parsing must go through explicit byte-order helpers
  when the network/HTTP layer lands; nothing may assume host order.
- Windows paths, handles, and socket types require their own adapter;
  POSIX-only calls must not leak into shared code.
