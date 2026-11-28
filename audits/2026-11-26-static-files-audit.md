<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: Static file serving (ADR 0036)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: 098cd36
- Scope: `include/cfw/static.h` + `src/cfw/static.c` — mount init,
  URL-prefix middleware, percent-decode + path confinement, directory
  → `index.html` resolution, MIME table, bounded stream-slot pool;
  tests in `tests/unit/test_static.c`
- Method: catalog-driven review per `docs/security/audit-catalog.md`

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; decode/path buffers are fixed 4096-byte stack arrays with explicit capacity checks; slot layout is `cfw_file_stream_t` + caller `buffer_size` with stride aligned to `2*sizeof(void*)`; slots found via `!cfw_file_valid`, released by `cfw_file_stream_end` |
| 2 | Integer/bounds | Pass | `max_concurrent * stride` overflow-checked before arena alloc; decode rejects output overflow; root+suffix join bounds-checked against `fspath`; `/index.html` append bounds-checked |
| 3 | Parser/protocol state machine | Pass | Percent-decoder rejects malformed `%`, truncated escapes, NUL (`%00`), and backslash — including percent-encoded backslash (`%5C`), caught during testing; `..` segment scan runs on the *decoded* path so `%2e%2e`, mixed-case, and separator-encoded traversal all collapse to the same check |
| 4 | API misuse | Pass | Mount init validates all options (prefix must start `/`, no trailing `/` except root `/`); unready/NULL mount passes through; non-GET/HEAD and non-prefix paths call `next()` |
| 5 | Concurrency/cancellation | Pass | Slots are per-mount and claimed only while a file handle is valid; `stream_end` frees on completion AND on mid-stream connection teardown (verified by `server_stream_end_on_close` for the underlying hook) |
| 6 | DoS resistance | Pass | Fixed slot pool: `max_concurrent` concurrent streams max, excess → 503; no per-request allocation; decode/path work bounded by 4 KiB caps; directory resolution does at most two `stat` calls per request |
| 7 | Platform portability | Pass with notes | Uses `cfw_file_*`/`cfw_file_is_directory`/`cfw_file_size` portability boundary; backslash rejection covers Windows separators; module itself is platform-neutral. POSIX-only tests (mkdir); Windows path untested — matrix pending |
| 8 | Cryptography/transport | N/A | No crypto/transport changes |
| 9 | Supply chain/license | Pass | No new dependencies |
| 10 | Documentation/ABI | Pass | ADR 0036 records design + deferred items; header documents root/prefix/arena lifetimes and slot memory formula; additive API only |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| S-101 | Medium | Percent-encoded backslash (`%5C`) initially decoded to `\` without rejection — a path-separator on Windows | Fixed during testing — decoder now rejects `\` post-decode; regression covered by `static_errors_and_passthrough` |
| S-102 | Low | Symlinks inside `root` pointing outside it are followed | Accepted for v1 — documented in ADR 0036 and `static.h`; OS-level confinement (`openat2`/`RESOLVE_BENEETH`) deferred as platform-specific hardening |
| S-103 | Low | `root` itself is not canonicalized; a `root` containing `..` or symlinks is trusted config | Accepted — `root` is operator configuration, not request input |
| S-104 | Info | No Range/If-Modified-Since/ETag; HEAD returns headers without Content-Length | Accepted — deferred per ADR 0036; chunked streaming and header-only HEAD are correct HTTP |
| S-105 | Info | TOCTOU between `stat` and `open` can serve a swapped file | Accepted — same class as F-103 in the fs audit; confinement is path-based, not handle-based |

## Verified behaviors (tests)

- Mount init validation matrix (NULLs, bad prefix, zero counts).
- GET file → 200 + correct Content-Type + byte-exact streamed body.
- Directory → `index.html`; nested path; CSS/HTML/octet-stream MIME;
  uppercase extension; extensionless fallback.
- HEAD → headers only, no stream installed.
- 404 missing file; 403 directory without index; 400 malformed `%`,
  `%00`, `%5C`; 404 for `..`, `%2e%2e`, `%2f`-encoded traversal.
- Prefix boundary (`/staticx` ≠ `/static`), non-prefix and POST
  pass-through to `next()`.
- Slot exhaustion → 503; slot freed by `stream_end` → subsequent 200.

## Verification

```text
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure   # 6/6 pass
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure  # 5/5 pass
```
