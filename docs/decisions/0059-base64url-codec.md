<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0059: URL-safe base64url codec variant

- Status: Accepted
- Date: 2026-11-28

## Context

ADR 0047 shipped canonical RFC 4648 base64 (standard alphabet,
mandatory padding) and explicitly deferred a URL-safe variant "if a
consumer (e.g. JWT-style tokens) needs it". Tokens that travel in
URLs, query strings, or cookie values benefit from the section 5
alphabet where `-`/`_` replace `+`/`/` and padding is typically
omitted.

## Decision

Add `cfw_base64url_encode`/`cfw_base64url_decode`/
`cfw_base64url_encode_bound` to `cfw/codec.h`:

- Encoder emits the URL-safe alphabet with **no padding** — the
  dominant convention for tokens (JWT, PASETO-style encodings).
- Decoder accepts both the unpadded canonical form and `=`-padded
  input whose total length is a multiple of 4, enforcing canonical
  pad-bit zeroing in both forms. Mixed or non-canonical padding is
  rejected with `CFW_ERROR_INVALID_ARGUMENT`, matching the strict
  posture of the existing decoder.
- The encoder shares one internal implementation with
  `cfw_base64_encode` parameterized by alphabet and padding, so the
  canonical path is unchanged and both variants are covered by the
  same audit surface.

## Alternatives considered

- **Flags parameter on existing functions**: rejected — C has no
  default arguments, and a flags word on every call is worse API
  ergonomics than clearly named variants.
- **Padding-emitting encoder**: rejected — unpadded is what URL
  token consumers actually need; padded output remains decodable
  for interop.
- **Do nothing**: rejected — every consumer would otherwise
  hand-roll the alphabet swap, reintroducing the un-audited codec
  duplication ADR 0047 eliminated.

## Consequences

- Purely additive API; no ABI or behavioral change to existing
  functions.
- `signed.h` token envelopes and future JWT-style work can adopt
  base64url without new dependencies.
