<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: shared URI percent-decoding (ADR 0044)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: b5585d2
- Scope: `cfw_uri_decode` (`include/cfw/uri.h`, `src/cfw/uri.c`),
  refactors of `cfw_query_parse` and `cfw_static_decode`, new
  `cfw_route_param_decode`, tests in `tests/unit/test_uri.c` and
  `test_route.c`
- Method: catalog-driven review per `docs/security/audit-catalog.md`;
  parser focus — percent-decoding gates filesystem path construction
  and handler-visible values

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; all reads bounded by `i < len` with the escape lookahead checked (`i + 2 >= len` rejected before indexing); writes bounded by `o + 1 >= cap` before each store; NUL terminator only written when capacity allows |
| 2 | Integer/bounds | Pass | `len`/`cap` are `size_t`; no arithmetic that can wrap (`i + 2` cannot overflow since `i < len ≤ SIZE_MAX-2` is not guaranteed — see W-201); flag mask rejects unknown bits |
| 3 | Parser/protocol state machine | Pass | Single-pass scanner; `%` requires exactly two hex digits or fails; no partial state across calls; decoded byte validated against flags after expansion |
| 4 | API misuse | Pass | NULL/zero-cap/unknown-flag rejection; `cap` must cover decoded+1 so output is always NUL-terminated when it fits |
| 5 | Concurrency/cancellation | N/A | Pure function over caller storage |
| 6 | DoS resistance | Pass | O(len) single pass, no recursion, no allocation; output never exceeds input |
| 7 | Platform portability | Pass | Byte-oriented, no platform dependencies; backslash/NUL rejection flags cover Windows path hazards |
| 8 | Cryptography/transport | N/A | No crypto; transport unchanged |
| 9 | Supply chain/license | Pass | No new dependencies |
| 10 | Documentation/ABI | Pass | ADR 0044 records flags, scratch-bound change, and the deliberate decision to keep route matching on the encoded path; additive API only |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| W-201 | Info | `i + 2 >= len` could theoretically wrap if `len == SIZE_MAX` | Accepted; `len` derives from bounded request-target/path spans far below `SIZE_MAX`, and the comparison is `i + 2 >= len` evaluated as `i + 2` first — `i < len` bounds `i ≤ SIZE_MAX-1`, so `i + 2` can wrap only when `len > SIZE_MAX - 2`, unreachable for span-derived inputs |
| W-202 | Info | `cfw_query_parse` scratch bound tightened `size` → `size + 1` | Accepted; documented in header and ADR; one spare byte covers the decoder's transient terminator |
| W-203 | Info | Route matching remains on the encoded path | Accepted by design; `%2F` cannot fabricate segments, and `cfw_route_param_decode` gives handlers decoded values without changing match semantics |
| W-204 | Info | Decoded route params are not guaranteed NUL-terminated | Accepted; `out_len` contract is explicit, consistent with `cfw_strview_t` conventions |

## Verified behaviors (tests)

- Basic decoding, lowercase/uppercase hex, UTF-8 passthrough,
  empty input.
- `+`→SP only under `CFW_URI_DECODE_PLUS_AS_SPACE`.
- NUL/backslash rejection applies to literal and decoded bytes.
- Malformed escapes (`%`, `%2`, `%zz`, truncated at end) rejected.
- Capacity edge (`cap == decoded + 1` succeeds, `== decoded` fails),
  `cap == 0` rejected, NULL/unknown-flag rejection.
- `cfw_route_param_decode`: encoded value stays raw via
  `cfw_route_match_param`, decoded via helper, `+` literal for path
  params, flag propagation, malformed escape error, unknown param,
  undersized buffer.
- Regression: full `test_query.c` and `test_static.c` suites green
  after the shared-decoder refactor.

## Verification

```text
cmake --build --preset debug
ctest --preset debug --output-on-failure
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure
cmake --preset shared && cmake --build --preset shared
ctest --preset shared --output-on-failure
```
