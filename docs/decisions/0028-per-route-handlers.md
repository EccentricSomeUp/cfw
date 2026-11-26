<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0028: Per-route handlers and dispatch extensibility

- Status: Accepted
- Date: 2026-11-26

## Context

ADR 0027 left route identity to the application: a single terminal
handler received `req->route_user` and had to switch on it. Real
applications and plugins need per-route handlers without giving up the
router's transport-agnostic design (the router must not know the
dispatch handler signature).

## Decision

- `cfw_route_t` gains an opaque `ex` extension pointer, set through the
  additive `cfw_router_add_ex` (`cfw_router_add` passes NULL). The
  pointer is copied verbatim into `cfw_route_match_t.ex` on match; the
  router never interprets it.
- `cfw_dispatch_add_route(dispatch, method, pattern, handler, user)`
  allocates a `cfw_route_binding_t {handler, user}` from the dispatch
  arena and registers the route with the binding as `ex`.
- At the end of the middleware chain, dispatch invokes the matched
  route's binding handler when `match.ex` is non-NULL, otherwise the
  dispatch default handler. Both receive `(req, res, user)`.
- Extension points now available to applications/plugins: global
  middleware (`cfw_dispatch_use`), per-route handlers
  (`cfw_dispatch_add_route`), custom error mapping
  (`options.error_handler`), and route/user data (`match.user`,
  `req->route_user`, `res->user`). Routes may also be added directly to
  the shared router at any time; dispatch picks them up on the next
  match.

## Consequences

- Mixed registration styles are unambiguous: `ex == NULL` always means
  "use the default handler".
- Bindings live in the dispatch arena; they are freed with it. Routes
  registered on a router whose arena differs from the dispatch arena
  still work — only `cfw_dispatch_add_route` requires the dispatch
  arena.
- Plugins that only add middleware or error mapping need no route
  changes; plugins adding routes use `cfw_dispatch_add_route` or plain
  `cfw_router_add` plus the default handler.
