<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0039: Static file conditional requests and Range support

## Status

Proposed.

## Context

ADR 0036 deferred conditional requests and Range support, noting that
Range needs positioned reads. RFC 9110 defines the full conditional
model (`If-Match`, `If-Unmodified-Since`, `If-None-Match`,
`If-Modified-Since`, `If-Range`, `Range`), and ADR 0038 added the
HTTP-date parser those headers require. Serving `Last-Modified` and
honoring `If-Modified-Since`/`Range` removes the largest remaining
functional gap for static content: cache revalidation and resumable
downloads.

Constraints:

- No per-request allocation (ADR 0036): response header values must be
  formatted somewhere that outlives `cfw_static_serve` — the transport
  serializes the head after dispatch returns. Stack storage dangles;
  the dispatch arena is configuration-lifetime.
- `cfw_static_mount_t` is public and caller-allocated; its layout
  cannot grow without an ABI break.
- Size/mtime must describe the same object the stream reads; a
  stat-then-open sequence leaves a TOCTOU window.
- The slot pool bounds concurrent streams; conditional responses
  (304/412/416) and HEAD should not consume slots.

## Decision

### Filesystem primitives (`cfw/fs.h`)

- `cfw_file_fstat(file, &stat)` returns `size` and `mtime_seconds`
  from the open handle (`fstat`/`_fstat64`), so metadata always
  describes the descriptor being read.
- `cfw_file_seek(file, offset)` repositions to an absolute offset
  (`lseek`/`_lseeki64`, SEEK_SET, INT64_MAX bound).

Both are additive public API.

### Serve flow

The middleware now opens the confined file *before* answering, fstats
the descriptor, evaluates the plan, and only then decides the
response:

1. `If-Unmodified-Since` (valid HTTP-date, `mtime > date`) → 412.
2. `If-Modified-Since` (valid HTTP-date, `mtime <= date`) → 304.
   Malformed or absent dates are ignored.
3. `If-Range` + `Range`: `If-Range` carrying an HTTP-date honors the
   range only when `mtime <= date`; any other value (entity-tags,
   which this mount never emits, or garbage) falls back to 200.
4. `Range`: only the single-range `bytes=` form is supported.
   Satisfiable → 206 with `Content-Range`; well-formed but
   unsatisfiable → 416 with `Content-Range: bytes */<size>`;
   malformed, multi-range, or foreign-unit values are ignored → 200.

Responses carry `Last-Modified` (when the mtime formats into the
four-digit-year domain), `Accept-Ranges: bytes`, and `Content-Type`
on 200/206/304. HEAD evaluates the identical plan and returns the
same status and headers with no body.

### Storage

- Stream slots now hold a private `cfw_static_stream_t`
  (`cfw_file_t` + buffer + `remaining` + `eof`) instead of
  `cfw_file_stream_t`; `remaining` bounds emitted bytes, which is how
  206 bodies are produced. Slot memory is still fixed at init.
- Dynamic header values use a small ring (8 × 192 bytes) carved from
  the tail of the same arena allocation as the slots — reachable via
  `slots + slot_count * slot_stride`, so `cfw_static_mount_t` is
  unchanged. Values only need to survive until head serialization,
  which precedes the next request on the single-threaded server;
  mounts shared across threads already require external
  synchronization for the slot pool.
- Slots are acquired only when a body will stream, so 304/412/416/HEAD
  never occupy one.

### New status codes

206 Partial Content, 412 Precondition Failed, and 416 Range Not
Satisfiable were added to `cfw_http_status_t` with reason strings and
`cfw_http_status_parse` cases.

## Alternatives considered

- **ETag validators (`W/"<mtime>-<size>"`)**: deferred. Generating a
  weak etag is cheap, but honoring `If-None-Match`/`If-Match` and
  `If-Range` entity-tags needs list parsing and weak/strong
  comparison rules; Last-Modified covers the common revalidation
  case. The scratch slot reserves space for a future etag.
- **Multipart/byteranges responses**: rejected for now. RFC 9110
  permits ignoring `Range` when multiple ranges are requested; the
  parser treats comma-separated range sets as ignorable → 200.
- **Per-request arena for header values**: rejected — no per-request
  allocator exists and the dispatch arena is configuration-lifetime
  (ADR 0036); the ring keeps memory fixed and allocation-free.
- **Path-stat for size/mtime**: rejected — fstat on the confined
  descriptor removes the stat-then-open TOCTOU window for free.

## Consequences

- Positive: standards-compliant revalidation (304) and resumable
  downloads (206/416); metadata is race-free; conditional and HEAD
  responses cost no slot; still zero per-request allocation.
- Negative: entity-tag conditionals are ignored (documented
  limitation; subsequently implemented by ADR 0040); single-range
  only; `If-Modified-Since` depends on the
  wall clock for RFC 850 forms (IMF-fixdate, the common case, does
  not).
- Compatibility: additive API (`cfw_file_fstat`, `cfw_file_seek`,
  three status codes); mount layout and slot accounting unchanged.

## Verification plan

- Unit tests: conditional matrix (304/412/stale/malformed), range
  matrix (bounds/open/suffix/clamp/416/ignored forms), If-Range
  gating, HEAD parity, `cfw_file_fstat`/`cfw_file_seek`.
- Sanitizers + shared presets; audit record per
  `docs/security/audit-catalog.md` (parser/protocol focus).
