<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: HTTP/1.1 client module (ADR 0068)

- Date: 2026-11-27
- Auditor: Devin (agent)
- Commit: <pending — audit written pre-commit; range covers the
  http_client.[ch] addition plus http.h/http.c response-side
  primitives>
- Category: memory safety, integer/bounds, parser/protocol state
  machines, API misuse/ownership, concurrency, platform behavior,
  denial-of-service
- Scope: `include/cfw/http_client.h`, `src/cfw/http_client.c`,
  `include/cfw/http.h` / `src/cfw/http.c` (response head parse,
  response framing, request head format, `CFW_HTTP_BODY_UNTIL_CLOSE`),
  `src/cfw/dispatch.c` (framing switch), `tests/integration/
  test_http_client.c`, `tests/unit/test_http.c`, `examples/
  http_client.c`

## Goals

- Verify the client cannot be driven into memory corruption, infinite
  loops, or unbounded resource use by a hostile or broken server.
- Verify request/response framing rules reject smuggling-prone
  responses and that caps (`max_head_size`, `max_body_size`) hold.
- Verify fd/pollset/TLS-conn lifetimes on every error path.
- Verify TLS hostname verification and SNI behavior.

## Method

- Full line-by-line review of `src/cfw/http_client.c` state machine:
  connect (SO_ERROR completion), TLS handshake drive, head
  accumulation via `cfw_http_head_reader_t`, interim-1xx skip, body
  read for all four framings, and the `done:` cleanup ladder.
- Review of the new `cfw/http.c` primitives against the existing
  request-side parsers for symmetry and bounds handling.
- Audit-catalog checks: memory safety (all buffers via
  `cfw_buffer_t`; head/body caps enforced before append; `pending`
  compaction cannot underflow — `pos <= size` invariant maintained by
  `consumed` accounting), integer/bounds (`need` head-size
  computation overflow-checked; `body.size` vs `max_body` compared in
  `uint64_t`), parser state machines (TE-over-CL precedence per
  RFC 9112, no-body statuses, HEAD suppression, chunked decoder
  reuse), API misuse/ownership (response always initialized and
  free-safe; partial results cleared on failure; TLS conn borrows
  the fd and is destroyed before `cfw_fd_close`), concurrency (all
  state is stack-local; only the caller's `tls_ctx` is shared per
  its own rules), platform behavior (Windows stub returns
  `CFW_ERROR_UNAVAILABLE`; POSIX-only `#if` boundary), DoS (single
  monotonic deadline covers connect+handshake+write+read; head and
  body caps bound memory; DNS documented as blocking/init-time).
- Defect found and fixed during review: `cfw_client_read` /
  `cfw_client_write` originally returned `CFW_OK` after a successful
  poll wait without retrying the I/O, leaving `*done == 0` — an
  infinite loop in `cfw_client_write_all` and false EOFs in reads on
  any WOULDBLOCK path. Both now loop until the operation completes,
  errors, or the deadline expires. Additionally, a malformed
  response-head parse failure was remapped from `CFW_ERROR_IO` to
  `CFW_ERROR_INVALID_ARGUMENT` to match the documented contract, and
  `cfw_net_send`/`cfw_net_recv` now zero their out-params on error.
- `cmake --preset debug && cmake --build --preset debug &&
  ctest --preset debug --output-on-failure`: all pass.
- `cmake --preset debug-tls` (TLS enabled): all pass, including the
  TLS roundtrip, wrong-CA rejection, and IP-literal hostname
  mismatch cases in `test_http_client.c`.
- `cmake --preset sanitizers && ctest --preset sanitizers
  --output-on-failure`: all pass (ASan+UBSan+LSan).

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| A-001 | high | src/cfw/http_client.c `cfw_client_read`/`cfw_client_write` | Post-wait path returned OK without performing I/O; `*done` stayed 0 → infinite write loop / false EOF on any WOULDBLOCK. Latent on fast loopback, real on real networks. | fixed before commit |
| A-002 | low | src/cfw/http_client.c `cfw_client_read_head` | Malformed response head surfaced as `CFW_ERROR_IO`, contradicting the documented `CFW_ERROR_INVALID_ARGUMENT` mapping. | fixed before commit |
| A-003 | low | src/cfw/net.c `cfw_net_send`/`cfw_net_recv` | Out-params unset on error returns; harmless to current callers but fragile for future ones. | fixed before commit |
| A-004 | info | src/cfw/http_client.c | `cfw_addr_resolve` (getaddrinfo) is blocking and outside the request deadline; documented in the header as init-time-only. | accepted (documented limitation) |
| A-005 | info | src/cfw/http_client.c | Interim 1xx heads are discarded without limit-counting; a hostile server could stream infinite 1xx responses, bounded only by the deadline and head cap per response. | accepted (deadline + per-head cap bound it) |

## Verification

```text
cmake --preset debug        && cmake --build --preset debug
ctest --preset debug --output-on-failure     → 6/6 pass
cmake --preset debug-tls    && cmake --build --preset debug-tls
ctest --preset debug-tls --output-on-failure → 6/6 pass (incl. TLS client tests)
cmake --preset sanitizers   && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure → 5/5 pass
./build/debug/cfw_example_http_client 127.0.0.1 8080 /hello
  against cfw_example_minimal_server → HTTP/1.1 200 + body
```

## Follow-up

- Keep-alive connection reuse, redirects, and streaming request
  bodies remain out of scope; revisit only with a concrete consumer
  (per ADR 0068 staged discipline).
- Windows client support awaits the Winsock/IOCP backend (ADR 0011).
