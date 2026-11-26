<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0019: HTTP request-head composition

- Status: Accepted
- Date: 2026-11-26

## Context

ADR 0017 and ADR 0018 define strict header-block and request-line parsers.
The server still needs a single entry point that validates a complete
request head — request line, header lines, and the mandatory empty-line
terminator — and rejects any bytes after it.

## Decision

Add `cfw_http_request_head_t` and `cfw_http_request_head_parse(head, out)`:

- Input is exactly the head bytes: `request-line CRLF *(header CRLF) CRLF`.
  A bare LF anywhere in the head is a parse error.
- The first CRLF ends the request line and is parsed by
  `cfw_http_request_line_parse` (ADR 0018).
- The remainder is walked with `cfw_http_headers_next` (ADR 0017); the
  iterator's stop at the empty line must leave exactly the final CRLF —
  anything after it (a second header block, garbage) is a parse error.
- `out->headers` retains the header section including its terminator so
  callers can re-iterate without reparsing.
- All failures return `CFW_ERROR_INVALID_ARGUMENT`; the server maps them
  to `400 Bad Request`.

## Alternatives considered

- **Returning a cursor/offset for pipelined bytes:** deferred to the
  connection layer, which owns buffering; the parser stays pure.
- **Lenient LF-only acceptance:** rejected; CRLF is required by RFC 9112
  and leniency enables request-smuggling differentials.

## Consequences

- One validated entry point covers the entire head; framing
  (`Content-Length` / `Transfer-Encoding`) and body handling remain the
  next Phase 3 items.
