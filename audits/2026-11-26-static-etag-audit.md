<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: static entity-tag validators and conditionals (ADR 0040)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: 823ef93
- Scope: entity-tag formatting and conditional evaluation in
  `src/cfw/static.c` (`cfw_static_format_etag`, `cfw_static_i64_dec`,
  `cfw_static_trim`, `cfw_static_etag_opaque`,
  `cfw_static_etag_list_match`, `cfw_static_plan`), `ETag` header
  emission in the serve path, tests in `tests/unit/test_static.c`
- Method: catalog-driven review per `docs/security/audit-catalog.md`;
  parser/protocol and memory-safety focus

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; list parsing is index-based over `cfw_strview_t` with explicit bounds; opaque-tag scan is bounded by member length; scratch `spare` region is 82 bytes vs ≤45 needed |
| 2 | Integer/bounds | Pass | `i64_dec` negates via `~u+1` (INT64_MIN-safe); list iteration advances `pos` monotonically and terminates on empty/trailing members; etag member requires closing quote at member end |
| 3 | Parser/protocol state machine | Pass | RFC 9110 §13.2.2 precedence implemented: If-Match (strong) → IUS → If-None-Match (weak) → IMS → If-Range → Range; INM suppresses IMS, IM suppresses IUS; If-Range entity-tag requires strong comparison; malformed members skipped, unusable headers ignored |
| 4 | API misuse | Pass | No public API change; helpers are file-static; `etag` strview aliases scratch storage that outlives dispatch until head serialization |
| 5 | Concurrency/cancellation | Pass with notes | Same single-threaded assumption as ADR 0039 scratch ring; etag is formatted before plan evaluation so comparisons never touch freed storage |
| 6 | DoS resistance | Pass | Conditional evaluation is O(header length); etag formatting is fixed-size; no additional slots or allocations; malformed lists cannot loop (monotonic `pos`) |
| 7 | Platform portability | Pass | Pure string/integer code; no platform dependencies |
| 8 | Cryptography/transport | N/A | ETag is a heuristic validator, not a security token; no crypto introduced |
| 9 | Supply chain/license | Pass | No new dependencies |
| 10 | Documentation/ABI | Pass | ADR 0040 records format, comparison rules, and the heuristic-validator limitation; `cfw_static_mount_t` unchanged |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| T-101 | Low | ETag `"<mtime>-<size>"` is a heuristic validator: same-size rewrites within one mtime second keep the tag | Accepted and documented (ADR 0040); same trade-off class as timestamp validators and common static servers; a content hash would require a dependency decision |
| T-102 | Info | Malformed entity-tag list members are skipped rather than failing the request | Accepted; preserves the ADR 0039 "malformed → ignore" posture; worst case is a 200 instead of an optimized 304/412 |
| T-103 | Info | `If-Match` on GET/HEAD can yield 412, which is unusual but RFC-correct | Accepted; static middleware only serves safe methods |

## Verified behaviors (tests)

- `ETag` emitted as a quoted opaque-tag on 200/206/304 alongside
  `Last-Modified`.
- `If-None-Match`: exact → 304 (GET+HEAD), `*` → 304, `W/` weak form →
  304, multi-member list → 304, non-match → 200.
- `If-Match`: exact → 200, `*` → 200, wrong tag → 412, weak tag → 412
  (strong comparison).
- Precedence: valid non-matching `If-None-Match` suppresses a
  satisfied `If-Modified-Since` (200 not 304); matching `If-Match`
  suppresses a stale `If-Unmodified-Since` (200 not 412).
- `If-Range` entity-tag: match → 206, mismatch → 200, weak tag → 200.

## Verification

```text
cmake --build --preset debug
ctest --preset debug --output-on-failure
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure
cmake --preset shared && cmake --build --preset shared
ctest --preset shared --output-on-failure
```
