# ADR 0032: Peer address exposure to dispatch

## Status

Accepted.

## Context

Handlers and middleware had no way to observe which peer a request came
from. IP-based access control, rate limiting keyed on client identity,
and audit logging all require the transport peer address. `cfw_net_accept`
already captures the peer `cfw_sockaddr_t`, but the server discarded it
and `cfw_dispatch_request` had no channel for transport metadata.

## Decision

Expose the peer address as formatted text on the request view:

- `cfw_request_t` gains a `remote_addr` `cfw_strview_t` field (appended
  at the end of the struct, preserving field order for existing members).
  It holds `"ip:port"` for IPv4 and `"[v6]:port"` for IPv6, valid only
  for the duration of the dispatch call; it is empty when the transport
  does not supply peer information.
- `cfw_dispatch_request_ex` is added as an extended variant taking an
  optional `const cfw_strview_t *remote_addr`. The existing
  `cfw_dispatch_request` delegates with `NULL`, preserving ABI.
- The server formats the peer once at accept time into a fixed 72-byte
  conn buffer (`INET6_ADDRSTRLEN` + port + brackets fits with margin).
  Formatting failure is non-fatal and leaves `remote_addr` empty.

Formatted text was chosen over exposing `cfw_sockaddr_t` because the
latter is platform-specific (`sockaddr_storage`) and private; a string
view is sufficient for the common allow/deny, logging, and keying use
cases and keeps the public API platform-neutral.

## Consequences

- Handlers can implement IP allowlists and per-client logging without
  new APIs.
- `remote_addr` is not a stable identifier across connections (NAT,
  ephemeral ports); it must not be used as an authentication factor.
- Proxies: unless a trusted `Forwarded`/`X-Forwarded-For` convention is
  implemented by the application, `remote_addr` is the direct peer —
  deployments behind a reverse proxy see the proxy address.
