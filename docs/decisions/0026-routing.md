<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0026: Route table and matching semantics

- Status: Accepted
- Date: 2026-11-26

## Context

Phase 4 needs a route table mapping `(method, path)` to application
handlers with path-parameter extraction. The design must stay small,
allocation-bounded, and unambiguous — routing is a security boundary
(authz decisions depend on which route matched).

## Decision

Add `cfw/route.h` + `src/cfw/route.c`, a `cfw_router_t` storing routes in
a caller-provided `cfw_arena_t` (bulk-free with the server lifetime):

- **Pattern syntax:** `/seg/seg/...` of literal segments and `:name`
  parameters. A parameter matches exactly one nonempty path segment
  (no `/`). No regex, wildcards, or optional segments in v1 — the grammar
  is fully enumerable and cheap to validate at registration.
- **Match semantics:** segment-count equality; literal segments compare
  case-sensitively byte-for-byte (RFC 3986 paths are case-sensitive);
  parameters capture the raw, still-percent-encoded segment bytes.
  First registered match wins, making registration order explicit
  priority.
- **Methods:** each route binds one `cfw_http_method_t` or
  `CFW_HTTP_METHOD_ANY`-equivalent wildcard via a dedicated value.
- **Extraction:** `cfw_router_match` fills a caller-provided
  `cfw_route_param_t` array (name+value strviews into the target);
  `CFW_ERROR_INVALID_ARGUMENT`-style overflow is avoided by returning the
  required count so callers can size once and match twice, or query
  params by name via `cfw_route_match_param`.
- **Target input:** the router matches the raw `request-target` path
  portion only; callers strip query/fragment first (helper provided).
  Percent-decoding is deliberately *not* performed at match time —
  decoded-vs-raw ambiguity is a classic traversal vector; a separate,
  opt-in decode helper will come with path normalization rules.

## Alternatives considered

- **Regex/glob patterns:** rejected for v1; unbounded compile cost and
  catastrophic-backtracking surface for marginal benefit.
- **Radix-tree storage:** deferred; linear scan over a bounded route
  count is fast enough for typical APIs, and arena storage keeps it
  simple. A sorted/trie index can be added later without API change.
- **Case-insensitive paths:** rejected; would merge distinct resources.

## Consequences

- Route count and pattern length are bounded by arena capacity; matching
  is O(routes × segments) with zero allocation.
- Ambiguity is impossible: patterns are validated at registration
  (duplicate `:name`s within a pattern, empty segments, invalid chars).
- Middleware ordering/cancellation builds on match results in Phase 4
  follow-ups.
