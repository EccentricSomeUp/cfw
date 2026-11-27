<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: dispatch middleware ordering fix (ADR 0065)

- Date: 2026-11-27
- Auditor: Devin (agent)
- Commit: 85c8ba1
- Category: 3 parser/protocol state machines, 4 API misuse/lifetime,
  6 denial-of-service
- Scope: `src/cfw/dispatch.c` (`cfw_dispatch_request_ex`,
  `cfw_dispatch_chain_next`, `cfw_dispatch_route_miss`),
  `include/cfw/dispatch.h`, `tests/unit/test_dispatch.c`,
  `examples/static_files.c`

## Goals

- Verify the middleware-ordering fix (route-miss moved to the chain
  terminal) preserves response semantics and does not introduce new
  attack surface.

## Method

- Code review of the reordered dispatch flow: framing validation and
  body-consistency checks still run before the chain; route matching
  still populates `req->match`/`params`/`route_user` before the chain;
  the 404/405 (+`Allow`) responses are produced by the same
  `cfw_dispatch_fail`/error-handler path as before, now reachable as
  the chain terminal.
- Checked every early return in `cfw_dispatch_request_ex`:
  INVALID_ARGUMENT, 400 (framing/body), INTERNAL (router failure)
  all precede the chain unchanged.
- Confirmed middleware cancellation semantics: a middleware returning
  CFW_OK without `next()` short-circuits identically for matched and
  unmatched requests; non-OK aborts to the 500 error path unchanged.
- End-to-end verification via `examples/static_files.c`: file 200,
  Range 206, missing file 404, dynamic route pass-through.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| A-001 | medium | src/cfw/dispatch.c | Pre-fix, `cfw_static_serve` and any prefix middleware could never intercept: unmatched paths 404'd before the chain ran — latent functional bug, not a vulnerability | fixed (ADR 0065) |
| A-002 | low | include/cfw/dispatch.h | Middleware now observes requests that will 404/405; middleware assuming `req->match.route != NULL` must check it (documented state) | accepted, documented |

## Verification

```text
cmake --build --preset debug && ctest --preset debug   # 6/6 pass
cmake --preset sanitizers && ctest --preset sanitizers  # 5/5 pass
curl checks against cfw_example_static_files: 200/206/404/route OK
```

## Follow-up

- None; behavior is covered by `dispatch_middleware_unmatched`.
