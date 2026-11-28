<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0044: shared percent-decoding primitive

## Status

Accepted.

## Context

Percent-encoded bytes appear throughout HTTP request handling, and
each consumer had (or needed) its own decoder with subtly different
rules:

- `cfw_query_parse` (ADR 0042) decodes `+`→SP and `%XX`, streaming
  into scratch with no per-component terminator.
- `cfw_static_serve` percent-decodes the path suffix into a
  filesystem path, additionally rejecting NUL and backslash bytes
  (literal or decoded) for platform safety.
- Route parameters and `req.path` remain percent-encoded, leaving
  handlers without a supported way to decode them.

Three private copies of "scan for `%`, validate two hex digits,
write byte" is exactly the kind of duplication that drifts into
inconsistent escape handling — a security-relevant parser concern.

## Decision

New module `cfw/uri.h` + `src/cfw/uri.c` exposing one primitive:

```c
cfw_uri_decode(src, len, dst, cap, &out_len, flags)
```

- Strict RFC 3986 `%XX` decoding; output NUL-terminated, `cap` must
  cover decoded length + 1.
- Flags select per-consumer rules:
  `CFW_URI_DECODE_PLUS_AS_SPACE` (form components),
  `CFW_URI_DECODE_REJECT_NUL`,
  `CFW_URI_DECODE_REJECT_BACKSLASH` (filesystem paths — rejects the
  byte wherever it appears, literal or decoded).
- Unknown flag bits are rejected.

Consumers:

- `cfw_query_parse` now delegates to `cfw_uri_decode` with
  `PLUS_AS_SPACE`. Components are packed without terminators (each
  decode's terminator is overwritten by the next), so the scratch
  bound tightened from `query->size` to `query->size + 1`.
- `cfw_static_decode` is now a thin wrapper passing
  `REJECT_NUL | REJECT_BACKSLASH` — behavior identical, verified by
  the existing static test suite.
- New `cfw_route_param_decode` convenience looks up a route parameter
  and decodes it into caller storage with caller-chosen flags.
  Route matching itself is unchanged: patterns still match the raw
  encoded path, so `%2F` can never fabricate path segments.

## Alternatives considered

- **Decode the whole path before routing**: rejected — `%2F` would
  introduce phantom segments and change route-param boundaries;
  matching stays on the encoded path by design.
- **A flags-free decoder plus caller-side checks**: rejected — the
  NUL/backslash rejections are exactly the kind of rule callers
  forget; encoding them as flags keeps each call site explicit.
- **Decoded `req.path` field**: rejected — whole-path decoding has
  the `%2F` problem; per-segment/per-param decoding is the correct
  granularity. Handlers decode what they need.

## Consequences

- Positive: one audited decoder; consistent escape semantics;
  handlers gain decoded route-param access without new allocation.
- Negative: `cfw_query_parse` scratch requirement is `size + 1`
  (was `size`); decoded route params are not NUL-terminated unless
  the caller's buffer happens to be — the `out_len` contract makes
  this explicit.
- Compatibility: additive public API; `cfw_query_parse` bound change
  is documented in its header comment.

## Verification plan

- `tests/unit/test_uri.c`: basic/edge decoding, flag behavior,
  malformed escapes, capacity and argument validation, UTF-8
  passthrough.
- `test_route.c`: `cfw_route_param_decode` success, `+` literal,
  flag rejection, malformed escape, unknown param, small buffer.
- Existing `test_query.c`/`test_static.c` suites unchanged and
  green — regression coverage for the refactor.
- Sanitizers + shared presets; audit record per
  `docs/security/audit-catalog.md`.
