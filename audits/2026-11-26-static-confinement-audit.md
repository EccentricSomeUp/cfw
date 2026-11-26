<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: static file confinement (ADR 0037)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Scope: internal `cfw_file_open_confined` primitive
  (`src/cfw/private/fs_confined.h`, `src/cfw/fs.c`), static middleware
  GET/HEAD changes (`src/cfw/static.c`, `include/cfw/static.h`), and
  tests in `tests/unit/test_fs.c` / `tests/unit/test_static.c`
- Method: catalog-driven review per `docs/security/audit-catalog.md`;
  closes accepted gaps S-102/S-105 from
  `2026-11-26-static-files-audit.md`

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; normalized path uses a fixed 4096-byte stack buffer with explicit capacity checks; descriptor chain closes every intermediate fd on success and failure |
| 2 | Integer/bounds | Pass | Path normalization bounds every segment copy; Windows join checks `rootlen`/`norm_len` before copying; no allocation in the request path |
| 3 | Parser/protocol state machine | Pass | Lexical validation still rejects decoded `..`, NUL, backslash, and malformed `%`; confinement adds a second, descriptor-relative layer so encoded traversal cannot reach the filesystem |
| 4 | API misuse | Pass | Internal entry point requires an invalid `cfw_file_t`, rejects NULL/absolute/empty/`..`/backslash paths, and verifies regular-file type with `fstat`; static slots are initialized before open and remain free when open fails |
| 5 | Concurrency/cancellation | Pass | No shared mutable state; each request opens its own descriptor chain on the connection's event-loop thread; stream cleanup still runs via `stream_end` |
| 6 | DoS resistance | Pass | Work is bounded by the 4 KiB path cap and one fd pair per component; HEAD now costs an open/close but remains under the same connection/request limits as GET |
| 7 | Platform portability | Pass with notes | POSIX uses `openat` + `O_NOFOLLOW` + `fstat`; Windows uses lexical validation plus CRT open/fstat as a best-effort fallback while server support is deferred |
| 8 | Cryptography/transport | N/A | No crypto/transport changes |
| 9 | Supply chain/license | Pass | No new dependencies |
| 10 | Documentation/ABI | Pass | ADR 0037 recorded; ADR 0036 limitation note updated; public API unchanged (internal primitive only) |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| C-101 | Medium | Symlinked files/directories inside the document root could escape confinement | Fixed on POSIX: intermediate components use `openat` + `O_NOFOLLOW` + directory `fstat`; final component uses `O_NOFOLLOW` + regular-file `fstat`; regression tests cover file and directory symlinks for GET and HEAD |
| C-102 | Medium | Stat-then-open race allowed a swapped filesystem object between validation and serving | Fixed: the served handle is the same descriptor that was walked and `fstat`-verified; callers never reopen by path |
| C-103 | Low | HEAD previously trusted path metadata and could describe a symlink GET would refuse | Fixed: HEAD performs the same confined open/close before returning headers |
| C-104 | Low | Hard links inside root to sensitive inodes remain servable | Accepted: hard links are indistinguishable from regular files by path; preventing them requires filesystem policy outside the process |
| C-105 | Low | Bind mounts or a symlinked `root` itself can still expose outside data | Accepted: `root` is trusted operator configuration; confinement protects request-controlled suffixes, not the configured root |
| C-106 | Info | Windows cannot provide `openat`/`O_NOFOLLOW` semantics in this codebase | Accepted/deferred: Windows server support is already deferred; the internal primitive performs lexical validation and regular-file checks there |

## Verified behaviors (tests)

- `fs_file_open_confined`: rejects NULL/empty/absolute/`..`/backslash
  paths, opens a regular file relative to root, reads it, and rejects a
  directory final component on POSIX.
- `static_symlink_confinement`: GET and HEAD for a symlinked file return
  404; GET through a symlinked directory component returns 404; a failed
  confined open does not leak the stream slot.
- Existing static tests continue to cover normal files, directory index
  resolution, MIME selection, HEAD, traversal rejection, prefix
  boundaries, and slot exhaustion.

## Verification

```text
cmake --build --preset debug
./build/debug/tests/cfw_unit_tests --filter fs
./build/debug/tests/cfw_unit_tests --filter static
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure   # 5/5 pass
cmake --preset shared && cmake --build --preset shared
ctest --preset shared --output-on-failure       # 6/6 pass
```
