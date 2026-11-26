<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: signed tokens (ADR 0048)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Scope: `cfw_signed_encode`/`cfw_signed_decode`/`cfw_signed_encode_bound`
  (`include/cfw/signed.h`, `src/cfw/signed.c`), tests in
  `tests/unit/test_signed.c`
- Method: catalog-driven review per `docs/security/audit-catalog.md`
  §8; tamper matrix; ASan/UBSan preset

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; encode capacity precomputed and checked before writes; decode bounds every segment by the token span; payload decode delegates cap checks to `cfw_base64_decode` (per-group `o + group_bytes > cap`) |
| 2 | Integer/bounds | Pass | `encode_bound` guards SIZE_MAX via base64 bound + explicit subtraction guard; separator scan bounded by token size; `mac_b64_len` computed as `size - sep - 1` only after `sep < size` is established; minimum-size check precedes all indexing |
| 3 | Parser/protocol state machine | Pass | Strict grammar: literal `v1.` prefix, first '.' after prefix is the separator (encoded segments cannot contain '.'), MAC segment must be exactly 44 canonical chars decoding to 32 bytes; payload segment validated by canonical base64 decoder |
| 4 | API misuse | Pass | NULL contracts documented and enforced; empty key/payload valid; `payload_out` NULL permitted only for empty payloads; `written`/`payload_size` optional |
| 5 | Concurrency/cancellation | N/A | Pure functions over caller storage; no globals |
| 6 | DoS resistance | Pass | O(token length) parse + O(payload) codec work; no allocation; MAC compared in constant time; attacker-controlled length bounded by caller's token buffer |
| 7 | Platform portability | Pass | Byte operations and `cfw_*` primitives only; C11 |
| 8 | Cryptography/transport | Pass with notes | HMAC-SHA256 per RFC 2104 over version-bound encoded prefix; `cfw_secure_equal` for MAC comparison; format provides integrity only — header documents no-confidentiality, no-expiry, key-rotation semantics; payload is never interpreted |
| 9 | Supply chain/license | Pass | No new dependencies; composes ADR 0046/0047 primitives |
| 10 | Documentation/ABI | Pass | ADR 0048 records format, MAC-input choice, non-goals, and rejected alternatives (AEAD, hex MAC, decoded-payload signing); additive API only |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| S-301 | Info | MAC mismatch and malformed token share CFW_ERROR_INVALID_ARGUMENT | Intended; avoids an oracle distinguishing "bad format" from "bad signature" |
| S-302 | Info | Payload is not confidential (base64 only) | Documented non-goal; callers must not embed secrets; AEAD deferred pending dependency ADR |
| S-303 | Info | No expiry/nonce/replay protection in format | Documented; callers embed timestamps/nonces (cfw_random_bytes) — policy stays app-side |
| S-304 | Info | Decode verifies MAC before decoding payload | Intended ordering; malformed-payload tokens with valid MACs are only producible by the key holder, and payload decode still validates canonical base64 |
| S-305 | Info | `payload_out == NULL` rejected for nonempty payloads | Documented contract; verify-only mode not supported (would need a scratch bound) |

## Verified behaviors (tests)

- Round-trips: text, empty, and binary (NUL-containing) payloads.
- Tamper: every single-character mutation of a real token rejected.
- Wrong key, truncation, empty token, `v2.` prefix all rejected.
- `encode_bound` exactness (written == bound-1), undersized encode
  cap → OVERFLOW, undersized decode payload cap → OVERFLOW.
- NULL/empty-key argument matrix; cookie-octet charset conformance
  of emitted tokens.

## Verification

```text
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure        # 6/6 pass
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure   # 5/5 pass
```

## Follow-up

- Example usage: signed session cookie (`cfw_signed_encode` →
  `cfw_cookie_serialize` value) in `examples/` when a consumer lands.
- AEAD/confidential tokens remain deferred with the TLS/dependency
  decision (ADR 0030 posture).
