<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0007: Non-owning string view

- Status: Accepted
- Date: 2026-11-25

## Context

HTTP header names, methods, paths, tokens, and field values are length-
delimited byte sequences that must not require NUL termination or copying.
`cfw_span_t` already models arbitrary bytes, but text-oriented operations —
equality, prefix/suffix checks, and ASCII case-insensitive matching required
by HTTP semantics — deserve a distinct, self-documenting type so call sites
express intent and the API can grow text-specific helpers without conflating
binary and text uses.

## Decision

Add `include/cfw/strview.h` and `src/cfw/strview.c` defining
`cfw_strview_t { const char *data; size_t size; }`, a non-owning view over
text bytes. The API provides:

- Construction: `cfw_strview_init`, `cfw_strview_from_cstr`,
  `cfw_strview_from_span`, and `cfw_strview_to_span` for interop with the
  byte-span primitive.
- Inspection: `cfw_strview_is_empty`.
- Comparison: `cfw_strview_equal` (byte-exact) and
  `cfw_strview_equal_nocase` (ASCII-only case folding of `A-Z` to `a-z`).
- Affix checks: `cfw_strview_starts_with` and `cfw_strview_ends_with`.

A NULL `data` pointer with `size == 0` denotes the empty view and is valid.
`cfw_strview_equal_nocase` folds only ASCII `A-Z`; all other bytes compare
exactly, so non-ASCII input cannot alias under case folding. Empty affixes
match every view, including the empty view. Comparisons and affix checks
return boolean results through an out-parameter with `cfw_status_t`, matching
the checked-arithmetic API style.

The view does not validate or normalize encoding; UTF-8 or protocol-specific
validation belongs to higher layers.

## Alternatives considered

- **Reuse `cfw_span_t` for strings:** rejected because text and binary views
  have different contracts (encoding assumptions, future helpers), and a
  distinct type prevents accidental interchange at API boundaries.
- **Owning string type now:** deferred; an owning type should be defined
  together with allocation/copy policy once a concrete consumer (e.g. header
  storage) establishes requirements.
- **Locale-aware or `tolower`-based folding:** rejected because locale
  dependence and signed-`char` hazards make behavior unpredictable; ASCII
  folding matches HTTP token semantics.
- **NUL-terminated `const char *` API:** rejected because HTTP fields are
  length-delimited and may contain arbitrary bytes.

## Consequences

- Header parsing, routing, and serialization can compare and slice text
  without copying or NUL-termination concerns.
- Case-insensitive matching is explicitly ASCII-only; any future need for
  Unicode-aware comparison requires a separate, reviewed API.
- Memory-safety and bounds audits apply to view construction and affix
  arithmetic; all functions validate NULL and inconsistent `data`/`size`
  pairs.
