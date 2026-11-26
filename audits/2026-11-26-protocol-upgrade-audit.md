<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: protocol upgrades (ADR 0034)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Scope: `101 Switching Protocols` connection takeover —
  `cfw_server_options_t.upgrade`, `cfw_upgraded_*` API, pending-buffer
  handoff, event delivery, and the `service_read`/`service_conn`
  changes in `src/cfw/server.c`
- Method: catalog-driven review per `docs/security/audit-catalog.md`

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; `upgrade_pending` is a `cfw_buffer_t` freed in `cfw_server_close_conn`; staged bytes are copied (bounded by `max_pending_size` enforced at feed time), never aliased |
| 2 | Integer/bounds | Pass | `cfw_upgraded_recv` bounds copies by `capacity`; `cfw_upgraded_send` reuses the `max_out_size` all-or-nothing check before queueing; `pending_pos <= pending.size` invariant maintained by the reader |
| 3 | Parser/protocol | Pass with design notes | Post-request bytes are moved out of the HTTP reader before any further parsing; `service_read` breaks on `conn->upgraded` so leftover bytes cannot be reparsed as HTTP; takeover requires 101 + request `Upgrade` header + configured hook — three independent opt-ins |
| 4 | API misuse | Pass | All `cfw_upgraded_*` functions validate the conn is upgraded, live, and not closing; `send` after `close` returns `CFW_ERROR_IO`; hook errors and `CFW_ERROR_IO` from `recv` mark the conn dead; per-conn state via `cfw_upgraded_set_user` |
| 5 | Concurrency/cancellation | Pass | Everything runs on the connection's event-loop thread; no new shared state; `upgrade_closing` suppresses callbacks during drain; `cfw_server_free`/sweep paths reap upgraded conns like any other |
| 6 | DoS resistance | Pass | Idle upgraded conns remain subject to `keep_alive_timeout` (0 disables — documented); `POLL_IN`-only interest means no busy loop; `POLL_OUT` is armed only while output is queued; HUP/ERR delivered once (`upgrade_hup_seen`) to avoid event spam; send buffering bounded by `max_out_size` |
| 7 | Platform portability | Pass | Implementation is POSIX-only inside `#else _WIN32`; Windows stubs return `CFW_ERROR_UNAVAILABLE`/`NULL` per contract |
| 8 | Cryptography/transport | N/A | Upgraded protocols are the application's responsibility (ADR 0034); TLS remains deferred (ADR 0030) |
| 9 | Supply chain/license | Pass | No new dependencies |
| 10 | Documentation/ABI | Pass | ADR 0034 recorded; additive API only (new option fields default to disabled via zeroed options); dispatch.h upgrade note updated; api-stability rules unaffected pre-1.0 |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| U-101 | Medium | Half-close handling: after `recv` reports EOF the conn drops `POLL_IN` interest and auto-closes once queued output drains — an app that defers its close frame to a later callback loses the connection | Documented contract: final bytes must be sent in the same hook invocation that observes EOF (`server.h` recv comment, ADR 0034). Acceptable: WebSocket close frames precede TCP FIN in practice |
| U-102 | Low | An app that ignores `CFW_UPGRADE_EV_HUP`/`ERR` and never reads can spin the event loop until the idle deadline fires | Bounded by `keep_alive_timeout` when configured; documented that apps must close on HUP/ERR. Same class as ignoring handler errors |

## Reviewed and rejected concerns

- **101 without hook / without request Upgrade header:** sent as an
  ordinary response; verified by `server_upgrade_disabled` and
  `server_upgrade_requires_header` tests — the connection remains
  HTTP/1.1 and serves subsequent requests.
- **Pipelined protocol bytes:** moved from `http1_server.pending` to
  `upgrade_pending` before `upgraded` is set; `server_upgrade_pipelined`
  proves a frame sent in the same segment as the handshake is echoed
  intact.
- **Wire ordering:** upgraded sends append to the same `out` queue as
  the 101 head, so app bytes can never overtake the response head.
- **Reentrancy:** the READY invocation happens inside `handle_done`;
  the app may queue/close immediately — `conn->dead` and
  `close_after_write` paths were traced through `service_read`,
  `service_conn`, `update_conn_interest`, and `sweep`.
- **Deadline during drain:** `keep_alive_timeout` doubles as the
  upgraded idle deadline and is refreshed on each delivered event;
  `cfw_server_run`'s drain phase services upgraded conns normally.
