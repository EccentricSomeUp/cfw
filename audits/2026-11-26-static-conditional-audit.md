<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: static conditional requests and Range support (ADR 0039)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Scope: `cfw_file_fstat`/`cfw_file_seek` (`include/cfw/fs.h`,
  `src/cfw/fs.c`), conditional/Range evaluation and ranged streaming
  in `src/cfw/static.c`, new status codes in `include/cfw/http.h` /
  `src/cfw/http.c`, tests in `tests/unit/test_fs.c` and
  `tests/unit/test_static.c`
- Method: catalog-driven review per `docs/security/audit-catalog.md`;
  parser/protocol and memory-safety focus

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; all new parsing is cursor/index based over `cfw_strview_t` with explicit bounds; scratch ring is fixed-size arena storage; slot layout is private and stride-aligned |
| 2 | Integer/bounds | Pass | Range numbers use checked `uint64_t` accumulation (overflow → header ignored); range arithmetic is clamped to `fstat.size`; `remaining` is `uint64_t` and read sizes are capped by `buffer_size`; seek offsets bounded to `INT64_MAX` |
| 3 | Parser/protocol state machine | Pass with notes | RFC 9110 precedence order followed for supported validators (IUS → IMS → If-Range → Range); entity-tag conditionals (`If-Match`, `If-None-Match`, `If-Range` entity-tags) are documented as ignored; multi-range requests fall back to 200 per RFC-permitted ignoring; weekday names in dates remain unverified by design (ADR 0038) |
| 4 | API misuse | Pass | NULL/invalid handles rejected; `cfw_file_seek` validates handle and offset; mount/serve argument validation unchanged; header-add failure maps to 500 with file closed |
| 5 | Concurrency/cancellation | Pass with notes | Server is single-threaded per instance; scratch ring reuse is safe because head serialization precedes the next dispatch. Mounts shared across threads already require external synchronization for the slot pool — documented in ADR 0039. `stream_end` releases the fd on completion and abort |
| 6 | DoS resistance | Pass | Conditional/HEAD responses consume no stream slot; 304/412/416 cost one open+fstat+close; range parsing is O(header length); scratch ring is fixed 8×192B |
| 7 | Platform portability | Pass | `fstat`/`lseek` mapped per platform (`_fstat64`/`_lseeki64` on Windows, `_FILE_OFFSET_BITS=64` on POSIX); confined-open confinement unchanged |
| 8 | Cryptography/transport | N/A | No crypto; transport framing unchanged (206 streams use existing chunked/close-delimited paths) |
| 9 | Supply chain/license | Pass | No new dependencies |
| 10 | Documentation/ABI | Pass | ADR 0039 records decisions and limitations; additive API only; `cfw_static_mount_t` layout unchanged (scratch carved from existing arena allocation) |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| R-101 | Low | Size/mtime now come from `fstat` on the confined fd, eliminating the previous stat-then-open TOCTOU window for metadata | Improvement; recorded |
| R-102 | Low | Entity-tag conditionals are ignored rather than evaluated | Accepted and documented (ADR 0039); weak-etag generation deferred to a future milestone; ignoring yields correct-but-unoptimized responses (200 instead of 304/412) |
| R-103 | Info | `If-Modified-Since` RFC 850 forms resolve against the wall clock | Accepted; IMF-fixdate (the generated form) is absolute |
| R-104 | Info | Multi-range requests receive 200 instead of multipart/byteranges | Accepted per RFC 9110 (server MAY ignore Range); documented |

## Verified behaviors (tests)

- 304 for satisfied `If-Modified-Since` on GET and HEAD; stale and
  malformed dates ignored → 200.
- 412 for failed `If-Unmodified-Since`; success at `Last-Modified`.
- 206 with correct `Content-Range` and body for explicit, open-ended,
  suffix, and clamped ranges; HEAD parity with no stream.
- 416 with `Content-Range: bytes */<size>` for out-of-range, `-0`,
  and empty-file ranges.
- Ignored Range forms (invalid spec, multi-range, foreign unit,
  malformed) → 200 full body.
- `If-Range` date gating (honor/stale) and entity-tag fallback → 200;
  `If-Modified-Since` precedence over `Range` → 304.
- `cfw_file_fstat`/`cfw_file_seek` success and error paths.

## Verification

```text
cmake --build --preset debug
ctest --preset debug --output-on-failure
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure
cmake --preset shared && cmake --build --preset shared
ctest --preset shared --output-on-failure
```
