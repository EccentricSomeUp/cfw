<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0014: Internal pollset implementation

- Status: Accepted
- Date: 2026-11-26

## Context

ADR 0011 designed the event-set contract. Phase 2 needs a working
implementation before socket and HTTP layers can be built. The first
consumer is internal, so the API lives under `src/cfw/private/` and is not
installed or exported.

## Decision

Add `src/cfw/private/pollset.h` and `src/cfw/private/pollset.c`:

- `cfw_pollset_create` / `cfw_pollset_destroy`: opaque set lifetime.
- `cfw_pollset_add` / `cfw_pollset_modify` / `cfw_pollset_remove`:
  register, change, or drop `(handle, events, token)` triples. Adding an
  already-registered handle is `CFW_ERROR_INVALID_ARGUMENT`.
- `cfw_pollset_wait`: blocks until at least one event or the deadline;
  returns `CFW_ERROR_TIMEOUT` with `count == 0` on expiry, `CFW_OK` with
  populated `cfw_poll_event_t` entries otherwise. `deadline == NULL` blocks
  indefinitely.

Event flags: `CFW_POLL_IN`, `CFW_POLL_OUT`, `CFW_POLL_ERR`, `CFW_POLL_HUP`.
Level-triggered semantics. `ERR`/`HUP` are always surfaced when the backend
reports them, even if not requested.

Backends, selected at compile time:

- Linux/Android: `epoll` (`epoll_create1(EPOLL_CLOEXEC)`). Each registration
  owns a heap record `{handle, token}` referenced by `epoll_event.data.ptr`;
  records are freed on modify/remove/destroy. Re-adding an existing fd is
  rejected up front via a slot table.
- Other POSIX (macOS, *BSD): `poll(2)` fallback with a parallel
  `pollfd`/entry array. This is correct but not scalable to very large fd
  counts; `kqueue` is the planned optimization.
- Windows: all entry points return `CFW_ERROR_UNAVAILABLE` until the IOCP
  backend lands.

Timeout handling converts the monotonic deadline to whole milliseconds
(rounding up, clamped to `INT32_MAX`), retries on `EINTR` while recomputing
the remaining time, and maps `poll`/`epoll_wait` errors to
`CFW_ERROR_IO`/`CFW_ERROR_TIMEOUT`/`CFW_ERROR_NOMEM`.

Tests compile `pollset.c` directly into the unit-test binary because the
symbols are intentionally hidden from the shared library.

## Alternatives considered

- **Public API now:** deferred; the contract has no external consumer yet
  and the backend matrix is incomplete (no kqueue/IOCP).
- **`select(2)` fallback:** rejected; fd-count limits and O(n) scanning make
  it strictly worse than `poll` for the same effort.
- **Edge-triggered mode:** deferred per ADR 0011; level-triggered is easier
  to use correctly for stream sockets.
- **Token table indexed by fd:** rejected for epoll; the `data.ptr` record
  approach avoids a lookup on the hot wait path.

## Consequences

- Socket code can register fds and wait with deadlines using only portable
  types.
- The epoll backend allocates one small record per registration; modify and
  remove free/replace it, so registration churn has bounded cost.
- Concurrency audit must cover record lifetime vs in-flight `epoll_wait`
  results (records are only freed on explicit modify/remove/destroy, never
  from the wait path).
- kqueue and IOCP backends will be added behind the same internal contract.
