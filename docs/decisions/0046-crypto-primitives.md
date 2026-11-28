<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0046: cryptographic primitives (SHA-256, HMAC-SHA256)

## Status

Accepted.

## Context

Signed cookies, bearer tokens, webhook signatures, and integrity tags
are the next consumers of the HTTP stack (cookie parsing landed in
ADR 0045), but the project has no reviewed cryptographic primitive to
build them on. TLS itself remains out of scope (ADR 0030), so the
crypto surface is limited to small, self-contained primitives that
applications compose into higher-level schemes.

Constraints from `AGENTS.md`: C11 only, zero third-party runtime
dependencies, `cfw_` API prefix, additive-only public API.

## Decision

New module `cfw/crypto.h` + `src/cfw/crypto.c`:

- `cfw_sha256_init` / `cfw_sha256_update` / `cfw_sha256_final`:
  streaming SHA-256 (FIPS 180-4) over caller storage. The context is a
  public struct (`cfw_sha256_ctx_t`) so callers can embed or reuse it;
  `total_len` counts message bits mod 2^64 per spec.
- `cfw_sha256(data, size, out)`: one-shot convenience wrapper.
- `cfw_hmac_sha256(key, key_size, data, data_size, out)`: HMAC-SHA256
  per RFC 2104 with the standard 64-byte block size; keys longer than
  the block size are hashed first. Empty key and empty message are
  valid.
- `cfw_secure_equal(a, b, size)`: constant-time byte comparison that
  does not early-exit, for MAC/tag verification. Callers compare
  lengths separately.

Explicitly out of scope: password hashing (memory-hard KDFs such as
Argon2id/scrypt/bcrypt are a separate, dependency-heavy decision),
key derivation, AEAD encryption, RNG (the `cfw/random.h` boundary
already exists), and TLS.

### Hardening properties

- No heap allocation; all state lives in the caller-provided context
  or fixed-size stack buffers.
- `cfw_secure_equal` iterates the full `size` regardless of content.
- NULL-pointer contracts mirror the rest of the codebase: NULL with
  nonzero size is documented UB/caller error; NULL with size 0 is
  valid.
- The compression function uses only `uint32_t` arithmetic — defined
  behavior on every C11 platform, no reliance on `unsigned int` width.

## Alternatives considered

- **Third-party crypto library (OpenSSL, libsodium, mbedTLS)**:
  rejected for the core — violates the dependency-free constraint;
  would require an approved ADR and re-opens supply-chain review.
  Applications needing TLS/KDFs can link their own crypto and still
  use this module for MACs.
- **SHA-1 / SHA-512 / other algorithms**: rejected — SHA-256 covers
  the needed security level; the existing private SHA-1 in `ws.c` is
  a protocol transform (RFC 6455), not a security boundary. Additional
  algorithms can be added additively if a consumer needs them.
- **Streaming HMAC API**: deferred — one-shot HMAC covers current
  needs (cookies, webhook signatures); a streaming variant can be
  added later without breaking the one-shot form.
- **Hex/base64 encoding helpers in this module**: deferred — digest
  encoding is application formatting; the ws module's private base64
  can be promoted later if a second consumer appears.

## Consequences

- Positive: reviewed, KAT-verified primitives unblock signed-cookie,
  token, and webhook-signature work without new dependencies.
- Negative: applications needing password hashing or AEAD must bring
  their own dependency; this module does not provide them.
- Compatibility: purely additive; no existing behavior changes.
- Follow-up: deduplicate `ws.c`'s private SHA-1/base64 if a shared
  base64 helper is ever added; consider a streaming HMAC if a
  consumer needs it.

## Verification plan

- Unit tests: FIPS 180-4 SHA-256 vectors (empty, "abc", 56-byte
  example, million-'a'), streaming-vs-one-shot equivalence, padding
  boundary sizes, RFC 4231 HMAC-SHA256 cases 1–3, long-key (>64 B)
  hashing, empty key/message, `cfw_secure_equal` semantics.
- Sanitizers + debug/shared presets; cryptography audit per
  `docs/security/audit-catalog.md` §8.
