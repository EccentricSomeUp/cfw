<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: kqueue pollset backend

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: 1e8d2d5
- Category: audit catalog §7 (platform-specific behavior) and §3
  (parser/protocol state machines — event-state handling)
- Scope: `src/cfw/private/pollset.c` kqueue backend section,
  backend-selection preprocessor logic

## Goals

- Verify the kqueue backend preserves the pollset contract:
  level-triggered readiness, one merged event per handle, deadline
  semantics, and identical add/modify/remove validation.
- Verify no resource leaks on every failure path (kqueue fd, record
  allocations, partial filter registration).
- Verify the new `#elif` backend-selection chain cannot shadow or be
  shadowed by the epoll/poll/none branches, including the
  `CFW_POLLF_FORCE_POLL` test override.

## Method

- Line-by-line review of the kqueue section against BSD `kqueue(2)`
  semantics: `EV_ADD`/`EV_DELETE` changelists applied with
  `nevents == 0`, `udata`-keyed records, `NOTE_*`/`EV_ERROR` mapping,
  relative `timespec` timeouts with `EINTR` recomputation.
- Syntax/type check of the kqueue section compiled standalone against
  a purpose-built stub `sys/event.h` with `-std=c11 -Wall -Wextra
  -Werror` (Linux has no kqueue headers). This caught a real defect:
  `cfw_kqueue_to_cfw` initially typed the filter parameter `uint16_t`,
  which can never equal the negative `EVFILT_*` constants — corrected
  to `int16_t`.
- Linux debug build + full unit suite confirm the preprocessor chain
  still selects epoll/poll correctly and `CFW_POLLF_FORCE_POLL` still
  reaches the poll backend.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| A-001 | medium | pollset.c `cfw_kqueue_to_cfw` | `uint16_t filter` parameter made `EVFILT_READ`/`EVFILT_WRITE` (-1/-2) comparisons always false — every readiness event would have mapped to 0 | fixed before commit (`int16_t`, caught by stub compile) |
| A-002 | low | pollset.c `cfw_kqueue_apply` rollback | `cfw_kqueue_register` failure path re-issues `EV_DELETE` for the whole changelist; partial application is possible in theory | accepted: `kevent` changelist failures are limited to bad-fd/OOM classes already excluded by handle validation |
| A-003 | info | pollset.c wait | Merged per-handle events cannot exceed `nready <= max_events`; `record->ready` is cleared on every emit | verified by review |
| A-004 | info | pollset.c | Closing a registered fd without `cfw_pollset_remove` silently drops its kevents — same semantics as epoll auto-removal | accepted: documented in ADR 0052 |
| A-005 | info | platform matrix | macOS row previously claimed "not verified on CI" while `ci.yml` runs `macos-latest`; matrix updated to reflect kqueue + CI status | fixed |

## Verification

```text
# stub-header compile of the kqueue section: clean under -Werror
cc -std=c11 -Wall -Wextra -Werror -I/tmp/kqstub -c kqueue_harness.c

cmake --preset debug && cmake --build --preset debug   # 0 warnings
./build/debug/tests/cfw_unit_tests --filter pollset    # 4/4 pass
```

## Follow-up

- **Blocking verification gap**: the kqueue backend has never been
  compiled against real `<sys/event.h>` or executed. The macOS CI leg
  (`macos-latest`, all four presets) is the first real check — it must
  pass before this is considered verified. If BSD header differences
  surface (e.g. `NOTE_*` availability — all uses are `#ifdef`-guarded
  except `NOTE_READ`/`NOTE_WRITE`/`NOTE_EOF`, which are universal),
  patch and re-run.
- Consider a `CFW_POLLF_FORCE_KQUEUE`-style compile probe in CI if a
  BSD runner is ever added.
