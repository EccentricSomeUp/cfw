<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0038: HTTP-date parsing

## Status

Accepted.

## Context

Static-file conditional requests (`If-Modified-Since`, `If-Range`) and
other HTTP caching logic need to parse `HTTP-date` field values. RFC
9110 requires recipients to accept three formats — IMF-fixdate,
obsolete RFC 850, and ANSI C `asctime` — while senders generate only
IMF-fixdate. RFC 850 two-digit years are relative to the recipient's
clock, so a pure parser needs an explicit reference time for
deterministic behavior and testing.

Constraints:

- Parsing must be allocation-free and bounded by the input view.
- Impossible dates (e.g. February 30, non-leap February 29) must be
  rejected rather than normalized.
- The parser is a protocol primitive; it must not read past the
  `cfw_strview_t` bounds or depend on NUL termination.

## Decision

Add `cfw_http_date_parse` and `cfw_http_date_parse_at` to `cfw/http.h`:

- `cfw_http_date_parse_at(text, now, out)` parses all three HTTP-date
  formats. IMF-fixdate is accepted only in its fixed 29-byte form;
  RFC 850 and `asctime` are parsed with a bounded cursor. Fields are
  validated for range and calendar correctness; years outside
  1900..9999 are rejected.
- RFC 850 two-digit years map to `2000 + yy`, then slide back 100 years
  when the resulting timestamp would be more than 50 years after `now`
  (RFC 9110 §5.6.7), comparing the full month/day/time-of-day.
- `cfw_http_date_parse(text, out)` is the convenience wrapper that
  obtains `now` from `cfw_time_wall_now`; `_at` is the deterministic
  variant for callers that already hold a timestamp.

Weekday names are validated as tokens but not required to match the
computed date; the calendar fields are the unambiguous source of truth.

## Alternatives considered

- **IMF-fixdate only:** rejected. RFC 9110 requires accepting all three
  formats, and `asctime`/RFC 850 still appear in legacy caches and
  intermediaries.
- **Fixed 1970–2069 RFC 850 mapping:** rejected. That rule belongs to
  cookie expiration (RFC 6265), not HTTP-date; RFC 9110 requires the
  sliding 50-year interpretation.
- **Return a struct instead of Unix seconds:** rejected. Callers need a
  single comparable scalar for `If-Modified-Since`/`If-Range`; seconds
  since the epoch match `cfw_http_date_format` and `cfw_time_wall_now`.

## Consequences

- Positive: enables standards-compliant conditional-request handling
  without adding dependencies; deterministic `_at` variant keeps tests
  stable; reuses the same civil-date algorithm family as
  `cfw_http_date_format`.
- Negative: RFC 850 parsing needs a reference time, so the simple
  wrapper performs a wall-clock read; obsolete formats add parser
  surface that must stay bounded (it is: fixed buffers/cursor, no
  allocation).
- Compatibility: additive API only; no existing behavior changes.
