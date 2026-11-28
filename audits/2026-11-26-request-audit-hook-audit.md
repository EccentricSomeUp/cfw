<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: per-request completion audit hook (ADR 0055)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: 2f2ed45
- Scope: `cfw_request_done_fn` hook, `cfw_request_record_t`, and the
  per-connection audit state in `cfw_server_conn_t` (server.c)
- Method: catalog-driven review per `docs/security/audit-catalog.md`

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; record strviews borrow conn buffers with documented callback-only lifetime; no new allocation; emit paths verified for every exit (queued response, stream end, conn close, upgrade) |
| 2 | Integer/bounds | Pass | Byte counters are uint64 accumulations of already-bounded sizes; no arithmetic on attacker-controlled lengths beyond existing caps |
| 3 | Parser/protocol | N/A | No parser changes; audit arms only after a fully parsed head |
| 4 | API misuse | Pass with docs | Lifetime contract stated on `cfw_request_record_t` and in ADR 0055; hook must not block/reenter (same convention as upload/upgrade hooks); `status == 0` semantics documented |
| 5 | Concurrency/cancellation | Pass | Hook runs on the event-loop thread synchronously; exactly-once emission enforced by `audit_pending` cleared on first emit; deferred stream emission cannot double-fire (pending cleared in pump, later emit no-ops) |
| 6 | DoS resistance | Pass | Hook is NULL-gated; bookkeeping is a handful of scalar updates per request; a slow hook stalls only its own event loop (documented) |
| 7 | Platform portability | Pass | Uses only `cfw_time_now`/`cfw_timepoint_diff` public primitives; Windows stubs unaffected |
| 8 | Cryptography/transport | N/A | No crypto/transport changes |
| 9 | Supply chain/license | Pass | No new dependencies |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| A-201 | Low | `response_bytes` for chunked streams excludes framing overhead, so the figure is payload bytes, not wire bytes | Documented on `cfw_request_record_t.response_bytes`; intentional (framing is transport detail) |
| A-202 | Low | Records for connections killed before head parse completion are not emitted (no request head exists to describe) | Documented: emission is per *parsed* head; garbage-input coverage verified by test |

## Reviewed and rejected concerns

- Double emission for pipelined requests: emission precedes parsing of
  the next head in every path; `audit_pending` is re-armed only at
  `CFW_HTTP1_EV_HEAD`.
- Use-after-free of `conn->head` strviews: emission always precedes
  conn teardown or reuse of the read buffer; upgraded conns emit at
  activation before protocol bytes flow.
- Hook reentrancy: same restriction as existing hooks; documented.
