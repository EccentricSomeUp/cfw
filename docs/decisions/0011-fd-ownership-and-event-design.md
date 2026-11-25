<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0011: Descriptor ownership and event-loop abstraction design

- Status: Accepted (design); implementation proceeds incrementally
- Date: 2026-11-25

## Context

Phase 2 requires socket, polling, and file-descriptor abstractions before
any transport code (Phase 3) can exist. The central risks are ownership
ambiguity (double-close, leaked descriptors on error paths) and platform
divergence (POSIX fds vs Windows `SOCKET`/`HANDLE`, `poll` vs `epoll` vs
`kqueue` vs IOCP).

## Decision

### Descriptor ownership

All platform resources are represented internally by an opaque owning
handle type. The first implementation is `cfw_fd_t`, a single-field value
type wrapping a signed descriptor (`intptr_t` so Windows `SOCKET` values
also fit). Rules:

- An fd value is invalid (`<= 0` semantics per platform; the wrapper stores
  `-1` for "empty") unless obtained from a successful creation call.
- Ownership is unique and non-shared: exactly one `cfw_fd_t` owns a live
  descriptor at a time.
- `cfw_fd_close` closes and invalidates; `cfw_fd_release` returns the raw
  descriptor and invalidates the wrapper for ownership transfer (e.g. into
  an event registration that assumes ownership).
- Copying a `cfw_fd_t` by value is permitted but creates a non-owning
  alias only if the original is treated as borrowed; the API therefore
  exposes accessors (`cfw_fd_get`) and requires callers to use release/close
  on the owning instance. Documented misuse (closing the same descriptor
  through two owners) is a contract violation.
- On platforms where `SOCKET` is unsigned, values are stored in the same
  signed field; Windows support will adapt at the platform boundary rather
  than changing the public shape.

### Event-loop abstraction

The event subsystem will be an internal (non-public initially) abstraction
with this shape:

- `cfw_pollset_t`: opaque set of registered descriptors with interest
  flags (read/write/error/hangup) and per-descriptor user tokens.
- Platform backends selected at runtime or compile time: `poll`/`epoll` on
  Linux, `kqueue` on BSD/macOS, IOCP on Windows. Backend choice is an
  internal implementation detail; the public API will expose only the
  pollset contract.
- Events carry `(handle, events_mask)`; edge vs level triggering is fixed
  per backend and documented. Level-triggered semantics are preferred for
  v1 simplicity and correctness under partial reads.
- Timeouts integrate with `cfw_timepoint_t`/`cfw_duration_t` from ADR 0006.

Threading, mutexes, and condition variables are deferred until a subsystem
requires them (Phase 5 server lifecycle); no threading API is added now.

## Alternatives considered

- **Public fd/event API now:** rejected; the contract is not yet exercised
  by real consumers, and premature public ABI would constrain backend
  choices.
- **Refcounted shared fd handles:** rejected; shared ownership invites
  races and complicates cancellation. Unique ownership with explicit
  transfer is simpler to audit.
- **Exposing `struct pollfd` directly:** rejected because it leaks POSIX
  specifics and cannot represent IOCP.
- **Edge-triggered events:** deferred; level-triggered is easier to use
  correctly and can be revisited per-backend later.

## Consequences

- Socket and HTTP layers get a single ownership discipline and a portable
  event contract.
- Windows support requires an adapter at the platform boundary; the public
  shape is already wide enough (`intptr_t`) to hold `SOCKET`.
- API-misuse and concurrency audits must cover double-close, release
  semantics, pollset registration lifetime, and cancellation.
