<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: cryptographic primitives (ADR 0046)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: 74b59da
- Scope: `cfw_sha256_*`, `cfw_hmac_sha256`, `cfw_secure_equal`
  (`include/cfw/crypto.h`, `src/cfw/crypto.c`), tests in
  `tests/unit/test_crypto.c`
- Method: catalog-driven review per `docs/security/audit-catalog.md`
  §8 (cryptography and transport); known-answer tests against FIPS
  180-4 and RFC 4231 vectors; ASan/UBSan preset

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; compression reads exactly 64-byte blocks; update path bounds every memcpy by `64 - block_len` or remaining `size`; padding loop terminates because `block_len` strictly increases mod 64 toward 56; `total_len` mod 2^64 cannot wrap in practice (would require >2^61 bytes) |
| 2 | Integer/bounds | Pass | All lengths are `size_t`/`uint64_t`; `size * 8` computed in `uint64_t` before addition; block scheduling indices are `unsigned int` bounded by 64; bit-length serialization shifts `56 - i*8` for i<8 (no negative shift) |
| 3 | Parser/protocol state machine | N/A | No parsing; compression is fixed-sequence arithmetic |
| 4 | API misuse | Pass | NULL/undersized contracts documented and guarded: `init`/`update`/`final`/`sha256`/`hmac`/`secure_equal` return early on NULL where a non-NULL contract exists; `size==0 && data==NULL` accepted per codebase convention; `secure_equal(NULL, NULL, 0)` valid |
| 5 | Concurrency/cancellation | Pass | Context is caller-owned, not shared; no globals mutated (K table is `const`); no threads |
| 6 | DoS resistance | Pass | O(n) single pass, no allocation, no recursion; `secure_equal` deliberately does not early-exit (timing is length-proportional only, matching its MAC-comparison purpose); callers must compare lengths separately — documented |
| 7 | Platform portability | Pass | Only `uint32_t`/`uint64_t` arithmetic and `memcpy`/`memset`; no endianness assumptions (block words assembled big-endian explicitly); C11 only |
| 8 | Cryptography/transport | Pass with notes | SHA-256/HMAC per FIPS 180-4 / RFC 2104 verified by KATs; explicitly NOT for password hashing (documented in header); constant-time compare provided for MAC verification; TLS unchanged (ADR 0030) |
| 9 | Supply chain/license | Pass | No new dependencies; implementation is in-tree original code |
| 10 | Documentation/ABI | Pass | ADR 0046 records scope exclusions (KDF, AEAD, RNG, streaming HMAC) and rejected alternatives; additive public API only; `cfw_sha256_ctx_t` layout is public but versioned under the pre-1.0 freeze policy |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| C-301 | Info | `cfw_sha256_final` mutates the context (padding via `update`), so a finalized ctx cannot be re-finalized | Accepted; standard streaming-hash contract, matches every SHA-256 implementation; callers needing reuse re-`init` |
| C-302 | Info | `cfw_secure_equal` is not constant-time w.r.t. `size` itself | Accepted; length equality is the caller's job per docs — leaking length of a fixed-size MAC is harmless; early-exit on size would break MAC comparison use |
| C-303 | Info | HMAC is one-shot only; no streaming HMAC | Deferred; no current consumer needs it; additive API can add it later |
| C-304 | Info | ws.c retains private SHA-1/base64 copies | Accepted; SHA-1 there is an RFC 6455 protocol transform, not a security boundary; dedup deferred until a shared encoding helper is justified |
| C-305 | Info | `total_len` is bits mod 2^64 | Accepted; spec-mandated truncation; unreachable in practice (2^61 bytes) |

## Verified behaviors (tests)

- SHA-256: empty, "abc", FIPS 180-4 56-byte vector, million-'a'
  (1,000,000 bytes via 1000×1000 streaming updates), streaming
  equivalence at 7-byte chunks, padding boundaries 55/56/63/64/65/
  119/120/128 bytes.
- HMAC-SHA256: RFC 4231 cases 1–3, >64-byte key hashing path,
  empty key + empty message.
- `cfw_secure_equal`: equal, unequal (last byte), size 0,
  NULL/NULL/0.

## Verification

```text
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure        # 6/6 pass
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure   # 5/5 pass
```

## Follow-up

- Password hashing / AEAD / TLS remain out of scope pending a
  dependency ADR (ADR 0030 posture).
- Revisit streaming HMAC and shared base64/hex encoding when a
  second consumer appears.
