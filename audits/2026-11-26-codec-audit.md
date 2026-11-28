<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: byte-to-text codecs (ADR 0047)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: a4d8574
- Scope: `cfw_base64_*`, `cfw_hex_*` (`include/cfw/codec.h`,
  `src/cfw/codec.c`), ws.c refactor to shared base64,
  tests in `tests/unit/test_codec.c`
- Method: catalog-driven review per `docs/security/audit-catalog.md`;
  RFC 4648 vectors; malformed-input matrix; ASan/UBSan preset

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; encode capacity precomputed via bound helper and checked before any write; decode checks `o + group_bytes > cap` per group before writing; hex decode validates all digits in a first pass before writing anything (no partial output on malformed input) |
| 2 | Integer/bounds | Pass | `encode_bound` helpers guard SIZE_MAX-scale inputs (conservative `((SIZE_MAX-1)/4)*3` and `(SIZE_MAX-1)/2` thresholds, SIZE_MAX sentinel); decode length must be `%4==0`/`%2==0` before indexing; group decode uses fixed 4-char windows, no OOB reads |
| 3 | Parser/protocol state machine | Pass | Strict canonical grammar: '=' only trailing in final group, ≤2 pads, zero pad bits enforced; hex requires even length and digit-only; whitespace/foreign chars rejected — no parser differentials |
| 4 | API misuse | Pass | NULL contracts documented: `out` required when output nonempty, `data`/`in` NULL rejected, `written` optional; empty input decodes to 0 bytes with NULL `out` permitted |
| 5 | Concurrency/cancellation | N/A | Pure functions over caller storage |
| 6 | DoS resistance | Pass | O(n) single pass (hex decode is two passes, still O(n)); no allocation; output bounded by caller cap; oversized inputs rejected before decode work proportional to output |
| 7 | Platform portability | Pass | Byte tables and shifts only; `uint32_t` triple assembly is endian-independent; C11 only |
| 8 | Cryptography/transport | Pass with note | Encodings are not cryptographic; canonical-strict decode is the right posture for signature/token material. ws.c now rejects non-canonical base64 keys (pad bits nonzero) — tightening, RFC-compliant clients unaffected |
| 9 | Supply chain/license | Pass | No new dependencies; ws.c private code removed, net negative duplication |
| 10 | Documentation/ABI | Pass | ADR 0047 records alphabet choice, mandatory padding, deferred url-safe/nopad variants, and the ws strictness change; additive API only |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| K-301 | Info | `cfw_base64_decode` rejects non-canonical pad bits | Accepted/intended; RFC 4648 canonical form; ws Sec-WebSocket-Key values from conforming clients are canonical |
| K-302 | Info | No URL-safe alphabet or unpadded variant | Deferred; additive API can add `cfw_base64url_*` when a consumer needs it |
| K-303 | Info | Encode requires exact-capacity planning via bound helpers | Accepted; bound helpers make sizing trivial and overflow-safe |
| K-304 | Info | `written` is optional (NULL allowed) | Accepted; matches optional-out-param style elsewhere (e.g. `dropped_out`) |

## Verified behaviors (tests)

- Base64: all seven RFC 4648 vectors (encode+decode), bound values
  and SIZE_MAX overflow, exact/undersized capacity, NULL argument
  matrix, malformed corpus (bad length, misplaced/leading/excess
  padding, padding mid-stream, nonzero pad bits, whitespace, foreign
  chars), capacity-limited decode, empty input with NULL out.
- Hex: round-trip, lowercase output, mixed-case decode, malformed
  corpus (odd length, non-hex chars, "0x" prefix, spaces), capacity,
  bound overflow.
- ws: existing handshake/accept tests pass unchanged against the
  shared codec (RFC 6455 vector: key "dGhlIHNhbXBsZSBub25jZQ==" →
  accept "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=").

## Verification

```text
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure        # 6/6 pass
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure   # 5/5 pass
```

## Follow-up

- Add `cfw_base64url_*` (unpadded, '-' '_') only when a token
  consumer requires it.
- Signed-cookie/token modules (next ADRs) should build on this
  codec plus cfw_hmac_sha256 and cfw_random_bytes.
