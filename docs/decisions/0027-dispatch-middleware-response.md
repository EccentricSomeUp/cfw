<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0027: Request dispatch, middleware, and response pipeline

- Status: Accepted
- Date: 2026-11-26

## Context

Phase 4 needs to connect the route table (ADR 0026) to application code:
an ordered middleware chain with cancellation, a terminal handler, error
mapping, and response serialization into the `cfw_http1_conn_t` output
queue. The design must keep per-request state caller-owned (the library
is single-threaded per connection but shared across connections), avoid
hidden allocations, and make cancellation explicit rather than
exception-like.

## Decision

Add `cfw/dispatch.h` + `src/cfw/dispatch.c` with a stateless
`cfw_dispatch_t` configuration object:

- **Request view** (`cfw_request_t`): populated per call from a parsed
  `cfw_http_request_head_t`, a body span, and caller scratch arrays for
  headers and route params. All strviews reference caller/transport
  storage valid for the call duration; the dispatch object holds no
  per-request state, so one config can serve many connections.
- **Response** (`cfw_response_t`): status, caller array of headers with
  capacity, a single body span, and an app `user` pointer. Initialized
  to `200 OK` with empty body before the chain runs.
- **Middleware** (`cfw_middleware_fn`): onion model — each middleware
  receives `(req, res, next, next_ctx, user)` and invokes
  `next(next_ctx)` to continue. Registration order is outermost-first:
  middleware registered first sees the request first and the response
  last. `next_ctx` is an opaque chain frame managed by the dispatch;
  middleware must not retain it.
- **Cancellation:** a middleware that returns `CFW_OK` without calling
  `next` short-circuits the chain — the response it populated is used
  as-is. Any non-`CFW_OK` return aborts dispatch into the error path.
  Calling `next` more than once is a contract violation (documented;
  the chain does not defend against it).
- **Terminal handler** (`cfw_handler_fn`): invoked after the last
  middleware with the same `(req, res, user)`; `user` comes from
  dispatch configuration. Route identity is available via
  `req->match`/`req->route_user` for handler selection.
- **Error mapping:** bad head/framing → 400, no route → 404,
  handler/middleware/internal failure → 500. A configurable
  `cfw_dispatch_error_fn` receives `(req, res, status_hint, user)` and
  fills the response; the default emits the status with an empty body.
- **Response serialization:** `cfw_dispatch_response_head_format`
  writes status line + headers + CRLF terminator into a caller buffer,
  automatically appending `Content-Length` when a body is present, no
  CL/TE header exists, and the method/status permit a body
  (`cfw_http_response_has_body`). The body span is queued separately by
  the caller through `cfw_http1_conn_queue`.
- **Validation:** dispatch re-checks request framing
  (`cfw_http_request_framing`) and body-size consistency so it is safe
  for non-http1 callers; body views are capped by the transport's
  `max_body_size` before dispatch is invoked.

## Consequences

- Streaming/chunked bodies are accumulated by the caller into a single
  span before dispatch; incremental request-body handlers are deferred.
- Middleware cannot easily short-circuit *and* continue post-processing
  after cancellation — it owns the response once it stops the chain.
- Per-route middleware is out of scope; global ordering only in v1.
- The API is intentionally small enough to freeze at the Phase 4
  stability review.
