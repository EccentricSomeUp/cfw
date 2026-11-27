<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# cfw

`cfw` is a security-focused web server framework written from scratch in
C11 with no third-party runtime dependencies.

Implemented so far: core primitives (time, buffers, arenas, logging,
randomness), platform abstractions (sockets, pollset, filesystem), a
strict HTTP/1.1 parser with smuggling-resistant framing, a route table
with path parameters, an onion-model middleware dispatch pipeline, and a
single-threaded `cfw_server_t` event-loop server with connection limits,
timeouts, graceful stop/drain, automatic Date headers, RFC 9112 Host
enforcement, `Expect: 100-continue`, pull-style streaming response
bodies (Content-Length identity framing for declared sizes, chunked or
close-delimited otherwise), streaming request-body uploads via
server hooks, peer-address exposure to handlers, and opt-in protocol
upgrades (`101 Switching Protocols` takeover for application-defined
protocols, ADR 0034), WebSocket on both sides (`cfw/ws.h` server module
and `cfw/ws_client.h` synchronous-style client, RFC 6455: handshake
helpers, frame codec, fragmentation, ping/pong, close handshake,
bounded reassembly — ADRs 0035/0071), and static file serving
(`cfw/static.h`: prefix middleware, traversal-safe decoding,
index resolution, MIME types, bounded stream slots, and confined
no-follow file opens on POSIX — ADRs 0036/0037), cookie parsing and
Set-Cookie serialization (ADR 0045), plus security primitives for
application tokens: SHA-256/HMAC-SHA256 and constant-time comparison
(ADR 0046), canonical base64/hex codecs (ADR 0047), and HMAC-signed
token envelopes for cookies/CSRF/reset tokens (ADR 0048), buffered
multipart/form-data parsing (`cfw/multipart.h`, ADR 0043), and portable
mutex/condition-variable primitives and thread spawn/join/detach for
application-owned concurrency (`cfw/sync.h`, `cfw/thread.h`, ADRs
0050/0051), and a per-request completion audit hook delivering status,
byte counts, duration, and peer address for every parsed request
(ADR 0055). A public socket/address API (`cfw/net.h`, ADR 0067) and a
minimal one-shot HTTP/1.1 client (`cfw/http_client.h`, ADR 0068 —
optional TLS via a client-role `cfw_tls_ctx_t`) and a public
level-triggered pollset (`cfw/pollset.h`, ADR 0069) cover outbound
and custom-protocol connections. TLS is available as an optional OpenSSL-backed module with
`cfw_server_t` HTTPS listener support, ALPN, session resumption,
peer-certificate access, and mixed cleartext/TLS ports (ADRs
0057–0062); the default build remains dependency-free and cleartext.
Multi-instance serving via `SO_REUSEPORT` scales a single listen
address across worker threads (ADR 0056).

## Priorities

1. Security
2. Stability
3. Performance

These priorities are ordered intentionally. Features that weaken memory safety,
input validation, predictable failure behavior, or auditability require an
explicit design decision and security review before implementation.

## License

This project is licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE).

The license permits broad noncommercial use, modification, and distribution but
prohibits commercial use and does not permit closed-source redistribution of the
software. See [SECURITY.md](SECURITY.md) for vulnerability reporting and
[docs/security](docs/security) for the security process.

## Requirements

- A C11-capable C compiler
- CMake 3.25 or newer
- A platform-native build tool such as Make or Ninja
- No third-party libraries are required

## Quick start

Configure, build, and test the debug preset:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Build and test the sanitizer preset where supported:

```sh
cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure
```

The `tsan` preset (Clang only) runs the suite under ThreadSanitizer to
detect data races in the threading and server paths:

```sh
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan --output-on-failure
```

List or run selected unit tests directly:

```sh
./build/debug/tests/cfw_unit_tests --list
./build/debug/tests/cfw_unit_tests --filter server
./build/debug/tests/cfw_integration_tests   # end-to-end pipeline tests
```

The exact test executable path may differ on Windows; use the path reported by
the selected generator.

## Optional TLS module

The core library is dependency-free by default. An optional TLS
transport module (`cfw/tls.h`, ADR 0057) backed by the system
OpenSSL 3.x can be enabled at configure time:

```sh
cmake --preset debug-tls   # or: cmake -B build -DCFW_ENABLE_TLS=ON
cmake --build --preset debug-tls
ctest --preset debug-tls --output-on-failure
```

This links `libssl`/`libcrypto` and defines `CFW_HAVE_TLS` as 1 in
`cfw_config.h`. Without it, the `cfw_tls_*` entry points exist but
return `CFW_ERROR_UNAVAILABLE`. See `docs/decisions/0057` for the
license and pinning rationale.

With TLS enabled, `cfw_server_options_t.tls_ctx` turns the listener
into an HTTPS server: each accepted connection performs a TLS
handshake before HTTP parsing and all bytes flow through
`cfw_tls_read`/`cfw_tls_write` (ADR 0058). The same seam
(`cfw_http1_transport_t` in `cfw_http1_conn_options_t`) lets custom
transports wrap any `cfw_http1_conn_t`. ALPN protocol negotiation is
available via `cfw_tls_ctx_options_t.alpn_protos` and
`cfw_tls_conn_alpn` (ADR 0060), peer-certificate access via
`cfw_tls_conn_peer_cert_der` for mutual-TLS authorization (also
reachable from handlers as `req->tls_conn`, ADR 0063), session
resumption via `cfw_tls_conn_get_session`/`cfw_tls_conn_set_session`
(ADR 0062), and mixed cleartext/TLS listeners via `tls_detect`,
which sniffs each connection's first byte to serve HTTP and HTTPS on
one port (ADR 0061). Client-side connections get SNI plus optional
hostname verification via `cfw_tls_conn_set_server_name` (ADR 0064),
and the negotiated name is readable with
`cfw_tls_conn_get_server_name` (ADR 0066).

Horizontal scaling uses `cfw_server_options_t.reuse_port` (ADR 0056):
multiple `cfw_server_t` instances share one listen address via
`SO_REUSEPORT`, typically one per `cfw_thread_t` worker — see
`examples/threaded_server.c`. A loopback benchmark harness lives in
`tests/bench/bench_server.c` (`cfw_bench [requests] [warmup]
[workers] [tls]`; TLS mode needs `CFW_BENCH_TLS_CERT`/`_KEY`).

## Fuzzing

Coverage-guided fuzz targets for the parser and codec surface live in
`tests/fuzz/` (ADR 0077) and build with the opt-in `fuzz` preset:

```sh
cmake --preset fuzz        # clang: libFuzzer targets; other compilers: corpus driver
cmake --build --preset fuzz
./build/fuzz/tests/cfw_fuzz_http -max_total_time=120 tests/fuzz/corpus
```

On non-Clang toolchains the same binaries replay corpus files (and
stdin) through the identical `LLVMFuzzerTestOneInput` entry points
under ASan/UBSan. CI runs each target for 120 s per push.

## Minimal server

`examples/minimal_server.c` is a complete, compilable version of this
quickstart (built by default; disable with `-DCFW_BUILD_EXAMPLES=OFF`).
`examples/tcp_echo.c` is a nonblocking TCP echo server built entirely on
the public socket/pollset/fd APIs for custom-protocol work, and
`examples/http_client.c` shows the one-shot HTTP/1.1 client.

```c
#include <cfw/cfw.h>
#include <string.h>

static cfw_status_t hello(cfw_request_t *req, cfw_response_t *res,
                          void *user)
{
    static const char body[] = "Hello, cfw!";
    (void)req; (void)user;
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)body;
    res->body.size = sizeof(body) - 1;
    return CFW_OK;
}

int main(void)
{
    cfw_arena_t *arena = cfw_arena_create(1 << 20);
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t dopt = {0};
    cfw_server_options_t sopt = {0};
    cfw_server_t server;
    cfw_strview_t pattern;

    cfw_router_init(&router, arena);
    memset(&dopt, 0, sizeof(dopt));
    dopt.router = &router;
    dopt.handler = hello;
    dopt.max_middleware = 4;
    cfw_dispatch_init(&dispatch, arena, &dopt);
    cfw_strview_from_cstr(&pattern, "/hello");
    cfw_dispatch_add_route(&dispatch, CFW_HTTP_METHOD_GET, &pattern,
                           hello, NULL);

    memset(&sopt, 0, sizeof(sopt));
    sopt.host = "127.0.0.1";
    sopt.port = 8080;
    sopt.dispatch = &dispatch;
    sopt.http.max_head_size = 8192;
    sopt.http.max_pending_size = 65536;
    sopt.max_out_size = 8192;
    sopt.max_connections = 128;
    sopt.max_headers = 32;
    sopt.max_params = 16;
    if (cfw_server_init(&server, &sopt) != CFW_OK) {
        return 1;
    }
    cfw_server_run(&server);   /* blocks; call cfw_server_stop() elsewhere */
    cfw_server_free(&server);
    cfw_arena_destroy(arena);
    return 0;
}
```

Each `cfw_server_t` instance is single-threaded; scale out with one
instance per worker thread behind `reuse_port` (ADR 0056). The server
serves cleartext by default and HTTPS when built with
`CFW_ENABLE_TLS` and given a `tls_ctx` (ADR 0058). For production
HTTPS on a cleartext build, terminate TLS in a reverse proxy.

## Project layout

```text
include/cfw/       Public C API headers
src/cfw/           Library implementation
src/cfw/private/   Internal headers and implementation details
tests/             Custom test suites and test harness
examples/          Compilable example programs
cmake/             CMake modules and package configuration
docs/              Architecture, process, security, and decision records
```

Public API functions and types use the `cfw_` prefix. Internal symbols must not
use the public prefix unless they are intentionally part of the stable API.

## Documentation

- [Architecture](docs/architecture.md)
- [API and ABI stability](docs/api-stability.md)
- [Development process](docs/process/development.md)
- [Testing strategy](docs/process/testing.md)
- [Release checklist](docs/process/release-checklist.md)
- [Security process](docs/security/process.md)
- [Threat model](docs/security/threat-model.md)
- [Audit catalog](docs/security/audit-catalog.md)
- [Roadmap](docs/roadmap.md)
- [Decision records](docs/decisions)

## Contribution expectations

Changes are expected to be small, planned, reviewed, and tested. Security- or
stability-sensitive changes require explicit rationale, regression coverage, and
applicable audit evidence. See [AGENTS.md](AGENTS.md) and
[CONTRIBUTING.md](CONTRIBUTING.md).
