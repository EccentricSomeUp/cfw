<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: post-roadmap hardening — streaming, peer address, uploads

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: 1e2c891
- Scope: changes since the 1.0-readiness audit — response streaming
  (ADR 0031), Date header, Host enforcement, HEAD fallback/405,
  `remote_addr` (ADR 0032), request upload hooks (ADR 0033), and the
  connection-lifecycle fixes they surfaced
- Method: catalog-driven review per `docs/security/audit-catalog.md`

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; span/buffer lifetimes documented (stream chunks borrowed until next call; `remote_addr` valid for dispatch call; upload pieces valid for the call only) |
| 2 | Integer/bounds | Pass | Peer buffer fixed 72 B with snprintf bound check; upload path adds no allocation in framework code |
| 3 | Parser/protocol | Pass with fixes | Response framing ownership enforced (TE/CL stripped, correct CL or chunked emitted); `Connection: close` preserved on chunked streams of closing requests (regression test); HEAD mirrors GET headers; 405 + `Allow` |
| 4 | API misuse | Pass | `cfw_dispatch_request_info_t` documented; upload hooks require begin+write pairing (init validation); `upload_end` exactly-once semantics tested on success, write failure, begin failure, and timeout abort |
| 5 | Concurrency/cancellation | Pass | All hooks run on the connection's event-loop thread; pipelined input suspended during active streams and uploads are strictly sequential |
| 6 | DoS resistance | Pass | `max_body_size` still bounds total body bytes under uploads (now a time/bandwidth bound rather than memory); `max_pending_size` unchanged; rejected conns get drain deadlines |
| 7 | Platform portability | Pass | Peer formatting uses internal `cfw_addr_*` helpers; snprintf path POSIX-clean; Windows stubs unaffected |
| 8 | Cryptography/transport | N/A | TLS deferred (ADR 0030) |
| 9 | Supply chain/license | Pass | No new dependencies |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| H-101 | High | fd-reuse/pollset leak: conns whose fd was closed inside `http1_conn` lost their handle, so `pollset_remove` no-op'd and the stale epoll record blocked `pollset_add` for recycled fd numbers — new connections starved | Fixed in 39363b4: handle cached on `cfw_server_conn_t`, dead-fd conns reaped in `update_conn_interest` |
| H-102 | Medium | Stack-use-after-scope: Date-header strview outlived its scoped buffer | Fixed in d852bd6: buffer hoisted to function scope |
| H-103 | Medium | `cfw_time_wall_now` POSIX path missing NULL check (segfault) | Fixed in d852bd6 |
| H-104 | Medium | Chunked stream on `Connection: close` request lost its close header — `will_close` computed after `close_after_write` was cleared for the pump | Fixed in 1fa9468 via `stream_hold_close`; regression test added |
| H-105 | Low | Dispatch body-consistency check rejected legitimately streamed uploads (empty `req.body` vs declared length) | Fixed in d6bd02e: check skipped when `info->upload` is set |

## Reviewed and rejected concerns

- Post-reject dispatch: `service_conn` gates `service_read` on
  `!conn->rejecting`, so BODY/DONE events of a rejected request can
  never reach `handle_done`. Verified by inspection.
- Double `upload_end`: every path that ends an upload NULLs
  `conn->upload`; `close_conn` only fires for uploads still open,
  which are by definition incomplete.
- `upload_begin` returning `CFW_OK` with `*upload == NULL` is treated
  as failure (connection aborted); documented contract requires a
  non-NULL token.
