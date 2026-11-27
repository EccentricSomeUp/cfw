<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0071: WebSocket client (`cfw/ws_client.h`)

- Status: Accepted
- Date: 2026-11-27

## Context

ADR 0035 landed server-side WebSocket (`cfw/ws.h`) and ADR 0068 a
minimal one-shot HTTP/1.1 client (`cfw/http_client.h`). The audit
record for wss:// (ADR 0070) noted client-side WebSocket was deferred
"only with a concrete consumer". The public net/pollset/TLS surface
(ADRs 0067/0069/0058) now makes an in-tree client practical: the
handshake is an HTTP Upgrade exchange the client module can drive
over `cfw_net_*` + `cfw_pollset_*` + `cfw_tls_conn_*` exactly the way
`http_client.c` does, and RFC 6455 §5 framing rules are the mirror
image of the server's (client frames MUST be masked, server frames
MUST NOT be).

A WebSocket client completes the protocol story symmetrically and is
the natural consumer that exercises the whole outbound stack —
`cfw_addr_resolve`, nonblocking connect, optional TLS with SNI and
hostname verification, the pollset, and the shared ws protocol
helpers.

## Decision

Add `cfw/ws_client.h` + `src/cfw/ws_client.c`: a synchronous-style,
single-threaded WebSocket client.

```c
cfw_status_t cfw_ws_client_connect(const cfw_ws_client_options_t *o,
                                   cfw_ws_client_t **out);
void        cfw_ws_client_destroy(cfw_ws_client_t *ws);
cfw_status_t cfw_ws_client_send_text/binary/ping/pong/close(...);
cfw_status_t cfw_ws_client_pump(cfw_ws_client_t *ws,
                                const cfw_duration_t *timeout);
int         cfw_ws_client_is_closed(const cfw_ws_client_t *ws);
```

- `connect` performs DNS resolve, nonblocking connect, optional TLS
  handshake with SNI + hostname verification (`tls_ctx` selects
  `wss://`), the HTTP Upgrade request, and 101 validation including
  the `Sec-WebSocket-Accept` known-answer check — all bounded by
  `options.timeout` (blocking `getaddrinfo` is not covered, same
  contract as `cfw_http_client`).
- `pump` is the pull-style event driver: waits on the pollset,
  drains TLS pending bytes, feeds the incremental frame parser,
  auto-answers ping with pong, performs the close handshake, and
  invokes `on_message` once per complete message. Returns
  `CFW_ERROR_TIMEOUT` on an idle deadline, a protocol/IO status on
  failure, and `CFW_OK` on progress; after close it returns
  `CFW_ERROR_INVALID_ARGUMENT`-class terminal status (documented in
  the header). `is_closed` reports the terminal state.
- `send_*` performs a deadline-bounded masked-frame write directly
  (no outgoing queue): single-threaded contract means the caller —
  including `on_message` — owns the socket, and bounded blocking
  writes match the `http_client` style. Masking keys come from
  `cfw_random_bytes`.
- Frame semantics mirror `cfw/ws.c`: incremental header staging,
  7/16/64-bit lengths, fragmentation/reassembly up to
  `max_message_size` (default 1 MiB), UTF-8 validation on text,
  control frames ≤125 bytes and unfragmented, close-code validation.
  Role differences are enforced: inbound frames MUST be unmasked
  (masked server frames are a protocol violation), outbound frames
  are always masked.
- The SHA-1 / UTF-8 / close-status helpers are shared with the
  server module via `src/cfw/private/ws_proto.*` rather than
  duplicated.

## Consequences

- Adds one public header and one translation unit; no changes to the
  server-side `cfw_ws_*` API or its event contract.
- `ws_echo` example stays server-side; a `ws_client` example
  demonstrates the API.
- Subprotocol negotiation (`Sec-WebSocket-Protocol`) and extensions
  (permessage-deflate) remain out of scope; custom request headers
  are supported for auth cookies/tokens.
- Compression and proxy traversal are application concerns, same as
  for `http_client`.
