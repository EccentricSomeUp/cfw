<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0052: kqueue pollset backend

## Status

Accepted.

## Context

The internal pollset abstraction (ADR 0014) has two implemented
backends: `epoll` on Linux/Android and a `poll()` fallback on all other
POSIX. On macOS and the BSDs the `poll()` fallback is O(n) per wait —
every registered descriptor is scanned by the kernel on each call —
which does not scale for a server framework and was recorded as
"`kqueue` planned" in the platform matrix.

kqueue is the native scalable event mechanism on every Apple and BSD
target in the support matrix. Its filter model differs from epoll
(one kevent per fd+filter rather than one per fd), so the backend
needs a per-handle record that merges READ/WRITE filter readiness into
the single `cfw_poll_event_t` the abstraction returns.

## Decision

Add a kqueue backend to `src/cfw/private/pollset.c`, selected on
`__APPLE__`, `__FreeBSD__`, `__OpenBSD__`, `__NetBSD__`, and
`__DragonFly__`. The `poll()` fallback remains for other POSIX.

- One heap-allocated record per registered handle holds
  `{handle, token, want_read, want_write, ready}`; its address is the
  kevent `udata`, giving O(1) event-to-record mapping.
- `add`/`modify`/`remove` translate `CFW_POLL_IN`/`CFW_POLL_OUT` to
  `EVFILT_READ`/`EVFILT_WRITE` registrations applied through
  `kevent()` changelists; modify computes add/delete deltas per
  filter.
- `wait` uses a relative `struct timespec` timeout derived from the
  monotonic `cfw_timepoint_t` deadline (nanosecond precision — finer
  than the epoll/poll millisecond path), retries on `EINTR` with a
  recomputed timeout, and maps `NOTE_READ`→`IN`, `NOTE_WRITE`→`OUT`,
  `NOTE_EOF`/`NOTE_HUP`→`HUP`, `NOTE_ERR`/`EV_ERROR`→`ERR`.
- Readiness from both filters of one handle is merged into a single
  output event, preserving level-triggered semantics.
- The kqueue fd is created close-on-exec via `fcntl`.

The abstraction contract is unchanged: no public API or behavioral
difference is visible to callers.

## Consequences

- macOS/BSD move from O(n) `poll()` to O(1)-per-event kqueue waits.
- The backend cannot be compile-verified on the Linux dev host;
  verification relies on the macOS CI leg plus a stub-header syntax
  check performed during development.
- kqueue fd lifetime follows the same rules as epoll: closing a
  registered descriptor without `cfw_pollset_remove` drops its
  registrations silently (matching epoll auto-removal).
