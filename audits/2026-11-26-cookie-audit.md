<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: cookie parsing and serialization (ADR 0045)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: 3c7a389
- Scope: `cfw_cookie_parse`/`cfw_cookie_serialize`
  (`include/cfw/cookie.h`, `src/cfw/cookie.c`), tests in
  `tests/unit/test_cookie.c`
- Method: catalog-driven review per `docs/security/audit-catalog.md`;
  parser + output-encoding focus — Cookie headers are untrusted input
  and Set-Cookie values are response-splitting surface

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; all scans index/ptr bounded by segment extents; append helper checks `len > cap - *off - 1` before every write with the `off ≤ cap-1` invariant maintained inductively; `snprintf` result range-checked before append |
| 2 | Integer/bounds | Pass | All offsets are `ptrdiff_t`-derived segment extents cast to `size_t` only after positivity is established; `cap - *off - 1` cannot underflow (invariant); `expires_unix`/`max_age_seconds` are `int64_t` formatted via checked `snprintf` |
| 3 | Parser/protocol state machine | Pass | Single-pass `;` segmentation; first-'=' split; OWS trim bounded; DQUOTE strip requires both quotes and length ≥ 2; every name/value byte validated against token/cookie-octet tables — malformed input fails closed |
| 4 | API misuse | Pass | NULL/undersized/capacity args rejected; `written` excludes NUL; empty header yields zero pairs; pair_capacity=0 with NULL pairs accepted (count-only mode) |
| 5 | Concurrency/cancellation | N/A | Pure functions over caller storage |
| 6 | DoS resistance | Pass | O(header) parse, O(output) serialize; no allocation; output bounded by caller cap; no recursion |
| 7 | Platform portability | Pass | Byte tables only; no platform dependencies |
| 8 | Cryptography/transport | N/A | No crypto; transport unchanged |
| 9 | Supply chain/license | Pass | No new dependencies |
| 10 | Documentation/ABI | Pass | ADR 0045 records grammar, character classes, strict-vs-lenient choices, and rejected alternatives; additive API only |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| W-301 | Low | `;` inside a DQUOTE-wrapped cookie value still splits the segment | Accepted; RFC 6265 cookie-octet excludes ';' even inside quotes, so such a header is malformed and the parse fails closed rather than misparsing |
| W-302 | Info | OWS between cookie name and '=' is rejected | Accepted; strict token validation; browsers never emit it; matches fail-closed posture |
| W-303 | Info | `SameSite=None` does not auto-add `Secure` | Accepted; attribute semantics are caller policy; serializer stays mechanical, documented |
| W-304 | Info | Cookie values are not percent-decoded on parse or quoted on serialize | Accepted per RFC 6265; unsafe bytes are rejected on both sides, forcing callers to sanitize |

## Verified behaviors (tests)

- Parse: multi-pair, empty values, OWS tolerance, DQUOTE stripping,
  '=' inside values, empty segments, capacity/drop accounting,
  argument validation.
- Malformed: missing '=', empty/invalid names, OWS before '=',
  space/comma/backslash in values, unterminated quote, space inside
  quotes, non-ASCII bytes — all rejected.
- Serialize: bare pair, empty value, all attributes (Path, Domain,
  Max-Age, Expires IMF-fixdate, Secure, HttpOnly, Partitioned,
  SameSite Lax/Strict/None), validation rejections (bad name/value/
  path chars, bad enum, undersized buffer, NULL args), round-trip
  through the parser.

## Verification

```text
cmake --build --preset debug
ctest --preset debug --output-on-failure
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure
cmake --preset shared && cmake --build --preset shared
ctest --preset shared --output-on-failure
```
