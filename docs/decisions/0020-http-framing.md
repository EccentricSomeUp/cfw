<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0020: HTTP request body framing and smuggling resistance

- Status: Accepted
- Date: 2026-11-26

## Context

After the request head is parsed, the server must decide how many body
bytes belong to the request. Ambiguous framing — duplicate or conflicting
`Content-Length`, `Transfer-Encoding` combined with `Content-Length`, or
unsupported transfer codings — is the primary HTTP request-smuggling
vector. RFC 9112 leaves several of these cases to implementation policy;
this project chooses the strictest consistent policy.

## Decision

Add `cfw_http_request_framing(head, out)` returning
`CFW_HTTP_BODY_NONE`, `CFW_HTTP_BODY_CONTENT_LENGTH`, or
`CFW_HTTP_BODY_CHUNKED`:

- `Content-Length` must be `1*DIGIT`, parsed with overflow checking into
  `uint64_t`. Multiple occurrences are permitted only when all values are
  numerically identical (so `7` and `007` coalesce); any conflict rejects.
- `Transfer-Encoding` may appear at most once, and its value must be
  exactly `chunked` (ASCII case-insensitive). Coding lists such as
  `gzip, chunked` are rejected: only the identity-free chunked coding is
  supported, so no intermediate transfer codings exist to disagree about.
- `Content-Length` and `Transfer-Encoding` in the same head is always
  rejected, even though RFC 9112 permits TE to override CL. The override
  rule is itself a smuggling differential between implementations.
- Every violation returns `CFW_ERROR_INVALID_ARGUMENT` → `400 Bad Request`.

## Alternatives considered

- **RFC-lenient TE-over-CL precedence:** rejected; front/back-end
  disagreement on precedence is exactly how smuggling works.
- **Accepting coding lists ending in chunked:** deferred until
  transfer-coding support exists; today it can only create ambiguity.
- **Silently ignoring unknown TE values:** rejected; fail closed.

## Consequences

- Framing is a pure function of the parsed head; connection-level code
  consumes `content_length` or switches to the chunked decoder.
- Requests that RFC-permitted peers might accept are rejected; this is an
  intentional interoperability trade for smuggling resistance.
