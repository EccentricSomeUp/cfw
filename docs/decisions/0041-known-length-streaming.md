<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0041: Known-length streaming responses

## Status

Proposed.

## Context

ADR 0031 introduced pull-style streaming response bodies. Until now
every stream was framed as chunked transfer-encoding (HTTP/1.1) or
close-delimited (older versions), even when the producer knew the
exact byte count up front — the static file middleware (ADRs
0036–0040) being the canonical case. Consequences:

- Clients cannot show progress or pre-allocate for static downloads.
- HTTP/1.0 static responses are close-delimited, forcing connection
  close and losing keep-alive.
- HEAD responses for streams reported `Content-Length: 0` because the
  formatter only knew `res->body.size`, which is empty for streams.
- Chunk framing adds per-piece overhead to every streamed response.

## Decision

### Contract (`cfw_response_t.stream_size`)

A new field `stream_size` (appended last per `docs/api-stability.md`)
declares the total body length in bytes. `CFW_STREAM_SIZE_UNKNOWN`
(`UINT64_MAX`, the dispatch-reset default) preserves the existing
behavior exactly.

When set, the semantics are Content-Length semantics:

- The server emits `Content-Length: <stream_size>` and raw identity
  bytes on **every** HTTP version — no `Transfer-Encoding` header.
- The server pulls exactly `stream_size` bytes; the stream need not
  signal eof. Once the declared length is emitted the response
  completes, `stream_end` still runs exactly once, and keep-alive is
  preserved on all versions (HTTP/1.0 gets `Connection: keep-alive`).
- Source bytes beyond the declared length are never emitted (the
  extra tail of a straddling chunk is discarded).
- A stream that signals eof before `stream_size` bytes have been
  emitted closes the connection — a short body would corrupt framing,
  and there is no recovery.
- For HEAD requests (which never install the stream) `stream_size`
  still determines the `Content-Length` header, fixing the previous
  `Content-Length: 0`.
- `stream_size` is ignored for non-streaming responses on body-carrying
  methods, so a stray value cannot corrupt framing.

### Server enforcement (`src/cfw/server.c`)

The connection tracks `stream_size`/`stream_sent`. The pump:

1. Treats `stream_sent == stream_size` as stream completion (sets the
   eof path) without pulling the stream again.
2. Caps each emitted piece to the remaining declared bytes.
3. On real eof with `stream_sent != stream_size`, marks the
   connection dead.
4. Ends known-length streams by restoring `close_after_write` (not
   forcing close), unlike close-delimited streams.

`will_close` now distinguishes three framings: chunked, close-delimited
(unknown size, pre-1.1), and known-length identity.

### Head formatting (`src/cfw/dispatch.c`)

`cfw_dispatch_response_head_format` emits `stream_size` as the
auto `Content-Length` when a stream is installed or the method is
HEAD; otherwise `body.size` as before.

### Static middleware

`cfw_static_serve` sets `stream_size` to the file size (200) or the
range length (206), for GET (stream installed) and HEAD (header only).

## Alternatives considered

- **Caller-supplied `Content-Length` header honored for streams**:
  rejected — the server strips handler framing headers by design
  (ADR 0027); an explicit typed field is unambiguous and cannot
  conflict with `Transfer-Encoding`.
- **Strict contract (stream must yield exactly N then eof)**:
  rejected — a chunk straddling the boundary is a natural occurrence;
  truncation matches Content-Length semantics and is what sendfile-
  style servers do.
- **Per-request buffering to verify length**: rejected — defeats
  streaming; early-eof close is the correct failure mode.

## Consequences

- Positive: static files (and any sized stream) get correct
  `Content-Length` on all HTTP versions, working keep-alive on
  HTTP/1.0, contiguous bodies without chunk overhead, accurate HEAD
  lengths, and client-visible progress.
- Negative: `cfw_response_t` grew by one field (additive, end-appended;
  permitted pre-1.0 per api-stability). Streams that under-produce now
  close the connection — previously they would have been framed
  chunked/close-delimited and the shortfall was invisible until the
  client noticed; the new behavior is strictly more correct.
- Compatibility: `CFW_STREAM_SIZE_UNKNOWN` is the default; existing
  streams are byte-for-byte unchanged on the wire.

## Verification plan

- Unit tests: `stream_size` on static 200/206/HEAD; server-level
  known-length test covering CL emission, absent TE, contiguous body,
  keep-alive, over-long truncation, and short-stream close.
- Sanitizers + shared presets; audit record per
  `docs/security/audit-catalog.md` (protocol/memory-safety focus).
