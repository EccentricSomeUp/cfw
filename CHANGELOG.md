<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Changelog

All notable changes to this project will be documented here. The format is based
on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
semantic versioning; `0.x` releases carry no API/ABI stability guarantees —
the freeze applies at `1.0.0` (see `docs/api-stability.md`).

## [Unreleased]

### Added

- Protocol upgrades (ADR 0034): handlers may return `101 Switching
  Protocols` for requests carrying an `Upgrade` header when the new
  `cfw_server_options_t.upgrade` hook is configured; the server then
  stops HTTP framing and delivers readiness events
  (`CFW_UPGRADE_EV_*`) for the application's protocol. Upgraded
  connections get `cfw_upgraded_recv`/`cfw_upgraded_send`/
  `cfw_upgraded_close` raw I/O with input bytes buffered past the
  request handed off intact, bounded output via `max_out_size`,
  `cfw_upgraded_out_free` send-queue headroom queries, a final
  `CFW_UPGRADE_EV_CLOSE` hook event for per-connection cleanup, and
  `keep_alive_timeout` as the idle deadline. Without the hook, 101 is
  sent as an ordinary response (previous behavior preserved).
- WebSocket module (ADR 0035, RFC 6455): new `cfw/ws.h` provides
  opening-handshake helpers (`cfw_ws_handshake_validate`,
  `cfw_ws_accept_key`, `cfw_ws_handshake_accept`) backed by in-tree
  SHA-1/base64, plus a connection object (`cfw_ws_create`,
  `cfw_ws_on_event`, `cfw_ws_send*`, `cfw_ws_send_close`,
  `cfw_ws_is_closed`) for the upgraded data path: incremental masked
  frame parsing, fragmentation reassembly bounded by
  `max_message_size` (default 1 MiB, close 1009), UTF-8 validation on
  text messages (close 1007), protocol-error closes (1002), automatic
  ping→pong, close-handshake echo, and transactional fragmented sends
  with `CFW_ERROR_WOULDBLOCK` backpressure. Cleartext `ws://` only;
  `wss://` remains deferred with TLS (ADR 0030). Includes a runnable
  echo example (`examples/ws_echo.c`, `cfw_example_ws_echo`).
- File-read primitives and response-stream adapter in `cfw/fs.h`:
  `cfw_file_t` owning read-only handle (`cfw_file_open`,
  `cfw_file_read`, `cfw_file_close`) plus `cfw_file_stream_t`, a
  pull-style body source assignable to `cfw_response_t.stream` for
  streaming files without buffering them whole. This is the foundation
  for a future static-file module; path validation and traversal
  protection remain the caller's responsibility.
- Response stream cleanup hook: `cfw_response_t.stream_end`
  (`cfw_response_stream_end_fn`) runs exactly once when a streaming
  response finishes or its connection dies mid-stream, letting apps
  release per-stream resources such as open files. Includes
  `cfw_file_stream_end`, a ready-made adapter for `cfw_file_stream_t`.
- Static file serving (ADR 0036): new `cfw/static.h` provides
  `cfw_static_mount_init` + `cfw_static_serve`, a dispatch middleware
  that streams GET/HEAD requests under a URL prefix from a document
  root. Percent-decodes and validates suffixes (rejects `..`,
  encoded traversal, NUL, backslash), resolves directories to
  `index.html`, maps extensions to Content-Type, and serves through a
  fixed arena slot pool (`max_concurrent` × `buffer_size`, 503 on
  exhaustion) with cleanup via `stream_end` on completion or abort.
  File access uses confined descriptor-relative opens (ADR 0037):
  symlink components are rejected on POSIX, the final descriptor is
  verified as a regular file, and HEAD applies the same confinement
  check as GET.
- HTTP-date parsing (ADR 0038): `cfw_http_date_parse` and
  `cfw_http_date_parse_at` accept IMF-fixdate, obsolete RFC 850, and
  ANSI C `asctime` timestamps, validate calendar fields, and resolve
  RFC 850 two-digit years against an explicit or wall-clock reference
  time per the RFC 9110 50-year rule.
- Static conditional requests and Range support (ADR 0039): the static
  middleware now emits `Last-Modified`/`Accept-Ranges`, returns 304
  for satisfied `If-Modified-Since`, 412 for failed
  `If-Unmodified-Since`, and honors single-range `Range` requests with
  206/`Content-Range` (416 when unsatisfiable). New fs primitives
  `cfw_file_fstat` and `cfw_file_seek` provide handle-relative
  metadata and positioned reads; new status codes 206, 412, 416.
- multipart/form-data parsing (ADR 0043): new `cfw/multipart.h`
  module — `cfw_multipart_boundary` extracts the boundary parameter
  from Content-Type, `cfw_multipart_parse` splits a buffered body
  into parts with Content-Disposition name/filename unescaping,
  per-part Content-Type, raw header blocks, and zero-copy body spans.
  Strict delimiter semantics (CRLF-anchored boundaries, trailer
  validation, boundary-prefix collision handling) and bounded
  capacity/drop accounting.
- Query-string parsing (ADR 0042): new `cfw/query.h` module with
  `cfw_query_parse` (form-urlencoded pairs, `+`/`%XX` decoding into
  caller scratch, capacity/drop accounting, strict malformed-escape
  rejection) and `cfw_http_target_query` in `cfw/http.h` for
  extracting the query component of any request-target.
- Known-length streaming responses (ADR 0041): `cfw_response_t` gains
  `stream_size`/`CFW_STREAM_SIZE_UNKNOWN`. A stream with a declared
  length is framed with `Content-Length` + identity bytes on every
  HTTP version (no chunked TE), keeps keep-alive (including HTTP/1.0),
  emits exactly the declared bytes (over-long sources truncated), and
  closes the connection if the stream ends early. HEAD responses now
  report the real `Content-Length`. The static middleware declares
  file/range sizes, so static downloads lose chunk framing entirely.
- Static entity-tag validators (ADR 0040): the static middleware now
  emits a strong `ETag` (`"<mtime>-<size>"`) and honors the full
  RFC 9110 conditional model — `If-Match` (strong), `If-None-Match`
  (weak, →304), `If-Range` entity-tags (strong → range gating) — with
  correct precedence over the date-based validators from ADR 0039.

## [0.1.0] - 2026-11-26

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
- TLS scope decision: out of scope for the core library; boundary
  preserved for a future optional module (ADR 0030)
- API/ABI stability policy document (docs/api-stability.md)
- Platform matrix updated to reflect implemented capabilities
- Additional adversarial HTTP fuzz seeds
- Graceful drain on shutdown: `cfw_server_run` keeps serving existing
  connections after `cfw_server_stop` until `drain_timeout` expires
- `Expect: 100-continue` support: interim `100 Continue` response emitted
  at request-head time so waiting clients are not stalled
- DNS resolution for server listen hosts via getaddrinfo (init-time
  only; first address in the configured family)
- Response framing hardening: body-allowed responses now always emit a
  correct Content-Length ("0" for empty bodies, previously omitted —
  which broke keep-alive delimiting), and handler-supplied
  Content-Length is stripped whenever the server manages framing,
  preventing client hangs from mismatched values
- HEAD requests fall back to GET routes and emit Content-Length like
  GET; method mismatches now return `405 Method Not Allowed` with an
  `Allow` header instead of 404 (`allowed_methods` bitmask added to
  `cfw_route_match_t`)
- RFC 9110 Expect handling: unrecognized expectations now get
  `417 Expectation Failed` and connection close; `100-continue` token
  lists are supported; `CFW_HTTP_STATUS_EXPECTATION_FAILED` added
- RFC 9112 Host enforcement: HTTP/1.1 requests without a non-empty Host
  field are rejected with 400 and the connection closed
- Audit hardening for streaming and dispatch: `cfw_dispatch_request`
  now resets `stream`/`stream_user` on reused responses; stream drain
  deadline measures inactivity rather than total stream duration;
  handler-supplied Transfer-Encoding is stripped and Content-Length
  removed when streaming so server framing cannot be corrupted;
  rejected connections get a drain deadline
- Fixed a pollset registration leak when a connection fd closed during
  response drain: the cached handle is now used for pollset operations,
  preventing fd-number reuse from starving new connections
- Automatic `Date` response header (IMF-fixdate) unless the handler
  supplies one; `cfw_time_wall_now` wall-clock primitive and
  `cfw_http_date_format` formatter added
- Response streaming: pull-style `cfw_response_stream_fn` body source
  with chunked transfer-encoding for HTTP/1.1 and close-delimited framing
  for older versions, bounded output backpressure, and keep-alive
  preservation (ADR 0031)
- Peer address exposure: `cfw_request_t.remote_addr` carries the
  formatted "ip:port" peer address ("[v6]:port" for IPv6) for IP-based
  middleware, logging, and rate limiting; supplied via the new
  `cfw_dispatch_request_ex` variant (ADR 0032)
- Request-body upload streaming: optional `upload_begin`/`upload_write`/
  `upload_end` server hooks deliver decoded body pieces incrementally
  to an application sink, removing the `max_body_size` memory bound for
  opted-in routes; the token reaches handlers via
  `cfw_request_t.upload`/`cfw_dispatch_request_info_t` (ADR 0033)
- End-to-end request-smuggling security suite (tests/security):
  CL/TE conflicts, duplicate Content-Length, non-chunked and
  multi-coding Transfer-Encoding, obs-fold, malformed request lines,
  invalid Content-Length, and bare-LF chunked bodies all yield 400
- CMake build, presets, sanitizer configuration, install rules, and CMake
  package config
- Custom unit-test harness with debug, release, and sanitizer verification
- Development, testing, security, audit, roadmap, and decision-record
  documentation
- C++ consumer smoke test compiling all public headers as C++ and linking
  against `libcfw` (built when a C++ compiler is available)
- Install/package smoke test: scratch-prefix install plus a
  `find_package(cfw)` consumer build and run
- Adversarial HTTP fuzz corpus (`tests/fuzz/corpus`, 26 inputs) replayed
  through the parser, chunked decoder, and server pipeline by the new
  `http_fuzz_corpus` test
- GitHub Actions CI running all presets on Ubuntu and macOS (the macOS
  leg covers the `poll()` backend)
- `examples/minimal_server.c`: compilable quickstart server built by
  default (`CFW_BUILD_EXAMPLES`), keeping the README example honest
- `cfw_pollset_poll_tests`: pollset suite compiled against the `poll()`
  fallback via a test-only `CFW_POLLF_FORCE_POLL` override, verifying the
  non-epoll backend on Linux
- Fuzz suite extended to the dispatch pipeline (routing, scratch arrays,
  error mapping) and mutation iterations raised to 100k; `server_soak`
  gained 200 connection-churn cycles
- API contract comments added to previously undocumented public headers
  (span, status, version, platform, checked arithmetic, buffer)
- Server regression tests for previously uncovered paths: upload abort
  on mid-body protocol errors, stream callback failure and empty-chunk
  contract violation, warn logging on rejected requests, the blocking
  `cfw_server_run` loop with `drain_timeout`, oversized response-head
  scratch growth, and exhaustive method/status enum round-trips
- Core-primitive edge-case tests: i64 checked arithmetic, strview
  affixes, time NULL outputs, log level/throttle paths, buffer/span
  invalid state, arena `SIZE_MAX` overflow, and `cfw_fd_init(NULL)`

### Fixed

- `cfw_logger_t` in `cfw/log.h` now compiles for C++ consumers: the
  rate-limiting counter fields map to `std::atomic` under `__cplusplus`
  with size `static_assert`s preserving layout parity with the C
  `_Atomic` types; C-only warnings are no longer applied to C++ targets
