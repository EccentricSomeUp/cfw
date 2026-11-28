<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: File-read primitives and stream adapter (`cfw/fs.h`)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: eaddf92..901e405
- Scope: `cfw_file_t` (`cfw_file_open`/`cfw_file_read`/`cfw_file_close`),
  `cfw_file_stream_t` (`cfw_file_stream_init`/`cfw_file_stream_read`/
  `cfw_file_stream_end`/`cfw_file_stream_close`) in
  `include/cfw/fs.h` + `src/cfw/fs.c`; the `cfw_response_t.stream_end`
  cleanup hook (`cfw_response_stream_end_fn`) in `dispatch.h`/`server.c`;
  tests in `tests/unit/test_fs.c` / `tests/unit/test_server.c`
  (`server_file_stream_response`, `server_stream_end_on_close`)
- Method: catalog-driven review per `docs/security/audit-catalog.md`

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; `cfw_file_stream_read` emits spans only over caller-owned scratch; `cfw_file_stream_init` memsets state before opening so partial init cannot leak a handle; `cfw_file_close` invalidates the handle on both success and failure paths |
| 2 | Integer/bounds | Pass | `cfw_file_read` rejects `capacity == 0` and NULL args; Windows `_read` takes `unsigned int`, so `capacity > UINT32_MAX` is rejected up front; stream chunks are bounded by caller `buffer_size` |
| 3 | Parser/protocol state machine | Pass | Stream honors the `cfw_response_stream_fn` contract: non-EOF calls return non-empty chunks, EOF is sticky, read errors abort the response via `CFW_ERROR_IO` |
| 4 | API misuse | Pass | NULL/invalid-handle/double-open/zero-capacity calls rejected; close is idempotent; `cfw_file_open` refuses an already-valid wrapper so a live handle cannot be silently overwritten |
| 5 | Concurrency/cancellation | Pass | Handles are per-call state with no shared mutable data; same single-threaded event-loop contract as the rest of the server |
| 6 | DoS resistance | Pass with notes | Read size is bounded by the caller's scratch buffer; a stalled or huge file only consumes one buffer per pull and backpressure is inherited from `max_out_size`. No path canonicalization is performed — serving untrusted paths remains the caller's duty (documented in `fs.h`) |
| 7 | Platform portability | Pass with notes | POSIX `open`/`read`/`close` and Windows CRT `_open`/`_read`/`_close` behind macros; `_O_BINARY` prevents CRLF translation on Windows; `intptr_t` handle storage matches `cfw_fd_t` conventions. Not yet exercised on Windows — platform-matrix verification pending |
| 8 | Cryptography/transport | N/A | No cryptographic or transport changes |
| 9 | Supply chain/license | Pass | No new dependencies |
| 10 | Documentation/ABI | Pass | Header documents ownership rules, EOF semantics, and the borrowed-span lifetime of emitted chunks; additive API only |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| F-101 | Info | `cfw_file_read` does not retry on `EINTR` | Accepted — the framework's socket paths handle EINTR at the poll layer; a short read or error surfaces to the caller, and the stream adapter propagates it as a response abort. Revisit if real workloads show spurious failures |
| F-102 | Info | No `pread`/seek support yet; Range requests and parallel readers are out of scope | Accepted — sequential read covers the streaming-response use case; additive seek API can land with the static-file module (ADR pending) |
| F-103 | Low | `stream_end` fires when the terminal chunk is *queued*, not flushed; a conn dying between queue and flush still runs the hook via `close_conn` | Verified — `close_conn` invokes `stream_end` for any conn with an active stream, and the completion path clears `stream_fn`/`stream_end` together so the hook runs exactly once (`server_stream_end_on_close` covers the abort path) |
| F-103 | Low | TOCTOU between `cfw_file_size(path)` and `cfw_file_open(path)` can serve a different file than sized | Accepted for now — documented that path handling is the caller's responsibility; the future static module must confine paths (root-prefix + canonicalization) before serving |

## Verified behaviors (tests)

- `fs_file_open_read_close`: chunked reads reassemble the payload, EOF
  is sticky at 0 bytes, close is idempotent.
- `fs_file_open_errors`: NULL/double-open/missing-file rejection,
  invalid-handle reads rejected, close on NULL/invalid tolerated.
- `fs_file_stream`: pull-until-eof reassembly with a 7-byte scratch
  buffer, sticky EOF, empty-file immediate EOF, init error matrix.
- `server_file_stream_response`: real file served over HTTP/1.1 as
  `Transfer-Encoding: chunked`; dechunked body byte-equals the file;
  `stream_end` closes the file handle; keep-alive survives.
- `server_stream_end_on_close`: abrupt client disconnect mid-stream
  still invokes `stream_end` exactly once.

## Verification

```text
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure   # 6/6 pass
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure  # 5/5 pass
```
