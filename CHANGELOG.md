<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Changelog

All notable changes to this project will be documented here. The format is based
on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
semantic versioning; `0.x` releases carry no API/ABI stability guarantees —
the freeze applies at `1.0.0` (see `docs/api-stability.md`).

## [Unreleased]

### Added

- WebSocket client (`cfw/ws_client.h`, ADR 0071): synchronous-style
  RFC 6455 client over the public net/pollset/TLS surface —
  `cfw_ws_client_connect` performs resolve/connect/TLS/Upgrade with a
  deadline and validates `Sec-WebSocket-Accept`, `cfw_ws_client_pump`
  is the pull-style event driver with auto ping/pong and close
  handling, and `cfw_ws_client_send_*` writes masked frames. Cleartext
  `ws://` and `wss://` (client `tls_ctx` with SNI + hostname
  verification) are supported; `examples/ws_client.c` demonstrates
  both. Shared SHA-1/UTF-8/close-status helpers moved to
  `src/cfw/private/ws_proto.*`.
- `wss://` support verified and documented (ADR 0070): the WebSocket
  stack runs unchanged over TLS listeners via the transport seam.
  `examples/ws_echo.c` now accepts optional certificate/key arguments
  to serve `wss://`, and `server_tls_ws_echo` covers the full path —
  TLS handshake, masked client frames, echo, and ping/pong.
- `examples/tcp_echo.c`: nonblocking TCP echo server built entirely on
  the public socket, pollset, and fd APIs (`cfw/net.h`,
  `cfw/pollset.h`, `cfw/fd.h`) — a worked example of custom-protocol
  serving with per-connection pending queues and `CFW_POLL_OUT`
  interest management.

## [0.10.0] - 2026-11-27

### Added

- Public socket and address API (`cfw/net.h`, ADR 0067): the internal
  socket layer is now public. `cfw_addr_t` is a portable opaque
  128-byte address value with `cfw_addr_from_str`/`cfw_addr_resolve`
  (numeric literals and blocking getaddrinfo), `cfw_addr_to_str`,
  `cfw_addr_port`, `cfw_addr_is_ipv4`/`cfw_addr_is_ipv6`, and
  `cfw_addr_any`; `cfw_net_socket`/`bind`/`listen`/`accept`/
  `connect`/`local_addr` manage nonblocking close-on-exec sockets
  owned by `cfw_fd_t`; `cfw_net_send`/`cfw_net_recv` provide
  nonblocking I/O; `cfw_net_peer_addr` and `cfw_net_so_error`
  (SO_ERROR read-and-clear for connect completion) round out the
  surface. All functions return `CFW_ERROR_UNAVAILABLE` on Windows.

- Minimal HTTP/1.1 client (`cfw/http_client.h`, ADR 0068):
  `cfw_http_client_request` performs one request per call — resolve,
  nonblocking connect, optional TLS (client-role `cfw_tls_ctx_t` with
  SNI and hostname verification), send, and full response read under a
  single monotonic deadline. `cfw_http_client_response_t` owns the raw
  response head and decoded body; `cfw_http_client_response_head`
  re-parses the head for status/headers. Supports all methods, caller
  headers (framework-managed names rejected), request bodies,
  Content-Length/chunked/close-delimited response framing, interim 1xx
  skipping, and `max_head_size`/`max_body_size` caps. POSIX only;
  `CFW_ERROR_UNAVAILABLE` on Windows or without TLS support.

- Response-side HTTP primitives (`cfw/http.h`):
  `cfw_http_response_head_parse` parses status line + headers,
  `cfw_http_response_framing` applies RFC 9112 response framing rules
  including the new `CFW_HTTP_BODY_UNTIL_CLOSE`, and
  `cfw_http_request_head_format` serializes request heads.

- `examples/http_client.c`: CLI GET client demonstrating the minimal
  request/response path.

- Public pollset API (`cfw/pollset.h`, ADR 0069): the internal
  readiness event set is now public, completing the nonblocking I/O
  toolkit alongside `cfw/net.h`. `cfw_pollset_create`/`destroy`/
  `add`/`modify`/`remove`/`wait` provide level-triggered readiness
  with per-registration tokens and deadline-bounded waits (epoll on
  Linux, kqueue on macOS/BSD, poll fallback; `CFW_ERROR_UNAVAILABLE`
  on Windows). No behavior changes — promotion only.

## [0.9.0] - 2026-11-27

### Added

- `examples/static_files.c`: static file server example mounting
  `cfw_static_serve` (conditional requests, Range, ETag) alongside a
  dynamic route.
- `examples/multipart_upload.c`: multipart/form-data parsing example
  (`cfw_multipart_boundary` + `cfw_multipart_parse`) reporting part
  names, filenames, and sizes.
- TLS SNI query (ADR 0066): `cfw_tls_conn_get_server_name` returns
  the SNI hostname — the client's offered name on server connections
  (virtual-host routing) or the installed name on client connections.

### Fixed

- Dispatch middleware now runs for requests that match no route
  (ADR 0065): the 404/405 response moved to the end of the middleware
  chain, so prefix-intercepting middleware such as
  `cfw_static_serve` can actually serve unmatched paths — previously
  they 404'd before middleware ran, making static mounts inert.

## [0.8.0] - 2026-11-27

### Added

- `examples/mtls_authz.c`: mutual-TLS authorization example —
  mandatory client certificates plus a SHA-256 fingerprint
  allowlist checked through `req->tls_conn`.
- TLS session exposure to handlers (ADR 0063):
  `cfw_request_t.tls_conn` carries the connection's
  `cfw_tls_conn_t` (borrowed, valid for the dispatch call) when the
  request arrived over TLS — NULL on cleartext — enabling
  mutual-TLS authorization via `cfw_tls_conn_peer_cert_der`,
  ALPN-aware routing via `cfw_tls_conn_alpn`, and session-reuse
  telemetry. Supplied through
  `cfw_dispatch_request_info_t.tls_conn`.
- HTTPS benchmarking: `cfw_bench` accepts a `tls` mode
  (`cfw_bench [requests] [warmup] [workers] tls`) that runs the same
  keep-alive measurement over TLS. Requires a `CFW_ENABLE_TLS` build
  and `CFW_BENCH_TLS_CERT`/`CFW_BENCH_TLS_KEY` (default
  `server.crt`/`server.key`).
- TLS client server name (ADR 0064):
  `cfw_tls_conn_set_server_name` sends SNI in the ClientHello and,
  with `verify_peer`, pins hostname verification against the server
  certificate — previously `verify_peer` checked only the chain.

### Fixed

- TLS connections no longer die spuriously under multi-threaded
  serving: OpenSSL's error queue is per-thread, so errors left by one
  connection's teardown could make `SSL_get_error` report
  `SSL_ERROR_SSL` for unrelated transient results on the next
  connection serviced by the same thread. `cfw_tls_*` operations now
  drain the queue (`ERR_clear_error`) before each `SSL_*` call.
  Found via the new TLS benchmark mode; see
  `audits/2026-11-27-tls-error-queue-isolation-audit.md`.
- Accepted TCP connections now set `TCP_NODELAY` (feature-probed via
  `CFW_HAVE_TCP_NODELAY`), disabling Nagle's algorithm so small
  request/response exchanges are not delayed by delayed-ACK
  interaction.

## [0.7.0] - 2026-11-27

### Added

- Mixed cleartext/TLS listeners (ADR 0061):
  `cfw_server_options_t.tls_detect` sniffs the first byte of each
  accepted connection — a TLS handshake record gets TLS, anything
  else is served as cleartext HTTP on the same port. Requires
  `tls_ctx`; `cfw_server_init` rejects `tls_detect` without it.
- TLS session resumption API (ADR 0062):
  `cfw_tls_conn_get_session`/`cfw_tls_conn_set_session` serialize
  and install `SSL_SESSION` blobs so clients can cache and resume
  sessions across connections (server-side caching uses OpenSSL's
  built-in per-context cache), and `cfw_tls_conn_session_reused`
  reports whether a handshake resumed. Only available with TLS
  support (`CFW_HAVE_TLS`).

## [0.6.0] - 2026-11-27

### Added

- Optional TLS transport module `cfw/tls.h` backed by OpenSSL 3.x
  (ADR 0057, implementing the deferred Phase 6 boundary from ADR
  0030): `cfw_tls_ctx_t` for certificate/trust/verify configuration
  and `cfw_tls_conn_t` for nonblocking `SSL_accept`/`SSL_read`/
  `SSL_write`/`SSL_shutdown` over `cfw_fd_t` sockets with explicit
  want-read/want-write hints and `SSL_pending` reporting for
  pollset-driven event loops. Enabled with `-DCFW_ENABLE_TLS=ON`
  (CMake option `CFW_ENABLE_TLS`, default OFF — the default build
  remains dependency-free); `CFW_HAVE_TLS` in `cfw_config.h` reports
  availability and all entry points return `CFW_ERROR_UNAVAILABLE`
  when compiled out.
- `cfw_server_t` HTTPS listener support (ADR 0058): setting
  `cfw_server_options_t.tls_ctx` makes every accepted connection
  perform a TLS handshake before HTTP parsing, with poll interest
  driven by the TLS want-direction hints during the handshake and
  `close_notify` sent best-effort on teardown. `cfw_server_init`
  returns `CFW_ERROR_UNAVAILABLE` when `tls_ctx` is set in a
  non-TLS build.
- `cfw_http1_conn_options_t.transport`: optional
  `cfw_http1_transport_t` read/write override letting any byte-stream
  transform (TLS today) wrap a `cfw_http1_conn_t` while the conn
  retains fd ownership. Options structs must now be zero-initialized
  (`memset`) so optional fields default off — all in-tree callers
  updated. Cleartext behavior is unchanged when `transport` is NULL.
- `debug-tls` CMake preset exercising the TLS module; TLS unit tests
  generate their CA/server/client certificates into the build tree at
  configure time (no key material committed).
- Peer certificate access (ADR 0057 follow-up):
  `cfw_tls_conn_peer_cert_der` returns the DER encoding of the
  certificate the peer presented — the identity needed for
  mutual-TLS authorization — with a size-then-fetch contract
  (`CFW_ERROR_OVERFLOW` reports the required buffer size) and an
  empty result when no certificate was sent. TLS builds only.
- TLS ALPN negotiation (ADR 0060): `cfw_tls_ctx_options_t.alpn_protos`
  configures a protocol list — servers select by server preference
  with NOACK fallback when nothing matches, clients send their list
  in the ClientHello, and `cfw_tls_conn_alpn` reports the negotiated
  protocol. Available in `CFW_ENABLE_TLS` builds.
- Streaming HMAC-SHA256: `cfw_hmac_sha256_init`/`update`/`final`
  allow incremental MAC computation over large or arriving-in-pieces
  data; `cfw_hmac_sha256` is now implemented on top of the streaming
  API with identical output (all RFC 4231 vectors still pass).
- Base64url codec variant (ADR 0059): `cfw_base64url_encode`/
  `cfw_base64url_decode`/`cfw_base64url_encode_bound` implement RFC
  4648 section 5 for URL-safe tokens — unpadded canonical encoding,
  decoding that accepts unpadded or canonically padded input with
  the same strictness as the standard base64 decoder.
- `cfw_example_tls_server` example demonstrating `tls_ctx` HTTPS
  serving; compiles in all builds and reports a clear message when
  TLS support is not enabled.

### Fixed

- `cfw_upgraded_recv` now honors the connection transport, so
  protocol upgrades (WebSocket/`wss://` and the generic `upgrade`
  hook) work over TLS listeners instead of receiving ciphertext.

### Changed

- Reduced per-request event-loop overhead: connection interest changes
  are only pushed to the pollset when the mask actually changes (the
  listener is no longer re-`epoll_ctl`'d every step), queued responses
  are flushed opportunistically instead of always waiting for a
  `POLLOUT` wakeup, and the epoll/kqueue backends reuse their modify
  records and wait buffers instead of allocating per call. Loopback
  `cfw_bench` improved ~10% single-worker (~9.7k → ~10.8k req/s) and
  ~25% at four workers (~46k → ~58k req/s).

## [0.5.0] - 2026-11-27

### Added

- `SO_REUSEPORT` multi-instance serving (ADR 0056): new `reuse_port`
  option on `cfw_server_options_t` lets multiple `cfw_server_t`
  instances — typically one per `cfw_thread_t` worker — share a
  listen address with kernel-level connection distribution. Support
  is compile-probed (`CFW_HAVE_SO_REUSEPORT`); init fails loudly with
  `CFW_ERROR_UNAVAILABLE` on platforms without it. This delivers the
  deferred Phase 2/5 scaling path now that `cfw/thread.h` and
  `cfw/sync.h` exist.
- `examples/threaded_server.c`: reference multi-worker server — N
  `cfw_thread_t` workers each running a `cfw_server_t` with
  `reuse_port` on a shared dispatch, with a mutex/condvar startup
  rendezvous.
- `tests/bench/bench_server.c` (`cfw_bench`): manual loopback
  keep-alive benchmark reporting requests/second and response
  throughput for `cfw_server_t` (POSIX builds, not a CTest case).
- Per-request completion audit hook (ADR 0055): new optional
  `request_done`/`request_done_user` fields on `cfw_server_options_t`
  deliver a `cfw_request_record_t` exactly once per parsed request
  head — method, target, version, final status (0 when no response
  was produced), decoded request-body bytes, response bytes handed to
  the writer, monotonic duration, peer address, and an upgraded flag.
  Records cover dispatched responses, server-generated errors and
  rejections, finished or aborted streams, 101 upgrades, and
  connections killed mid-request. This closes the roadmap's deferred
  "structured audit logging" item; applications can build access logs
  and security monitoring on it without per-request allocation.

## [0.4.0] - 2026-11-26

### Added

- CI coverage for additional platforms: a FreeBSD job runs the `debug`
  preset inside a QEMU VM (`vmactions/freebsd-vm`, releases 14 and 15),
  exercising the kqueue pollset backend end-to-end, and a Windows job
  performs a compile-only check (configure + build, no tests) to catch
  POSIX-isms leaking into shared code while the Windows socket/pollset
  stubs remain `CFW_STATUS_UNAVAILABLE` (ADR 0011).
- Portable maximum-alignment type (ADR 0054): new public header
  `cfw/max_align.h` defining `cfw_max_align_t`, a C11-portable union
  that replaces `max_align_t` (a POSIX extension absent from MSVC) in
  the inline-storage alignment padding of `cfw_mutex_t`, `cfw_cond_t`,
  and `cfw_thread_t`. No layout or ABI change.

### Changed

- Internal platform and feature detection (ADR 0053): raw compiler/OS
  macros are no longer scattered through the sources. CMake now probes
  capabilities at configure time and generates an internal
  `CFW_PLATFORM_*`/`CFW_HAVE_*` header; a single private
  `platform_detect.h` owns feature-test macro defaults and normalized
  selection. Pollset backend selection is explicit and compile-fixture
  targets now exercise the epoll, kqueue (via a stub header on
  non-kqueue hosts), poll, and none paths on every build. No public API
  or behavioral change.

### Fixed

- macOS/BSD polling: kqueue readiness was gated on BSD `NOTE_*`
  fflags, but those flags describe vnode/user events rather than
  READ/WRITE readiness. Delivered `EVFILT_READ`/`EVFILT_WRITE` events
  now map directly to `CFW_POLL_IN`/`CFW_POLL_OUT`, with `EV_EOF`
  reported as `CFW_POLL_HUP` and `EV_ERROR` as `CFW_POLL_ERR`.
- macOS build: the kqueue pollset backend was nested inside the epoll
  `#ifdef` branch, so on macOS/BSD it never compiled — the pollset fell
  through to the unavailable-backend stubs and the shared helpers were
  unused, failing `-Werror` builds. The backend selection is now a flat
  `epoll / kqueue / poll / none` conditional chain, so kqueue actually
  builds on Apple/BSD targets.
- macOS build: `cfw_file_open_mode` was compiled whenever `O_NOFOLLOW`
  was visible, but macOS hides that BSD extension under
  `_POSIX_C_SOURCE 200809L`, leaving the helper unused and failing
  `-Werror` builds. The helper is now guarded on `defined(O_NOFOLLOW)`
  so it only exists where it is actually used.
- macOS build: `cfw_log` failed Clang's `-Wformat-nonliteral` (enabled
  by `-Wformat=2`) because the printf-style format is a runtime
  parameter. The pass-through `vsnprintf` call is now wrapped in a
  scoped diagnostic suppression, and `cfw_log` is annotated with a new
  portable `CFW_PRINTF_FORMAT` attribute so callers still get compile
  -time format checking.
- macOS build: `cfw_net_socket` passed `SOCK_CLOEXEC` to `socket()`
  unconditionally, but macOS and the BSDs do not define that flag.
  `SOCK_CLOEXEC` is now guarded like `SOCK_NONBLOCK`, with close-on-exec
  applied via `fcntl(F_SETFD, FD_CLOEXEC)` where the flag is unavailable;
  the non-Linux `accept` path now sets `FD_CLOEXEC` on accepted sockets
  as well, matching the `accept4` behavior on Linux.
- macOS/BSD portability audit: `_POSIX_C_SOURCE` hides several Darwin
  extensions, so affected translation units now also define
  `_DARWIN_C_SOURCE` on `__APPLE__`. This restores `SO_NOSIGPIPE` in
  the socket layer (previously SIGPIPE suppression silently no-oped,
  so a client disconnect could kill the server), `O_NOFOLLOW`/`openat`
  in the confined file open (previously returned
  `CFW_ERROR_UNAVAILABLE`, breaking static file serving and its tests
  on macOS), and the `arc4random_buf` declaration in the CSPRNG
  (previously an implicit-declaration build failure).
- macOS/BSD build: `cfw_server_init` passed `SOCK_NONBLOCK` to
  `socketpair()`, a flag only Linux defines. The wake channel is now
  created with plain `SOCK_STREAM` and configured nonblocking and
  close-on-exec via `fcntl`, with `SO_NOSIGPIPE` applied where
  available. The wakeup drain also no longer requires `MSG_DONTWAIT`,
  which some BSDs do not define.
- Pollset test coverage: `CFW_POLLF_FORCE_POLL` previously only
  overrode the epoll backend, so on macOS/BSD the "poll fallback" test
  binary still exercised kqueue and the `poll()` backend was never
  compiled there. The override now takes precedence on every POSIX
  platform.
- macOS build: the static-file mount's slot and scratch-pointer casts
  from `unsigned char *` tripped Clang `-Wcast-align`. The casts now
  go through `void *`; alignment is still guaranteed by the arena's
  `cfw_max_align_t` allocation alignment and the slot stride.
- Windows build: `max_align_t` is a POSIX extension, not ISO C11, and
  is absent from MSVC — it broke `cfw_mutex_t`, `cfw_cond_t`,
  `cfw_thread_t`, and the arena alignment constant. The new portable
  `cfw_max_align_t` (ADR 0054) replaces it everywhere.
- Windows build: MSVC only exposes C11 library features such as
  `<stdatomic.h>` in `/std:c11` mode. C sources in strict-flag targets
  now compile with `/std:c11`, scoped by a `COMPILE_LANGUAGE`
  generator expression so C++ consumers are unaffected.
- Windows build: `fs.c` used the POSIX-only `S_ISREG`/`S_ISDIR` mode
  tests (now defined over `_S_IFMT` for MSVC), referenced an
  undefined `CFW_FILE_FSTAT_FUNC` (typo for `CFW_FILE_STAT_FUNC`,
  `_fstat64`), and drew `_CRT_SECURE_NO_WARNINGS`-gated deprecation
  warnings for `_open`/`_read`/`_close`/`_lseeki64`.
- Windows build: the endianness runtime probe in `platform.c` was
  unreachable code on known little-endian targets (MSVC C4702 under
  `/W4`). It is now a macro expanded only where no compile-time
  answer exists.
- Windows build: `cfw_multipart_parse` could read `next`
  uninitialized when MSVC's flow analysis could not prove
  `cfw_mp_delim_trailer` had set it (C4701); it is now initialized,
  matching the first delimiter loop.
- FreeBSD build: the configure-time `accept4` probe compiled despite
  the function being undeclared (implicit declarations are only
  warnings in the probe's default dialect), so `CFW_HAVE_ACCEPT4` was
  wrongly enabled and the strict `-Werror` build failed. Feature probes
  now compile with `-Werror=implicit-function-declaration`, and the
  accept path falls back to `accept` + `fcntl` on the BSDs.
- macOS build: `cfw_sync_to_abstime` passed `&abstime->tv_sec` /
  `&abstime->tv_nsec` (`long *` on Darwin) to `cfw_checked_add_i64`
  (`int64_t *`), and every pthread mutex/cond cast from
  `unsigned char *` storage tripped Clang `-Wcast-align`. The deadline
  arithmetic now uses `int64_t` temporaries assigned to the `timespec`
  fields, and the primitive pointers are formed through the union's
  `cfw_max_align_t` member so no cast increases required alignment.
- Windows build: MSVC's `<stdatomic.h>` additionally requires
  `/experimental:c11atomics` on top of `/std:c11`; C sources in
  strict-flag targets now pass both. The SRWLOCK/condition-variable
  APIs require `_WIN32_WINNT >= 0x0600`, so `platform_detect.h` now
  pins a modern floor, and `sync.c` calls the correctly spelled
  `InitializeConditionVariable`. `_CRT_SECURE_NO_WARNINGS` in `fs.c`
  was defined after `<stdio.h>`/`<string.h>` had already pulled in the
  CRT headers, so it is now defined before any include.
- macOS/FreeBSD build: an adjacent string-literal pair in the malformed
  multipart fixture tripped Clang's `-Wstring-concatenation` under
  `-Werror`; the intended single literal is now explicitly
  parenthesized.

## [0.3.0] - 2026-11-26

### Added

- Synchronization primitives (ADR 0050): new `cfw/sync.h` module with
  `cfw_mutex_t` (`cfw_mutex_init`/`destroy`/`lock`/`try_lock`/`unlock`)
  and `cfw_cond_t` (`cfw_cond_init`/`destroy`/`wait`/`signal`/
  `broadcast`), backed by pthreads on POSIX and
  SRWLOCK/CONDITION_VARIABLE on Windows. Fixed inline storage — no
  allocation — non-recursive mutex semantics,
  `CFW_ERROR_WOULDBLOCK` from contended try-locks, and
  `cfw_cond_wait` with an optional monotonic `cfw_timepoint_t`
  deadline returning `CFW_ERROR_TIMEOUT`. Enables safe sharing of
  state between a server's event-loop thread and application-owned
  threads; thread creation itself remains deferred. `libcfw` now
  links the platform thread library (`Threads::Threads`, resolved
  automatically by `find_package(cfw)`).
- Thread spawn primitive (ADR 0051): new `cfw/thread.h` module with
  `cfw_thread_create`/`cfw_thread_join`/`cfw_thread_detach` —
  joinable thread creation with `int` exit statuses, fixed inline
  handle storage, pthreads on POSIX and `CreateThread` on Windows.
  Scoped to spawn/join/detach only; stack sizes, cancellation, TLS,
  and pools remain deferred. Completes the application-concurrency
  story begun by the sync primitives (ADR 0050).
- kqueue pollset backend (ADR 0052): the internal event-set
  abstraction now uses `kqueue`/`kevent` on macOS and the BSDs
  instead of the O(n) `poll()` fallback. Per-handle records map
  kevent `udata` for O(1) event lookup, READ/WRITE filter readiness
  merges into single level-triggered events, and timeouts keep
  nanosecond precision via relative `timespec` deadlines. The `poll()`
  fallback remains for other POSIX; abstraction contract unchanged.

## [0.2.0] - 2026-11-26

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
- Signed tokens (ADR 0048): new `cfw/signed.h` module —
  `cfw_signed_encode`/`cfw_signed_decode` implement a
  versioned `v1.<base64(payload)>.<base64(HMAC-SHA256)>` envelope
  for signed cookies, CSRF tokens, and similar caller-bound values.
  Allocation-free encode (payload segment encoded in place, MAC over
  the contiguous encoded prefix), constant-time verification, strict
  canonical-base64 parsing, and `cfw_signed_encode_bound` for
  capacity planning. Integrity only — no confidentiality, expiry, or
  replay protection (callers embed those in the payload).
- Byte-to-text codecs (ADR 0047): new `cfw/codec.h` module with
  canonical RFC 4648 base64 (`cfw_base64_encode`/`decode` +
  `cfw_base64_encode_bound`) and hex (`cfw_hex_encode`/`decode` +
  `cfw_hex_encode_bound`). Decoders are strict — canonical padding
  and pad bits for base64, even-length digit-only for hex — for
  token/signature material. `ws.c` now uses the shared codec instead
  of private base64 copies, tightening key validation to canonical
  form.
- Cryptographic primitives (ADR 0046): new `cfw/crypto.h` module with
  SHA-256 (`cfw_sha256_init`/`update`/`final` streaming API and
  one-shot `cfw_sha256`), HMAC-SHA256 (`cfw_hmac_sha256`, RFC 2104),
  and `cfw_secure_equal` constant-time comparison. Intended for
  integrity tags, MACs (signed cookies, tokens, webhook signatures),
  and non-adversarial checksums — explicitly not a password-hashing
  function (memory-hard KDFs remain out of scope).
- Cookie parsing and serialization (ADR 0045): new `cfw/cookie.h`
  module — `cfw_cookie_parse` splits a Cookie header into zero-copy
  name/value pairs (token names, cookie-octet values, DQUOTE
  stripping, strict malformed-input rejection), and
  `cfw_cookie_serialize` renders Set-Cookie values with Path,
  Domain, Max-Age, Expires, Secure, HttpOnly, Partitioned, and
  SameSite attributes.
- URI percent-decoding primitive (ADR 0044): new `cfw/uri.h` module
  with `cfw_uri_decode` — strict `%XX` decoding, NUL-terminated
  output, and flags for `+`→SP (form components), NUL rejection, and
  backslash rejection (filesystem paths). `cfw_query_parse` and the
  static-file path decoder now share it; new
  `cfw_route_param_decode` helper decodes route parameter values on
  demand. Route matching still operates on the raw encoded path, so
  `%2F` cannot fabricate segments. `cfw_query_parse` scratch
  requirement is now `query->size + 1`.
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
