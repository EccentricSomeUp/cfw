<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0022: HTTP response serialization primitives

- Status: Accepted
- Date: 2026-11-26

## Context

Phase 3 requires an HTTP/1.1 serializer alongside the request parser. The
server must emit status lines, header fields, and complete response heads
without dynamic allocation on the hot path, and must not emit responses
that protocol rules forbid (e.g. bodies on HEAD or 204).

## Decision

Add caller-buffered format functions to `cfw/http.h`:

- `cfw_http_status_line_format` — `HTTP/D.D NNN Reason-Phrase\r\n`;
  rejects unknown status codes and versions with components > 9.
- `cfw_http_header_line_format` — `Name: value\r\n`; validates the name as
  an HTTP token and the value against the field-value grammar (VCHAR,
  obs-text, SP, HTAB; no other CTLs), sharing the parser's validation via
  `cfw_http_field_value_valid`.
- `cfw_http_response_head_format` — composes status line + header array +
  terminating CRLF into one buffer.
- `cfw_http_response_has_body` — implements RFC 9110 §6.4.1/§9.3.2:
  no body for HEAD, 1xx, 204, 304.

All format functions return `CFW_ERROR_INVALID_ARGUMENT` on insufficient
capacity or invalid input, consistent with `cfw_http_version_format`.
Serialization is deliberately dumb: it does not inject `Content-Length`,
`Date`, or `Server` headers — policy belongs to the response pipeline
(Phase 4).

## Alternatives considered

- **Builder/streaming writer object:** deferred; fixed-buffer formatting
  covers the common case and keeps the API surface small. A streaming
  writer can layer on later without ABI breakage.
- **Auto-adding framing headers:** rejected; implicit headers hide policy
  and complicate HEAD/204/304 handling.

## Consequences

- Symmetric strictness: what the parser accepts, the serializer emits.
- Response pipeline code can pre-size buffers or retry with larger ones.
