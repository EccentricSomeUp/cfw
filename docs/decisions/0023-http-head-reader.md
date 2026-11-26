<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0023: Incremental HTTP head reader

- Status: Accepted
- Date: 2026-11-26

## Context

`cfw_http_request_head_parse` (ADR 0019) requires the complete head in one
contiguous view, but transport delivers bytes fragmented across reads.
The connection layer needs a bounded accumulator that finds the
`\r\n\r\n` boundary without retaining body or pipelined bytes.

## Decision

Add `cfw_http_head_reader_t` (embeds `cfw_buffer_t`):

- `cfw_http_head_reader_init(reader, max_size)` configures the head size
  limit; `feed(input, &consumed)` appends bytes and reports exactly how
  many input bytes belong to the head.
- The terminator scan resumes from `old_size - 3`, so fragmented feeds
  cost O(new bytes) rather than rescanning the whole head.
- Appends are capped at `max_size`: a single oversized feed cannot force
  an allocation beyond the limit, and a head that reaches `max_size`
  without a terminator can never complete (next feed fails).
- On completion, bytes past the terminator are dropped from the buffer
  and reported as unconsumed — the caller keeps them for body or
  pipelined-message handling.
- `cfw_http_head_reader_head` exposes the head bytes for
  `cfw_http_request_head_parse`; the view lives until the next feed/free.

## Alternatives considered

- **Caller-side accumulation:** rejected; every connection would
  reimplement boundary detection and limit enforcement.
- **Retaining post-head bytes in the reader:** rejected; body framing
  (Content-Length vs chunked) is the connection layer's concern, and
  retaining them here would couple the reader to message semantics.

## Consequences

- Per-connection head memory is bounded by `max_size` plus one read.
- The reader is transport-agnostic: it consumes `cfw_span_t` chunks from
  any source (socket, TLS record layer, test harness).
