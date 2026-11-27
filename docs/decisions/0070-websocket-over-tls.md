<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0070: WebSocket over TLS listeners (wss://)

- Status: Accepted
- Date: 2026-11-27

## Context

ADR 0035 deferred `wss://` pending "the deferred TLS decision
(ADR 0030)". That decision has since been made and implemented:
OpenSSL-backed TLS is an optional module (ADR 0057), the
`cfw_http1_transport_t` seam carries every connection's byte stream
(ADR 0058), and upgraded protocols ride the same transport — the
architecture document already described `wss://` as working
end-to-end, but nothing exercised the full RFC 6455 stack (handshake
helpers, masked client frames, `cfw_ws_*` framing) over TLS.

## Decision

`wss://` is supported on any `cfw_server_t` listener configured with
`tls_ctx`: no additional wiring is required because the upgrade path
(ADR 0034) and the `cfw_ws_*` module are transport-agnostic. The
ws_echo example accepts optional certificate/key arguments to serve
`wss://`, and `server_tls_ws_echo` verifies the complete path —
TLS handshake, HTTP upgrade, masked client frame, echo, and
ping/pong — in the unit suite.

Scope boundaries are unchanged: server-side only (there is still no
client-side WebSocket API), cleartext `ws://` remains the default,
and no extensions (permessage-deflate etc.) are negotiated.

## Consequences

- `cfw/ws.h` and `examples/ws_echo.c` documentation updated to
  reflect that `wss://` works; the stale "cleartext only" notes are
  removed.
- Deployments terminating TLS in a reverse proxy can still serve
  plain `ws://` internally; direct `wss://` is now a first-class,
  tested option.
