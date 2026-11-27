<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0069: Public pollset (readiness event) API

- Status: Accepted
- Date: 2026-11-27

## Context

ADR 0014 introduced the pollset as an internal abstraction and
deferred making it public "until the contract has an external
consumer". ADR 0067 then promoted the socket layer (`cfw/net.h`) to
public — but the promotion exposed a gap: `cfw_net_*` sockets are
nonblocking, and without a public wait primitive an application
cannot efficiently drive them. The HTTP client (ADR 0068) works
around this by keeping its pollset internal, which is correct for a
self-contained one-shot call but leaves custom-protocol applications
(forward proxies, protocol bridges, bespoke TCP services built on
`cfw_net_*`) with only busy-polling or blocking-I/O options.

The pollset contract has been stable and heavily exercised since
Phase 2: three real backends (epoll, kqueue, poll fallback), a
none-backend stub convention, level-triggered semantics, per-
registration tokens, and deadline-bounded waits shared with
`cfw_server_t`, `cfw_http1_conn_t`, `cfw_tls_conn_t`, and the HTTP
client.

## Decision

Promote the pollset to a public API in `include/cfw/pollset.h`,
moving the implementation from `src/cfw/private/pollset.[ch]` to
`src/cfw/pollset.c` (same layout as the ADR 0067 net promotion):

- The six functions (`cfw_pollset_create`/`destroy`/`add`/`modify`/
  `remove`/`wait`) gain `CFW_API` export and full public-contract
  documentation: level-triggered semantics, `handle` =
  `cfw_fd_get()`/`cfw_tls_conn_fd()` value, per-registration `token`,
  ERR/HUP always reported, NULL deadline = infinite wait,
  `CFW_ERROR_TIMEOUT` on deadline expiry.
- Ownership rules are documented explicitly: the pollset owns only
  its registrations — descriptors are borrowed and must be removed
  (or the set destroyed) before close. A pollset is not thread-safe;
  one per thread or external synchronization.
- No behavior changes: backend selection, the `CFW_POLLF_BACKEND`
  test override, and all error mappings are unchanged. Windows and
  backend-less platforms keep returning `CFW_ERROR_UNAVAILABLE`.

## Alternatives considered

- **Leave internal; document busy-poll pattern:** rejected —
  nonblocking sockets without a wait primitive are not a usable
  public contract; every consumer would reinvent polling.
- **Fold wait into `cfw_net_*` (e.g. `cfw_net_poll`)**: rejected —
  readiness is inherently set-oriented (multiple fds, tokens,
  deadlines); a per-fd poll wrapper would be a poorer API and a
  second implementation of the same backends.
- **Public edge-triggered mode:** rejected per ADR 0011 —
  level-triggered is easier to use correctly; edge mode can be
  layered by consumers if ever needed.

## Consequences

- `cfw_net.h` + `cfw/pollset.h` together form a complete public
  nonblocking I/O toolkit: sockets, addresses, TLS transport
  (`cfw/tls.h`), and readiness waiting.
- `cfw_pollset_t` becomes ABI/API surface under the stability rules;
  the opaque type keeps backend details private.
- Security audit must cover the documented ownership/threading
  contract (fd closed while registered, cross-thread wait vs.
  modify races) since misuse is now possible from public code.
