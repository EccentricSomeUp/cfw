<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0021: Incremental chunked-transfer decoder

- Status: Accepted
- Date: 2026-11-26

## Context

`CFW_HTTP_BODY_CHUNKED` framing (ADR 0020) needs a decoder that works on
streaming socket input: chunk boundaries and CRLFs routinely straddle
read buffer edges, and decoded output must not require copying.

## Decision

Add `cfw_http_chunked_decoder_t`, a resumable state machine
(`SIZE → DATA → DATA_CRLF → SIZE … → TRAILER → DONE`) driven by
`cfw_http_chunked_decode(decoder, input, output, consumed, finished)`:

- **Zero-copy output:** each call returns at most one contiguous span of
  decoded body bytes viewed directly from the input. When a second chunk's
  data would begin in the same call, decoding pauses at the chunk boundary
  and the caller re-invokes with the remainder — framing bytes consumed
  along the way are not re-emitted.
- **Bounded line state:** chunk-size and trailer lines accumulate in a
  fixed `CFW_HTTP_CHUNKED_MAX_LINE` (4096) buffer; longer lines are a
  parse error, bounding memory per connection.
- **Strict grammar:** chunk sizes are hex with optional `;extensions`
  (printable ASCII only, nonempty); data must be followed by CRLF (a split
  CR is remembered via `cr_pending`); trailer fields reuse the header
  field grammar; bare LF anywhere is rejected.
- **Pipelining-safe:** decoding stops exactly after the terminal CRLF;
  `consumed` delimits body bytes from the next pipelined message.
- Header/trailer field values now reject CTLs other than HTAB (bare CR
  included), closing a smuggling-adjacent gap in field parsing.

## Alternatives considered

- **Copying decoder into a caller buffer:** rejected; doubles memory
  traffic on the hot path for no benefit.
- **Unbounded line buffering:** rejected; a single `\r`-less line would
  exhaust memory (slowloris vector).
- **Ignoring chunk extensions:** rejected; accepting-and-validating keeps
  RFC-permitted peers working while bounding the surface.

## Consequences

- Callers loop `decode` until `consumed == input.size` or `finished`;
  each iteration yields ≤1 body span, so streaming to the application is
  natural.
- Decoder state is ~4 KiB per connection (line buffer); acceptable and
  documented for capacity planning.
