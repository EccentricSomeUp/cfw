<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0042: Query-string parsing helpers

## Status

Accepted.

## Context

`cfw_request_t.path` excludes the query component (ADR 0019), and
until now the framework offered no way to parse it: handlers had to
hand-roll percent-decoding, `+`-to-space handling, and pair splitting
— exactly the kind of code that breeds parser bugs. Query strings are
untrusted input on every request, so the parsing rules need to be
bounded, documented, and tested once.

## Decision

Two additive helpers:

- `cfw_http_target_query(target, &query)` (`cfw/http.h`) extracts the
  query component — the text after the first `?` — for any
  request-target form. Absent `?` yields an empty view and CFW_OK.
- `cfw_query_parse(query, pairs, capacity, scratch, scratch_size,
  &count, &dropped)` (`cfw/query.h`, new module) decodes an
  application/x-www-form-urlencoded query into `cfw_query_pair_t`
  name/value strviews.

### Semantics

- Pairs split on `&`; each pair splits at the **first** `=` (later
  `=` bytes are literal value data). Bare names yield empty values.
- Empty segments (`&&`, leading/trailing `&`) are skipped.
- `+` decodes to SP; `%XX` decodes to the byte (either hex case). A
  `%` not followed by two hex digits fails the whole parse with
  CFW_ERROR_INVALID_ARGUMENT — strict, so handlers can map it to 400.
- Decoded bytes go to caller scratch; decoding never expands, so
  `scratch_size >= query->size` is a sufficient bound checked up
  front. Returned strviews reference scratch.
- At most `pair_capacity` pairs are stored; the remainder are counted
  in `dropped_out` (optional), mirroring the `headers_dropped`
  convention. No pair or name/value length cap beyond the input size —
  the query itself is already bounded by the head-size limit.

## Alternatives considered

- **Views into the request target without decoding**: rejected —
  `+`/`%XX` are part of the format; returning raw pairs would push
  decoding back to every handler.
- **Iterator API**: rejected for now — the array form matches the
  route-params/headers conventions and is simpler for the common
  "collect a few pairs" case; an iterator can be added later without
  breaking this API.
- **`req.query` field on `cfw_request_t`**: rejected — it would need
  dispatch-level scratch ownership decisions; helpers keep the
  contract explicit and allocation-free.
- **Lenient `%` handling (pass through literally)**: rejected —
  silently accepting malformed escapes hides client bugs and creates
  parser differentials; strict failure is the safer default.

## Consequences

- Positive: handlers get correct, bounded query parsing with zero
  allocation; `cfw_http_target_query` + `cfw_query_parse` compose
  directly on `req.target`.
- Negative: callers must size scratch (query->size worst case); strict
  `%` rejection means some real-world sloppy queries 400 where a
  lenient parser would 200 — documented and handler-overridable.
- Compatibility: purely additive; no existing behavior changes.

## Verification plan

- Unit tests: pair forms, decoding (`+`, `%XX` cases), empty segments,
  capacity/drop accounting, malformed escapes, argument validation,
  `cfw_http_target_query` across target forms.
- Sanitizers + shared presets; audit record per
  `docs/security/audit-catalog.md` (parser focus).
