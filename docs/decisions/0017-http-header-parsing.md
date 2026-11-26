<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0017: HTTP header block parsing

- Status: Accepted
- Date: 2026-11-26

## Context

The request parser needs to iterate `field-name: field-value` lines without
copying, while rejecting malformed input that enables request smuggling or
parser differentials.

## Decision

Add `cfw_http_header_t { name, value }` (borrowed `cfw_strview_t`s) and
`cfw_http_headers_next(block, cursor, header, done)` to the HTTP module:

- Input is a header block: a sequence of lines each terminated by CRLF.
  An empty line or end of block reports `done`.
- Each line must contain `:`; the name must be a nonempty RFC 9110 `token`
  (tchar set only — no whitespace or separators).
- The value has optional whitespace (SP/HTAB) stripped from both ends;
  interior whitespace and empty values are preserved.
- Bare CR or LF inside the block, unterminated final lines, empty names,
  and obs-fold (lines beginning with SP/HT) all return
  `CFW_ERROR_INVALID_ARGUMENT`.
- Views into the caller's buffer; no allocation, no NUL termination.

## Alternatives considered

- **Accepting obs-fold:** rejected; folded headers are a classic
  smuggling/desync vector and RFC 9110 deprecates them.
- **Lenient whitespace before `:`:** rejected; `Name :` is invalid per the
  grammar and tolerating it creates parser differentials.
- **Copying headers into owned storage:** deferred; borrowed views suffice
  until routing needs persistent header storage, which will get its own
  allocation-policy decision.
- **Case-normalizing names during parse:** rejected; normalization belongs
  to lookup, not parsing.

## Consequences

- Header iteration is allocation-free and bounds-safe by construction.
- Strict grammar reduces request-smuggling surface; remaining risks
  (duplicate headers, transfer-encoding conflicts) belong to the message
  parser layer.
- Parser audit (catalog 3) applies: all malformed-input classes have
  dedicated tests.
