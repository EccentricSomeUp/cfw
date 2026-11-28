<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0045: cookie parsing and serialization

## Status

Accepted.

## Context

Session and preference cookies are table stakes for a web framework,
but the Cookie header and Set-Cookie attribute grammar (RFC 6265,
RFC 9110 §5.6) has enough sharp edges — token vs cookie-octet
character sets, DQUOTE wrapping, attribute ordering, Max-Age vs
Expires precedence — that hand-rolled handling reliably produces
header-injection or parsing bugs.

## Decision

New module `cfw/cookie.h` + `src/cfw/cookie.c`:

- `cfw_cookie_parse(header, pairs, cap, &count, &dropped)` splits a
  Cookie header value into zero-copy name/value strviews. Grammar:
  `cookie-pair *( ";" cookie-pair )` with OWS tolerated around
  segments; empty segments skipped; first '=' splits (later '=' is
  literal value data); surrounding DQUOTEs stripped from the value
  view. Names must be nonempty RFC 9110 tokens; values must be
  cookie-octets. Any malformed segment fails the whole parse with
  CFW_ERROR_INVALID_ARGUMENT.
- `cfw_cookie_serialize(cookie, dst, cap, &written)` renders a
  Set-Cookie value: `name=value` plus optional `Path`, `Domain`,
  `Max-Age`, `Expires` (via `cfw_http_date_format`), `Secure`,
  `HttpOnly`, `Partitioned`, `SameSite=Lax|Strict|None`. Negative
  max_age/expires and empty views omit the attribute.

### Character rules enforced

- Parse: name ∈ token; value ∈ cookie-octet (visible ASCII minus
  DQUOTE/comma/semicolon/backslash), DQUOTE-wrapped form accepted.
  Values are NOT percent-decoded — cookies do not use URI encoding.
- Serialize: same classes; values that would need quoting are
  rejected rather than silently mangled (callers sanitize untrusted
  input first, e.g. via `cfw_uri_decode`).

## Alternatives considered

- **Percent-decoding cookie values**: rejected — RFC 6265 does not
  define it; decoding would corrupt values containing literal '%'.
- **Lenient skip-bad-pair parsing**: rejected — silently dropping a
  malformed pair hides client bugs and can mask injection attempts;
  strict failure matches the query/multipart modules' posture.
- **Automatic Set-Cookie quoting**: rejected — quoting rules differ
  across attributes; explicit validation keeps output predictable.
- **Cookie attributes struct with bitflags for everything**: rejected
  — empty-view/negative-sentinel omission is simpler and matches the
  codebase's optional-field style.

## Consequences

- Positive: one audited implementation of both directions; strict
  character validation prevents response/header injection through
  cookie values.
- Negative: `Partitioned`/`SameSite=None` semantics are the
  caller's responsibility (e.g. Secure requirement for None); the
  module serializes, it does not decide policy.
- Compatibility: purely additive module; no existing behavior
  changes.

## Verification plan

- Unit tests: basic/multi/empty/quoted parsing, OWS tolerance,
  capacity/drop, malformed inputs (missing '=', empty name, bad
  chars, unterminated quote, non-ASCII), argument validation;
  serialization of all attributes, SameSite variants, validation
  rejections, undersized buffer, round-trip.
- Sanitizers + shared presets; audit record per
  `docs/security/audit-catalog.md`.
