<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0043: multipart/form-data parsing

## Status

Accepted.

## Context

File uploads are the most common remaining gap in the HTTP feature
set: `cfw_request_t.body` delivers the raw entity body, and handlers
had no supported way to split `multipart/form-data` into fields and
files. Hand-rolled multipart parsing is a classic source of bugs
(boundary confusion, header injection via part headers, unbounded
allocation), so the rules belong in one tested module.

Constraints:

- No allocation; parsed values are views into the request body plus
  caller scratch for unescaping.
- Part headers must satisfy the same grammar as request headers —
  reuse `cfw_http_headers_next` rather than a second parser.
- The boundary delimiter must not match inside part bodies: the
  delimiter is CRLF + "--" + boundary, and a boundary that is a
  prefix of a longer token is not a delimiter.
- Buffered bodies only: upload-streamed bodies (ADR 0033) are not
  visible to `cfw_request_t.body`; incremental multipart over streams
  is out of scope.

## Decision

New module `cfw/multipart.h` + `src/cfw/multipart.c`:

- `cfw_multipart_boundary(content_type, &boundary)` extracts the
  `boundary` parameter (token or simple quoted-string, case-
  insensitive name, ≤70 octets per RFC 2046). Quoted values containing
  escapes are rejected — real boundaries never need them.
- `cfw_multipart_parse(body, boundary, parts, capacity, scratch,
  scratch_size, &count, &dropped)` splits the body into
  `cfw_multipart_part_t` records: `name`/`filename` from
  Content-Disposition (quoted-string unescaped into scratch),
  `content_type`, raw `header_block`, and zero-copy `body` span.

### Grammar enforced

- Delimiter = "--" + boundary; the first may appear anywhere (preamble
  skipped), subsequent delimiters require a preceding CRLF.
- After each boundary: transport padding (SP/HTAB), then either "--"
  (close) or CRLF (next part). Epilogue after close is tolerated to
  end-of-body only — trailing junk after the close CRLF is ignored.
- Header block ends at CRLF CRLF (or is empty); each line is validated
  by `cfw_http_headers_next` — malformed part headers fail the parse.
- A candidate delimiter whose trailer is not a valid continuation is
  treated as body content and the search resumes — boundary-prefix
  collisions inside bodies are handled correctly.
- Missing close delimiter, missing boundary, or truncated structure →
  CFW_ERROR_INVALID_ARGUMENT (handler maps to 400).
- `scratch_size >= body->size` is the sufficient bound (unescaped
  parameters never exceed their source bytes); smaller fails up
  front.

## Alternatives considered

- **Streaming/incremental multipart parser**: rejected for v1 —
  buffered bodies already cover typical form uploads; streaming
  multipart needs a state machine across chunk boundaries and can be
  added later without changing this API.
- **Storing all part headers**: rejected — `header_block` exposes the
  raw bytes for applications that need other headers; the parser
  extracts only Content-Type and Content-Disposition.
- **Lenient boundary handling (no trailer validation)**: rejected —
  prefix collisions would silently truncate part bodies.
- **`Content-Transfer-Encoding` decoding (base64/quoted-printable)**:
  rejected — obsolete for HTTP multipart (RFC 7578 deprecates it);
  part bodies are returned verbatim.

## Consequences

- Positive: file-upload handlers get correct, bounded, zero-copy
  multipart parsing with strict delimiter semantics.
- Negative: large uploads still need the upload-streaming hooks
  (ADR 0033) plus application-side incremental parsing; scratch
  requirement is `body->size` worst case.
- Compatibility: purely additive module; no existing behavior changes.

## Verification plan

- Unit tests: boundary extraction (token/quoted/case/malformed),
  two-part parse, preamble/epilogue/padding, quoted-string escapes,
  headerless parts, empty bodies, boundary-prefix collision,
  malformed inputs, capacity/drop, argument validation.
- Sanitizers + shared presets; audit record per
  `docs/security/audit-catalog.md` (parser focus).
