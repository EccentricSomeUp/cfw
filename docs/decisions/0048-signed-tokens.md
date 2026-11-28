<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0048: signed tokens (HMAC-authenticated envelopes)

## Status

Accepted.

## Context

ADR 0045 added cookie parsing/serialization and ADR 0046/0047 added
the primitives (HMAC-SHA256, base64) needed for integrity-protected
values. Signed cookies, CSRF tokens, and password-reset/bearer
tokens all need the same envelope: an opaque payload plus a
verifiable authenticator. Leaving the format to each application
invites MAC-then-encode mistakes (signing decoded vs encoded forms,
missing domain separation, non-constant-time compares).

## Decision

New module `cfw/signed.h` + `src/cfw/signed.c`:

- Wire format: `v1.` + base64(payload) + `.` + base64(HMAC-SHA256).
  Canonical RFC 4648 padded base64 throughout (ADR 0047), so the
  token is a valid cookie-octet value and header-safe string.
- The MAC input is the literal `v1.` + base64(payload) prefix —
  version-bound and encoding-exact, so verification needs no
  re-encoding and cannot disagree with encoding on canonical form.
- `cfw_signed_encode(key, key_size, payload, payload_size, out, cap,
  &written)` — allocation-free: the payload segment is base64-encoded
  directly into `out` after the prefix, the one-shot HMAC runs over
  that contiguous span, and the MAC segment is appended. Capacity via
  `cfw_signed_encode_bound(payload_size)` (key-independent).
- `cfw_signed_decode(key, key_size, token, payload_out,
  payload_cap, &payload_size)` — structural parse, MAC-segment
  decode, constant-time `cfw_secure_equal` comparison, and only then
  payload decode. MAC mismatch and malformed tokens return the same
  CFW_ERROR_INVALID_ARGUMENT.

### Properties and non-goals

- Integrity + authenticity only; payload is base64, not encrypted.
- No expiry/nonce/replay protection in the format — callers embed
  timestamps or nonces (via `cfw_random_bytes`) in the payload and
  enforce policy.
- Key rotation invalidates outstanding tokens; `v1.` prefix leaves
  room for algorithm/version agility.
- Empty payload and empty key are valid; binary payloads supported.

## Alternatives considered

- **Encrypt-then-MAC / AEAD (e.g. AES-GCM, ChaCha20-Poly1305)**:
  deferred — needs a new primitive and likely a dependency decision;
  confidentiality is not required for the target uses (session IDs,
  CSRF tokens are bearer secrets anyway, and opaque payloads are
  fine).
- **Hex-encoded MAC or truncated MAC**: rejected — base64 is denser
  and already canonical in this codebase; truncation weakens the MAC
  for no benefit at 44 chars.
- **Signing the decoded payload bytes**: rejected — requires
  decode-before-verify and creates encode/decode canonicalization
  hazards; signing the encoded form is unambiguous.
- **Putting this in cookie.h**: rejected — the envelope is useful for
  non-cookie tokens (CSRF, reset links); a generic module keeps
  cookie.h purely about header grammar.
- **Streaming HMAC for encode**: unnecessary — the allocation-free
  two-phase layout (encode payload segment, MAC the contiguous
  prefix, append MAC) avoids it entirely.

## Consequences

- Positive: one reviewed, constant-time, allocation-free signed-value
  format ready for signed cookies (`cfw_cookie_serialize` accepts the
  token as a cookie-octet value), CSRF, and reset tokens.
- Negative: tokens are ~88 chars of overhead plus payload; large
  payloads make large cookies (callers should keep payloads small).
- Compatibility: additive module; no existing behavior changes.

## Verification plan

- Unit tests: round-trips (text, empty, binary payloads), exhaustive
  single-character tamper rejection, wrong-key/truncated/garbage/
  bad-version rejection, encode bound exactness, decode capacity,
  NULL/empty-key argument matrix, cookie-octet charset conformance.
- Sanitizers + debug/shared presets; audit per
  `docs/security/audit-catalog.md` §8.
