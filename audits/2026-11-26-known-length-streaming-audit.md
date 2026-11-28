<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: known-length streaming responses (ADR 0041)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: 29b15da
- Scope: `cfw_response_t.stream_size` contract (`include/cfw/dispatch.h`),
  head formatting (`src/cfw/dispatch.c`), stream install/framing/pump
  enforcement (`src/cfw/server.c`), static middleware declaration
  (`src/cfw/static.c`), tests in `tests/unit/test_server.c` and
  `tests/unit/test_static.c`
- Method: catalog-driven review per `docs/security/audit-catalog.md`;
  protocol-state-machine and memory-safety focus

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; `stream_sent`/`stream_size` are u64 counters; piece cap uses a guarded u64→size_t narrowing only when `remaining < n ≤ 16384`; no new allocations |
| 2 | Integer/bounds | Pass | `stream_sent` only ever increments by emitted `n` and is capped at `stream_size`, so `stream_size - stream_sent` cannot underflow; `UINT64_MAX` sentinel cannot collide with a real file size (off_t ≤ INT64_MAX) |
| 3 | Parser/protocol state machine | Pass | Three framings are mutually exclusive (chunked / close-delimited / known-length identity); `will_close` updated so identity streams keep alive on all versions; early-eof → dead conn prevents CL/body mismatch; over-production truncated at exactly CL bytes |
| 4 | API misuse | Pass | `stream_size` ignored for non-stream body responses and defaults to UNKNOWN via dispatch reset; memset-zero responses behave as size-0 only where a body is impossible anyway; field appended last per api-stability |
| 5 | Concurrency/cancellation | Pass | Per-connection state only; server remains single-threaded; `stream_end` still runs exactly once on completion, abort, and shutdown |
| 6 | DoS resistance | Pass | Known-length streams cannot hang a client waiting for bytes that never come (early eof closes); truncation bounds work per response; no new unbounded state |
| 7 | Platform portability | Pass | Pure counter/framing logic; no platform dependencies |
| 8 | Cryptography/transport | N/A | Framing correctness only; no crypto |
| 9 | Supply chain/license | Pass | No new dependencies |
| 10 | Documentation/ABI | Pass | ADR 0041 records contract, enforcement, and truncation semantics; additive end-appended field permitted pre-1.0 |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| U-101 | Low | A stream signaling eof before `stream_size` bytes now closes the connection mid-response | Intended; the alternative (emit fewer bytes than declared CL) corrupts framing for the client and any pipelined requests |
| U-102 | Info | Over-long streams are silently truncated at the declared length | Accepted; matches Content-Length semantics and sendfile-style behavior; documented in ADR 0041 |
| U-103 | Info | `cfw_response_t` grew by `sizeof(uint64_t)` | Accepted; additive end-appended field, permitted while version is 0.x per docs/api-stability.md |

## Verified behaviors (tests)

- Static: `stream_size` = file size on GET 200 and HEAD, range length
  on 206.
- Server wire test: `Content-Length: 23` with no `Transfer-Encoding`,
  contiguous identity body, keep-alive second request.
- Over-long source (declared 10 of 23): exactly 10 bytes emitted,
  framing intact, subsequent request works.
- Short stream (declared 30 of 23): CL:30 head + 23 bytes, then the
  server closes the connection.
- Existing chunked (unknown-size) and HTTP/1.0 close-delimited stream
  tests unchanged and passing.

## Verification

```text
cmake --build --preset debug
ctest --preset debug --output-on-failure
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure
cmake --preset shared && cmake --build --preset shared
ctest --preset shared --output-on-failure
```
