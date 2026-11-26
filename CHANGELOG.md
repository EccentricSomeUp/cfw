<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Changelog

All notable changes to this project will be documented here. The format is based
on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
semantic versioning once a stable release policy is adopted.

## [Unreleased]

### Added

- Repository bootstrap for `cfw`, a C11 dependency-free web framework
- Public API conventions with the `cfw_` prefix
- Version and status primitives
- Checked arithmetic helpers for `u32`, `u64`, `i64`, and `size_t` addition,
  subtraction, and multiplication
- `CFW_ERROR_OVERFLOW` status for representability failures
- Immutable byte-span primitive with bounds-checked subspan extraction
- Owning byte-buffer primitive with checked growth and span views
- Monotonic duration, timepoint, and deadline primitives for timeout handling
- Non-owning string-view primitive with byte-exact and ASCII case-insensitive
  comparison plus prefix/suffix checks
- Logging facade with level filtering, bounded formatted messages, pluggable
  sinks, approximate rate limiting, and dropped-message accounting
- Bump-allocator memory regions (`cfw_arena_t`) with aligned allocation,
  oversized-request handling, zeroed allocation, and bulk reset
- Platform capability detection: OS/architecture/endianness enums,
  pointer width, diagnostic names, and OS page-size query
- Owning file-descriptor wrapper (`cfw_fd_t`) with unique-ownership close
  and release semantics per the Phase 2 design record
- Cryptographic random-source boundary: OS CSPRNG bytes, `u64` draws, and
  unbiased bounded generation via rejection sampling
- Filesystem boundary primitives for regular-file size and directory checks
- Documented platform support matrix covering Linux, macOS, BSD, and Windows
- Internal pollset abstraction: level-triggered event sets with epoll on
  Linux, poll() fallback on other POSIX, deadline-aware waits, and per-fd
  tokens
- Internal nonblocking socket primitives: numeric IPv4/IPv6 addresses,
  socket/bind/listen/accept/connect wrappers, and `CFW_ERROR_WOULDBLOCK`
  status for nonblocking I/O
- HTTP/1.x token primitives: method, version, and status enums with strict
  bounded parsers and canonical names
- Allocation-free HTTP header-block iterator with strict token grammar,
  OWS trimming, and obs-fold/bare-CRLF rejection
- Strict HTTP request-line parser producing method, target, and version
  with single-space grammar and control-character rejection
- Complete request-head parser validating request line, headers, and the
  empty-line terminator with nothing permitted after it
- Request body framing detection with request-smuggling-resistant rules:
  conflicting Content-Length rejected, TE+CL rejected, only sole
  "chunked" transfer coding accepted
- Incremental zero-copy chunked-transfer decoder with bounded line state,
  strict grammar, and pipelining-safe consumption
- HTTP response serialization: status line, validated header lines, full
  response head formatting, and body-allowance rules (HEAD/1xx/204/304)
- Incremental request-head reader with configurable size limit,
  boundary-spanning terminator detection, and pipelining-safe consumption
- HTTP/1.1 server message reader: per-connection state machine with
  keep-alive, pipelining, streaming zero-copy bodies, and configurable
  head/body/pending resource limits
- HTTP/1.1 connection binding: nonblocking socket ownership, integrated
  request reads, bounded response queue, and graceful close-after-write
- Route table with segment-exact matching, `:name` path parameters, static-segment
  priority, duplicate-parameter rejection, and arena-backed storage (ADR 0026)
- HTTP target path extraction supporting origin-form, absolute-form, and
  OPTIONS `*` targets
- Request dispatch pipeline: ordered onion-model middleware with
  short-circuit cancellation, terminal handler, configurable error
  mapping (400/404/500), and automatic Content-Length response heads
- Per-route handlers via `cfw_dispatch_add_route` and the router's opaque
  route extension pointer (`cfw_router_add_ex`, ADR 0028)
- End-to-end integration test binary exercising TCP, HTTP/1.1 framing,
  dispatch, middleware, and response serialization
- `cfw_server_t` lifecycle: listener management, single-threaded event
  loop (`cfw_server_run`/`cfw_server_step`), connection limits,
  keep-alive/request timeouts, graceful stop via wake pipe, and
  accept/error/request diagnostics counters (ADR 0029)
- `Connection: close` response header on non-keep-alive requests and
  correct deadline handling during response drain
- CMake build, presets, sanitizer configuration, install rules, and CMake
  package config
- Custom unit-test harness with debug, release, and sanitizer verification
- Development, testing, security, audit, roadmap, and decision-record
  documentation
