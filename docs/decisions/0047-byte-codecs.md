<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0047: byte-to-text codecs (base64, hex)

## Status

Proposed.

## Context

ADR 0046 added SHA-256/HMAC for MACs, but digests and tokens must be
serialized into text for cookies, headers, and URLs. `ws.c` already
carried a private base64 implementation for the RFC 6455 handshake;
signed cookies, bearer tokens, and webhook signatures (the stated
consumers of crypto.h) need shared, reviewed encoders. Duplicating
base64/hex per module would repeat the same audit surface.

## Decision

New module `cfw/codec.h` + `src/cfw/codec.c`:

- `cfw_base64_encode` / `cfw_base64_decode`: canonical RFC 4648
  section 4 base64 with '=' padding. Encode is NUL-terminated with
  `written` excluding the terminator; `cfw_base64_encode_bound`
  returns the required capacity (encoded length + NUL, SIZE_MAX on
  overflow). Decode is strict: input length must be a multiple of 4,
  '=' only as the trailing one or two chars of the final group, and
  pad bits must be zero (canonical form). Errors:
  CFW_ERROR_INVALID_ARGUMENT for malformed input, CFW_ERROR_OVERFLOW
  for undersized output.
- `cfw_hex_encode` / `cfw_hex_decode`: lowercase hex output;
  decode accepts either case and requires an even digit count.
  `cfw_hex_encode_bound` mirrors the base64 bound helper.

`ws.c` is refactored to use `cfw_base64_encode`/`cfw_base64_decode`
in place of its private copies.

### Scope notes

- Standard (not URL-safe) alphabet only: '+' '/' are legal
  cookie-octets and header values; URL-safe base64url can be added
  additively if a consumer (e.g. JWT-style tokens) needs it.
- Padding is mandatory on decode; unpadded variants can be added
  later without breaking the canonical decoder.
- No streaming/incremental API — inputs are bounded by callers.

## Alternatives considered

- **Keep base64 private per module**: rejected — three anticipated
  consumers (ws handshake, signed cookies, tokens); one audited
  implementation is cheaper to verify.
- **URL-safe alphabet now**: deferred — no current consumer requires
  it; standard base64 is valid in Cookie and Authorization headers.
  Adding it later is additive.
- **Lenient decode (ignore whitespace, optional padding)**: rejected —
  canonical-only decoding prevents parser differentials between
  components that might re-encode or compare encoded forms.
- **Hex-only**: rejected — base64 is denser (needed for the ws
  handshake regardless) and already proven in-tree.

## Consequences

- Positive: one audited codec for all byte-to-text needs; ws.c loses
  ~110 lines of private crypto-adjacent code.
- Negative: `cfw_ws_handshake_validate` is now stricter — base64 keys
  with non-zero pad bits (non-canonical encodings of 16 bytes) are
  rejected where the private decoder previously accepted them.
  RFC-compliant clients always send canonical base64, so this is a
  tightening, not a regression; recorded in the codec audit.
- Compatibility: additive module; ws behavior change is limited to
  rejecting malformed/non-canonical keys (documented above).

## Verification plan

- Unit tests: RFC 4648 vectors, encode bounds/capacity/NULL args,
  decode malformed inputs (bad length, misplaced '=', nonzero pad
  bits, whitespace, truncated tails), decode capacity, hex
  round-trip/mixed-case/malformed/capacity.
- Existing ws handshake tests must still pass (they use canonical
  RFC 6455 vectors).
- Sanitizers + debug/shared presets; audit per
  `docs/security/audit-catalog.md`.
