<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0036: Static file serving

## Status

Proposed.

## Context

The framework can stream response bodies (`cfw_response_stream_fn`,
ADR 0031) and now has portable file reads (`cfw_file_t`,
`cfw_file_stream_t`, `cfw_file_stream_end`) plus the `stream_end`
cleanup hook. What is missing is the policy layer: mapping a URL path
to a confined filesystem path, picking a Content-Type, and managing
the per-request stream state that `cfw_file_stream_t` requires.

Constraints:

- The router (ADR 0026) has no wildcard/prefix patterns — only literal
  and `:param` segments — so a catch-all file route is not expressible.
- Dispatch middleware (ADR 0027) runs before routing and may
  short-circuit by returning `CFW_OK` without calling `next()`; this
  is the natural interception point for a URL prefix.
- Per-request state must outlive `cfw_dispatch_request` because the
  server pulls stream chunks during later `cfw_server_step` calls.
  There is no per-request allocator; the dispatch arena is
  configuration-lifetime.
- Path handling is the caller's responsibility today (`fs.h`); a
  static module that serves untrusted URLs must confine paths itself.
- The HTTP parser preserves percent-encoding in `req->path`
  (route.h documents this), so `%2e%2e`-style traversal must be
  handled by decoding-then-validating, not assumed absent.

## Decision

Add `cfw/static.h` + `src/cfw/static.c`: a mount object installed as
dispatch middleware.

```c
typedef struct cfw_static_options {
    const char *root;          /* document root, trusted config */
    const char *url_prefix;    /* e.g. "/static" or "/" */
    size_t max_concurrent;     /* stream slots */
    size_t buffer_size;        /* per-slot read buffer */
    cfw_arena_t *arena;        /* slot storage; must outlive mount */
} cfw_static_options_t;

typedef struct cfw_static_mount cfw_static_mount_t;

CFW_API cfw_status_t cfw_static_mount_init(cfw_static_mount_t *mount,
                                           const cfw_static_options_t *o);
CFW_API cfw_status_t cfw_static_serve(cfw_request_t *req,
                                      cfw_response_t *res,
                                      cfw_dispatch_next_t next,
                                      cfw_dispatch_frame_t *frame,
                                      void *user); /* == mount */
```

Apps register it once: `cfw_dispatch_use(&dispatch, cfw_static_serve,
&mount)`. Requests whose path does not match `url_prefix`, or whose
method is not GET/HEAD, pass through to `next()`.

### Request flow (GET)

1. Match `req->path` against `url_prefix`; extract the remainder.
2. Percent-decode the remainder into a bounded stack buffer
   (4096 bytes). Reject malformed `%`, `%00`, and backslash with 400.
3. Split on `/`; reject any `..` segment and empty-result with 404.
   Literal `.` segments are skipped. Percent-encoded separators are
   decoded first, so `%2e%2e%2f` is caught by the same check.
4. Join `root` + decoded path. If the result is a directory, append
   `/index.html`; if still a directory or not a regular file → 403/404.
5. Acquire a stream slot (see below); on exhaustion → 503.
6. `cfw_file_stream_init` opens the file; set `res->status = 200`,
   `Content-Type` from a built-in extension table (default
   `application/octet-stream`), `res->stream = cfw_file_stream_read`,
   `res->stream_user = res->stream_end = slot` via
   `cfw_file_stream_end`, and return `CFW_OK` without `next()`.

### HEAD

Same decode/validate/stat path, but no slot is acquired and no file is
opened: only status + `Content-Type` are set. The server suppresses
body framing for HEAD anyway; `Content-Length` remains absent (the
server owns that header, ADR 0027/0031).

### Stream slots

`max_concurrent` slots are allocated once from the mount arena at init.
Each slot is `cfw_file_stream_t` followed by `buffer_size` scratch
bytes; slots are found by `!cfw_file_valid(&slot->stream.file)`, so
`cfw_file_stream_end` (which closes the file) is also the release
mechanism — no separate bookkeeping. A slot pointer is both the
`cfw_file_stream_t *` (first member) and the slot base, so
`stream_user` serves `cfw_file_stream_read` and `cfw_static`'s own end
hook uniformly.

Memory is therefore fixed at `max_concurrent * (sizeof(stream) +
buffer_size)` for the mount's lifetime: deterministic, allocation-free
per request, and DoS-bounded (a slot flood yields 503, not growth).

### Path confinement

- `root` is trusted configuration; it is not canonicalized (documented).
- The *suffix* is fully decoded then validated: no `..` segment, no
  NUL, no `\`, no empty path, length-capped. This blocks `..`,
  `%2e%2e`, mixed-encoding, and Windows-separator traversal.
- Known limitation (superseded by ADR 0037): symlinks inside `root`
  that point outside it are no longer followed on POSIX; file access
  uses confined descriptor-relative opens and `fstat` regular-file
  verification. Windows remains best-effort while server support is
  deferred.

### MIME types

A small static table (html, css, js, mjs, json, map, txt, xml, svg,
png, jpg, jpeg, gif, webp, ico, bmp, woff, woff2, ttf, otf, pdf,
mp4, webm, mp3, wav, wasm, zip) with
`application/octet-stream` fallback. Matching is on the final
extension, ASCII case-insensitive.

## Consequences

- Positive: real static serving with bounded memory, no per-request
  allocation, traversal-safe URL handling, HEAD support, and correct
  cleanup on completion *and* abort via `stream_end`.
- Negative: fixed slot pool caps concurrent file responses per mount
  (503 beyond it); no Range/conditional-request support (deferred —
  needs `pread`-style reads); no autoindex (directory without
  `index.html` → 403); no symlink confinement in v1.
- The middleware sees every request before routing; the prefix check
  is a single `memcmp`-style comparison and non-matching requests pay
  only that.

## Alternatives considered

- **Wildcard route** (`/static/*`): rejected — the router has no
  wildcard segment type; adding one is a larger router change than
  this feature warrants.
- **Per-request malloc**: rejected — the project avoids implicit
  allocation in request paths; the slot pool is deterministic and
  bounded.
- **Serve via `cfw_file_stream_t` directly in user handlers**:
  remains possible; the mount is convenience + safety policy, not a
  replacement for the primitives.

## Verification plan

- Unit tests: percent-decode matrix, `..`/encoded-traversal rejection,
  prefix matching, directory→index, 404/403/503 paths, MIME table.
- Integration: real server + temp docroot, GET file (dechunked body ==
  file bytes), HEAD, traversal attempts over the wire, slot exhaustion.
- Sanitizers preset (path buffers, decode bounds).
