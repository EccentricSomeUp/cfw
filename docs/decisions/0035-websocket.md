# ADR 0035: WebSocket support (RFC 6455)

## Status

Proposed.

## Context

ADR 0034 landed protocol-upgrade plumbing: a handler can return
`101 Switching Protocols` and the server hands the connection to an
application hook with raw nonblocking I/O. WebSocket is the primary
consumer of that API and the audit catalog already names "websocket or
upgrade paths" as a parser/protocol audit focus.

RFC 6455 has two distinct halves:

1. **Opening handshake** — HTTP request with `Upgrade: websocket`,
   `Connection: Upgrade`, `Sec-WebSocket-Key`, `Sec-WebSocket-Version:
   13`; server answers 101 with `Sec-WebSocket-Accept` =
   `base64(SHA-1(key || GUID))`.
2. **Data path** — a framed binary protocol: FIN/opcode, client frames
   must be masked, 7/16/64-bit lengths, control frames (close/ping/pong)
   ≤125 bytes and unfragmented, text messages must be valid UTF-8,
   fragmentation via continuation frames.

Constraints inherited from the project:

- No third-party dependencies; SHA-1 and base64 must be implemented
  in-tree. SHA-1 here is a protocol-mandated transform (the accept key
  is not a security boundary — it proves the server read the request,
  not secrecy), but the implementation still needs known-answer tests.
- Bounded memory per connection: message reassembly and frame parsing
  must be incremental and capped.
- Single-threaded pull model: the module consumes `cfw_upgraded_conn_t`
  readiness events; it must not block.
- Cleartext only: `ws://` over the existing transport. `wss://` waits
  on the deferred TLS decision (ADR 0030).

## Decision

Add a `cfw_ws_*` module in a new public header `cfw/ws.h` with three
layers:

### 1. Handshake helpers (used inside the dispatch handler)

```c
cfw_status_t cfw_ws_handshake_validate(const cfw_request_t *req);
cfw_status_t cfw_ws_accept_key(const char *key, char out[28]);
```

- `cfw_ws_handshake_validate` checks method GET, `Upgrade: websocket`,
  `Connection` containing `upgrade`, `Sec-WebSocket-Version: 13`, and
  a well-formed `Sec-WebSocket-Key` (base64 decoding to exactly 16
  bytes). Returns `CFW_ERROR_INVALID_ARGUMENT`-class status on failure;
  the handler maps it to 400/426 as it sees fit.
- `cfw_ws_accept_key` computes the accept value; the handler places it
  in the 101 response's `Sec-WebSocket-Accept` header (and may add
  `Sec-WebSocket-Protocol` after validating against its own list).

### 2. Connection object (used inside the upgrade hook)

```c
typedef struct cfw_ws cfw_ws_t;              /* opaque */
cfw_ws_t *cfw_ws_create(cfw_upgraded_conn_t *conn,
                        const cfw_ws_options_t *options);
void cfw_ws_destroy(cfw_ws_t *ws);
cfw_status_t cfw_ws_on_event(cfw_ws_t *ws, uint32_t events);
```

`cfw_ws_on_event` is called from the server's `upgrade` hook. It drains
`cfw_upgraded_recv`, parses frames incrementally, auto-answers ping
with pong, handles the close handshake (echo close, then
`cfw_upgraded_close`), reassembles fragmented messages up to
`max_message_size`, validates UTF-8 on text messages, and invokes the
application's `cfw_ws_message_fn` once per complete message. Returning
an error from the message callback or a protocol violation closes the
connection (1011/1002/1007/1009 close codes as applicable).

Per-connection state (fragment buffer, parse state) lives inside the
heap-allocated `cfw_ws_t`; nothing is retained across calls except
bounded assembly state.

### 3. Send helpers

```c
cfw_status_t cfw_ws_send(cfw_ws_t *ws, cfw_ws_opcode_t opcode,
                         const void *data, size_t size);
cfw_status_t cfw_ws_send_text/binary/ping/pong/close(...);
```

`cfw_ws_send` always emits a complete message: it fragments
automatically into continuation frames when `size` exceeds
`max_frame_payload` (default 16 KiB) and pre-flights the whole message
against `cfw_upgraded_out_free` so a send either queues completely or
returns `CFW_ERROR_WOULDBLOCK` with nothing queued. Server→client
frames are never masked.

## Alternatives considered

- **Callback-per-frame instead of per-message:** rejected; apps almost
  always want whole messages, and reassembly is where the memory-safety
  risk lives — keeping it inside the module with a hard cap is safer
  than pushing it to every app.
- **Extensions (permessage-deflate, etc.):** rejected for v1; RFC 7692
  adds a compression context and a decompression-bomb surface. The
  negotiation path must reject unknown `Sec-WebSocket-Extensions`
  values (or the client must not offer them); v1 ignores the header and
  documents that no extensions are supported.
- **Subprotocol registry in the framework:** rejected; `Sec-WebSocket-
  Protocol` validation is application policy. The helper validates
  syntax only; selection is the app's.
- **TLS (`wss://`):** out of scope per ADR 0030; the module is
  transport-agnostic byte-stream code and would work unchanged under a
  future TLS module.
- **Built-in HTTP-route integration (e.g. `cfw_dispatch_ws_route`):**
  deferred; composition via existing handler + upgrade hook keeps the
  dispatch pipeline untouched.

## Consequences

- SHA-1 and base64 are implemented in-tree (~150 lines total) with
  known-answer tests; SHA-1's use is confined to the handshake accept
  key and documented as non-security.
- New audit surface per catalog item 3: frame parser state machine,
  length arithmetic (64-bit lengths, overflow), mask handling,
  fragmentation bounds, UTF-8 validation, and close/ping state
  confusion — all covered by a dedicated fuzz target and the upgrade
  audit.
- `max_message_size` (default 1 MiB) bounds reassembly memory;
  `max_frame_payload` bounds per-send scratch.
- No ABI impact: all additions are new files/types/functions.
- Follow-ups: fuzz corpus for WS frames, soak test with many
  concurrent upgraded connections, optional permessage-deflate ADR if
  ever wanted.
