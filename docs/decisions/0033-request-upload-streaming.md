# ADR 0033: Request-body upload streaming

## Status

Accepted.

## Context

Response streaming (ADR 0031) covered large *outbound* bodies, but
inbound bodies were still buffered whole: `cfw_http1_conn` decoded body
bytes into a per-connection buffer bounded by `max_body_size`, and the
dispatch handler only ran at `MESSAGE_DONE` with the complete span.
Uploads larger than `max_body_size` were rejected with 413 — correct as
a DoS bound, but it made large legitimate uploads impossible, and even
moderate bodies cost memory proportional to size.

The HTTP/1.1 reader already emits `CFW_HTTP1_EV_BODY` events
incrementally, so the transport layer needed no changes; the gap was in
the server/dispatch plumbing.

## Decision

Add optional per-request upload hooks to `cfw_server_options_t`:

```c
typedef cfw_status_t (*cfw_upload_begin_fn)(void *user,
    const cfw_http_request_head_t *head, void **upload);
typedef cfw_status_t (*cfw_upload_write_fn)(void *upload,
    const cfw_span_t *piece);
typedef void (*cfw_upload_end_fn)(void *upload, int completed);
```

- `upload_begin` runs once per request whose framing declares a body,
  after head validation (Host/Expect checks) and before the first body
  byte is consumed. It must return `CFW_OK` and set `*upload` to a
  non-NULL application token (file handle, buffer object, ...). Failure
  aborts the connection.
- `upload_write` receives each decoded body piece as a borrowed span
  valid only for the call; it must not retain it. Returning an error
  aborts the connection.
- `upload_end` runs exactly once per begun upload: `completed=1` after
  the full body arrives (before dispatch, so the handler sees a
  finalized upload) or `completed=0` on any abort — protocol error,
  413, write failure, timeout, or connection teardown.

When hooks are configured, the server does not accumulate the body:
`req.body` is empty and the token travels to the handler via the new
`cfw_request_t.upload` field, supplied through
`cfw_dispatch_request_ex`'s `cfw_dispatch_request_info_t` (which also
carries `remote_addr`, ADR 0032).

`max_body_size` still applies — it bounds time and upstream bandwidth,
not memory, when uploads are enabled.

## Consequences

- Uploads of arbitrary size are possible with O(1) connection memory;
  the application chooses the sink (temp file, object store, ...).
- The handler runs at `MESSAGE_DONE` as before; it cannot react to body
  bytes *during* receipt. Applications needing early rejection must do
  it in `upload_begin` (headers are available) — this is deliberate,
  keeping dispatch semantics unchanged.
- Pipelined requests are unaffected: uploads are strictly sequential
  per connection.
- `upload_begin`/`upload_write` must both be set to enable the feature;
  `upload_end` is optional but recommended for cleanup.
