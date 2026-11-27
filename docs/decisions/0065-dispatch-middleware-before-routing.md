<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0065: Dispatch middleware runs for unmatched routes

- Status: Accepted
- Date: 2026-11-27

## Context

ADR 0036 designed `cfw_static_serve` as dispatch middleware that
intercepts a URL prefix and short-circuits file responses. Its
context section states the assumption explicitly: "Dispatch
middleware (ADR 0027) runs before routing and may short-circuit by
returning `CFW_OK` without calling `next()`; this is the natural
interception point for a URL prefix."

The implementation diverged: `cfw_dispatch_request_ex` ran
`cfw_router_match` and produced the 404/405 error response *before*
invoking the middleware chain. Since the router has no wildcard or
prefix patterns (ADR 0036 context), no route can match a file URL —
so `cfw_static_serve` mounted via `cfw_dispatch_use` could never
intercept anything. Every request under the configured prefix fell
through to the router's 404. The bug was invisible to the unit tests,
which call `cfw_static_serve` directly rather than through dispatch.

## Decision

Run the middleware chain for every well-formed request, including
those whose path matched no route. Route matching still happens up
front so `req->match`, `req->params`, and `req->route_user` are
populated for middleware inspection; only the 404/405 short-circuit
moves from before the chain to the chain's terminal step:

- `cfw_dispatch_chain_next`'s terminal branch now checks
  `req->match.route == NULL` first and produces the same 404/405
  (+ `Allow` header) responses via the error handler as before.
- Middleware may short-circuit unmatched requests (the static-file
  use case) or pass them through via `next()`, in which case the
  terminal step emits the route-miss response unchanged.

## Consequences

- `cfw_static_serve` (and any prefix-intercepting middleware) now
  works as ADR 0036 intended. The `examples/static_files.c` example
  demonstrates the mounted behavior end-to-end.
- Middleware now observes requests that will 404/405 — a behavioral
  change consistent with the onion model ("sees the request
  first"). Middleware that assumed `req->match.route != NULL` must
  check it; `match.route == NULL` was already a documented state.
- Error-mapping semantics are unchanged: bad head/framing still 400s
  before the chain; no-route still 404s (or 405 + `Allow`); handler
  failure still 500s.
- No API or ABI change; `cfw_dispatch_request*` signatures and all
  public structs are untouched.
