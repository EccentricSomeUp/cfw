<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: SO_REUSEPORT multi-instance serving (ADR 0056)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: c4a71e7
- Scope: `reuse_port` option on `cfw_server_options_t`, the
  `SO_REUSEPORT` setsockopt in `cfw_server_init`, and the
  `CFW_HAVE_SO_REUSEPORT` configure probe
- Method: catalog-driven review per `docs/security/audit-catalog.md`

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | No new allocation or lifetime changes; setsockopt uses a stack int |
| 2 | Integer/bounds | N/A | No arithmetic added |
| 3 | Parser/protocol | N/A | No parser changes |
| 4 | API misuse | Pass | Option documented with the all-instances-must-opt-in requirement; failure is loud (`UNAVAILABLE`/`IO`) rather than silent degradation; verified by tests on both supported and unsupported builds |
| 5 | Concurrency/cancellation | Pass | Each `cfw_server_t` remains fully independent; the kernel distributes connections, no shared mutable state is introduced; dispatch stays read-only during requests as already documented |
| 6 | DoS resistance | Pass | No new unbounded resource; each instance keeps its own `max_connections`/`max_out_size`/timeouts; an attacker can only reach instances that opted in |
| 7 | Platform portability | Pass with note | `SO_REUSEPORT` availability is compile-probed per ADR 0053; platforms lacking it (e.g. OpenBSD) get `CFW_ERROR_UNAVAILABLE` at init; Linux/macOS/BSD supported; Windows unaffected (server stub) |
| 8 | Cryptography/transport | N/A | No crypto changes |
| 9 | Supply chain/license | Pass | No new dependencies; CMake probe only |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| R-201 | Info | Kernel accept distribution may be uneven across instances | Documented in ADR 0056; acceptable for v1, revisit if affinity needed |
| R-202 | Low | A mixed group (some sockets with, some without SO_REUSEPORT) fails at bind rather than at a clearer earlier point | Inherent to the kernel API; documented on the option |

## Reviewed and rejected concerns

- SO_REUSEPORT set after bind: impossible in this code path — the
  setsockopt runs between `cfw_net_socket` and `cfw_net_bind`.
- Option on non-listener use: only `cfw_server_init` consumes it;
  client sockets are untouched.
