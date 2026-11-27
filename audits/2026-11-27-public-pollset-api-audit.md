<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: public pollset API promotion (ADR 0069)

- Date: 2026-11-27
- Auditor: Devin (agent)
- Commit: e446ac0
- Category: API misuse/ownership, concurrency, platform behavior,
  memory safety
- Scope: `include/cfw/pollset.h`, `src/cfw/pollset.c` (moved from
  `src/cfw/private/`), consumers `src/cfw/server.c`,
  `src/cfw/http_client.c`, `tests/unit/test_pollset.c`,
  `tests/CMakeLists.txt`, `CMakeLists.txt`, `include/cfw/cfw.h`

## Goals

- Verify the private→public promotion introduces no behavior change
  and that the newly documented contract matches the implementation.
- Verify ownership/threading rules are stated correctly and that
  misuse modes are understood.

## Method

- Line-by-line comparison of `src/cfw/pollset.c` against its previous
  private form: the only change is the include path
  (`platform_detect.h` → `private/platform_detect.h`,
  `pollset.h` → `<cfw/pollset.h>`); zero logic diff.
- Contract verification against implementation: `add` rejects
  duplicate handles and invalid masks; `modify` requires an existing
  registration; `remove` returns `CFW_ERROR_INVALID_ARGUMENT` for
  unregistered handles; `wait` returns `CFW_ERROR_TIMEOUT` with
  `*count == 0` on deadline expiry and treats NULL deadline as
  infinite; ERR/HUP are mapped unconditionally from backend events.
- Audit-catalog checks: API misuse/ownership (header now documents
  borrowed-fd semantics — remove-before-close — and single-threaded
  use), concurrency (no internal locking added; the not-thread-safe
  contract is explicit), platform behavior (Windows/none backend
  still returns `CFW_ERROR_UNAVAILABLE`; `CFW_POLLF_BACKEND` test
  override unchanged), memory safety (opaque `cfw_pollset_t`; all
  allocation remains internal).
- `ctest --preset debug --output-on-failure`: all pass, including
  the dedicated `cfw_pollset_poll` backend-variant suite.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| A-001 | info | include/cfw/pollset.h | Newly public API exposes a sharp edge: closing a registered fd without `cfw_pollset_remove` first is a contract violation (epoll tolerates it via ENOENT/EBADF cleanup, kqueue/poll may report stale handles). Documented, not enforced — consistent with fd-ownership conventions elsewhere in the API. | accepted (documented) |
| A-002 | info | src/cfw/pollset.c | No logic changes in the move; behavior identical to the audited private implementation (ADRs 0011/0014/0052 and prior pollset audits). | accepted |

## Verification

```text
cmake --preset debug        && cmake --build --preset debug
ctest --preset debug --output-on-failure     → 6/6 pass
  (includes cfw_pollset_poll backend-variant suite)
```

## Follow-up

- IOCP backend remains the Windows path (ADR 0011); the public API
  shape does not preclude it.
- Consider a worked example (custom TCP echo via `cfw_net_*` +
  `cfw_pollset_*`) if a second consumer appears.
