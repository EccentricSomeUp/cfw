<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: HTTP-date parsing (ADR 0038)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: f037e2b
- Scope: `cfw_http_date_parse` / `cfw_http_date_parse_at`
  (`include/cfw/http.h`, `src/cfw/http.c`) and tests in
  `tests/unit/test_http.c`
- Method: catalog-driven review per `docs/security/audit-catalog.md`;
  parser/protocol focus for a new network-facing primitive

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; parsing is cursor/index based over the caller's `cfw_strview_t` with explicit remaining-length checks; no allocation or NUL requirement |
| 2 | Integer/bounds | Pass | Digit fields are accumulated with fixed widths (2/4 digits); calendar conversion uses int64 civil-date math bounded to years 1900..9999; RFC 850 reference time is validated non-negative |
| 3 | Parser/protocol state machine | Pass with notes | Accepts exactly the three RFC 9110 HTTP-date formats; IMF-fixdate is fixed-length, RFC 850/asctime use a bounded cursor; impossible dates and trailing bytes fail closed; weekday tokens are validated but not required to match the date |
| 4 | API misuse | Pass | NULL views/outputs and negative reference times are rejected; `cfw_http_date_parse` reports `CFW_ERROR_UNAVAILABLE` if the wall clock is unavailable |
| 5 | Concurrency/cancellation | Pass | Pure function plus one wall-clock read; no shared mutable state |
| 6 | DoS resistance | Pass | Input is already bounded by header limits; parser performs O(input) work with no recursion or allocation |
| 7 | Platform portability | Pass | Platform-neutral C11; wall-clock dependency is the existing `cfw_time_wall_now` boundary |
| 8 | Cryptography/transport | N/A | Timestamp parsing only; no crypto or transport changes |
| 9 | Supply chain/license | Pass | No new dependencies |
| 10 | Documentation/ABI | Pass | ADR 0038 records format support and the RFC 850 reference-time rule; additive API only |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| D-101 | Low | Weekday names are not cross-checked against the computed calendar date | Accepted: RFC 9110 defines the calendar fields as the timestamp; requiring weekday agreement would reject otherwise unambiguous legacy dates and is not required for safe comparison |
| D-102 | Info | Years are restricted to 1900..9999 | Accepted: covers all practical HTTP-date uses and matches the formatter's four-digit-year domain; out-of-range values fail closed |

## Verified behaviors (tests)

- IMF-fixdate, `asctime`, and RFC 850 known-answer timestamps.
- Leap-day acceptance (2000-02-29) and rejection (1900-02-29, 1994-02-30).
- RFC 850 sliding-year rule for both "94"→1994 and "70"→1970 under fixed
  reference times.
- Rejection of malformed fields, impossible times, trailing bytes,
  lowercase IMF-fixdate, NULL arguments, and negative reference time.

## Verification

```text
cmake --build --preset debug
./build/debug/tests/cfw_unit_tests --filter http
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure
cmake --preset shared && cmake --build --preset shared
ctest --preset shared --output-on-failure
```
