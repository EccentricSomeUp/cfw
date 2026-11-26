<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0018: HTTP request-line parsing

- Status: Accepted
- Date: 2026-11-26

## Context

The request head begins with `request-line = method SP request-target SP
HTTP-version`. Parsing it strictly is the first line of defense against
request smuggling and parser differentials.

## Decision

Add `cfw_http_request_line_t { method, target, version }` and
`cfw_http_request_line_parse(line, out)`:

- Input is a single line with no trailing CRLF (the caller splits lines).
- Exactly two single-space separators are required; consecutive, leading,
  trailing, or additional spaces fail.
- Every byte outside the separators must be > 0x20 and != 0x7F, rejecting
  all control characters including bare tab.
- Method must parse to a known `cfw_http_method_t`; unknown methods are a
  parse error (`CFW_ERROR_INVALID_ARGUMENT`) so the server returns 400.
- The target is nonempty and may use origin, absolute, authority, or
  asterisk form; high bytes (UTF-8/obs-text) are permitted pending
  form-specific validation in routing.
- Version uses the strict `HTTP/D.D` grammar from ADR 0016.

## Alternatives considered

- **Lenient whitespace handling:** rejected; ambiguity here propagates to
  downstream differentials.
- **Request-target grammar validation at this layer:** deferred; form
  selection (origin vs absolute vs authority) is a routing concern, and
  over-strict parsing here would reject legal forms.
- **Accepting unknown methods:** rejected for the request path; UNKNOWN
  remains available for client-side parsing where 501 handling differs.

## Consequences

- Malformed request lines produce a single, well-defined failure that the
  server maps to `400 Bad Request`.
- Remaining smuggling surface moves to header conflicts
  (`Content-Length` vs `Transfer-Encoding`) handled by the message parser.
