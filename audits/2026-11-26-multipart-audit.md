<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: multipart/form-data parsing (ADR 0043)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: a61940f
- Scope: `cfw_multipart_boundary`/`cfw_multipart_parse`
  (`include/cfw/multipart.h`, `src/cfw/multipart.c`), part-header
  reuse of `cfw_http_headers_next`, tests in
  `tests/unit/test_multipart.c`
- Method: catalog-driven review per `docs/security/audit-catalog.md`;
  parser focus — multipart bodies are untrusted input on every upload

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; all scanning is index-based over `cfw_span_t`/`cfw_strview_t` bounds; delimiter search bounds check `pos + prefix + needle <= n` before any read; scratch bound (`>= body->size`) verified before parsing; unescape writes bounded by quoted length |
| 2 | Integer/bounds | Pass | All offsets derived from bounded scans; `(dpos - 2) - body_start` cannot underflow (CRLF-anchored match implies dpos ≥ body_start + 2); boundary ≤70 enforced at extraction |
| 3 | Parser/protocol state machine | Pass | Delimiter requires CRLF anchor + valid trailer (padding → "--" or CRLF); false prefix matches skipped and search resumed; part headers validated by the shared header parser; missing close/malformed → INVALID_ARGUMENT |
| 4 | API misuse | Pass | NULL/undersized args rejected; capacity/drop accounting mirrors headers convention; empty body → 0 parts; scratch smaller than body → up-front rejection |
| 5 | Concurrency/cancellation | N/A | Pure function over caller storage |
| 6 | DoS resistance | Pass | O(body × boundary) worst-case delimiter scan with ≤72-byte needle; no recursion; output bounded by caller capacity + scratch; part count bounded by input |
| 7 | Platform portability | Pass | Pure byte parsing; no platform dependencies |
| 8 | Cryptography/transport | N/A | No crypto; transport unchanged |
| 9 | Supply chain/license | Pass | No new dependencies |
| 10 | Documentation/ABI | Pass | ADR 0043 records grammar, scratch contract, buffered-body scope, and rejected alternatives; additive API only |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| W-101 | Low | Epilogue after the close delimiter is only tolerated to end-of-body | Accepted; HTTP multipart (RFC 7578) does not use epilogues; strictness prevents delimiter smuggling |
| W-102 | Info | `Content-Transfer-Encoding` (base64/qp) is not decoded | Accepted; deprecated for HTTP multipart per RFC 7578; bodies returned verbatim, documented |
| W-103 | Info | Upload-streamed bodies cannot use this parser | Accepted for 1.0.0: incremental parsing stays deferred (buffered bodies cover the common case; upload-streamed callers parse incrementally themselves per `multipart.h` and ADR 0043) |
| W-104 | Info | `scratch_size >= body->size` is a coarse bound | Accepted; decoded params never exceed source bytes and typical form bodies are small; documented |

## Verified behaviors (tests)

- Boundary extraction: bare token, quoted-string, case-insensitive
  parameter name, extra parameters; missing/empty/over-70/malformed
  rejections.
- Two-part parse: names, filename, per-part Content-Type, body
  spans, non-empty header block.
- Preamble/epilogue tolerance and transport padding around
  delimiters.
- Quoted-string escapes (`\"`) unescaped into scratch; bare token
  params referenced directly.
- Headerless parts (empty name/header_block) and empty part bodies.
- Boundary-prefix collision (`--XYZabc` inside a body does not
  terminate the part).
- Malformed inputs: no delimiters, missing close, unterminated
  headers, invalid header lines → CFW_ERROR_INVALID_ARGUMENT.
- Capacity/drop accounting and argument validation.

## Verification

```text
cmake --build --preset debug
ctest --preset debug --output-on-failure
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure
cmake --preset shared && cmake --build --preset shared
ctest --preset shared --output-on-failure
```
