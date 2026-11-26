<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: query-string parsing helpers (ADR 0042)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Scope: `cfw_query_parse` (`include/cfw/query.h`, `src/cfw/query.c`),
  `cfw_http_target_query` (`include/cfw/http.h`, `src/cfw/http.c`),
  tests in `tests/unit/test_query.c`
- Method: catalog-driven review per `docs/security/audit-catalog.md`;
  parser focus — query strings are untrusted input on every request

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; all access is index-based over `cfw_strview_t` bounds; decoded output bounded by `scratch_size >= query->size` checked before parsing; no allocation |
| 2 | Integer/bounds | Pass | Segment scan bounded by `query->size`; `%XX` requires two in-bounds hex digits (`end - src < 2` check before reading); decoded length ≤ consumed input length by construction |
| 3 | Parser/protocol state machine | Pass | Strict `%` validation (malformed → CFW_ERROR_INVALID_ARGUMENT, mappable to 400); empty segments skipped; first-'=' split with literal later '='; `+`→SP only inside the query grammar |
| 4 | API misuse | Pass | NULL/undersized arguments rejected up front; `dropped_out` optional; zero-capacity parse still counts drops; empty query yields zero pairs |
| 5 | Concurrency/cancellation | N/A | Pure function over caller storage; no shared state |
| 6 | DoS resistance | Pass | O(query length) single pass; output bounded by caller scratch; no recursion or backtracking; pair count bounded by input |
| 7 | Platform portability | Pass | Pure byte parsing; no platform dependencies |
| 8 | Cryptography/transport | N/A | No crypto; transport unchanged |
| 9 | Supply chain/license | Pass | No new dependencies |
| 10 | Documentation/ABI | Pass | ADR 0042 records grammar, scratch contract, and strictness choice; additive API only |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| V-101 | Low | Strict `%` rejection 400s sloppy real-world queries a lenient parser would accept | Accepted; documented in ADR 0042; handlers may pre-validate or substitute their own parser |
| V-102 | Info | `;` is not treated as a pair separator | Accepted; HTML5 dropped `;` support; `&`-only is the modern form |
| V-103 | Info | No per-name/value length cap beyond input size | Accepted; the query is already bounded by the request-head limit |

## Verified behaviors (tests)

- Basic multi-pair parse; bare names → empty values; `name=` →
  empty value; `a=b=c` keeps later '=' literal.
- `+`→SP, `%XX` both hex cases, `%2F`, multi-byte sequences.
- Empty segments skipped (`&&a=1&&&b=2&`).
- Capacity/drop accounting including zero-capacity count-only mode.
- Malformed escapes (`%zz`, `%1`, trailing `%`, `%2`) rejected.
- Argument validation (NULL query/count/pairs, undersized scratch).
- `cfw_http_target_query`: origin-form, absolute-form, embedded '?',
  absent '?', trailing '?', NULL rejection.

## Verification

```text
cmake --build --preset debug
ctest --preset debug --output-on-failure
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure
cmake --preset shared && cmake --build --preset shared
ctest --preset shared --output-on-failure
```
