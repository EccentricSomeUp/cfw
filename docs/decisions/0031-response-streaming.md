# ADR 0031: Response streaming

## Status

Accepted.

## Context

`cfw_response_t` originally carried a single `cfw_span_t body` queued
wholesale into the connection's bounded output buffer (`max_out_size`).
Responses larger than that bound — file downloads, generated documents,
server-sent events — could not be represented at all.

## Decision

Add an optional pull-style body source to `cfw_response_t`:

```c
typedef cfw_status_t (*cfw_response_stream_fn)(void *user,
                                               cfw_span_t *chunk,
                                               int *eof);
```

Each call returns the next body chunk as a borrowed span (valid until the
next call) and sets `*eof` at end of stream. The function returns
`CFW_OK` to continue or `CFW_ERROR_*` to abort (the connection closes).

When `stream` is set and the method/status permit a body, the server:

- suppresses the single-span `body` field,
- emits `Transfer-Encoding: chunked` for HTTP/1.1 requests, framing each
  emitted piece as its own chunk (pieces are capped at 16 KiB and by
  output-queue free space, giving natural backpressure),
- falls back to close-delimited raw bytes for HTTP/1.0 and whenever the
  chunked header cannot be added (e.g. header scratch exhausted),
- restores or forces `close_after_write` only when the stream ends, so a
  non-keep-alive connection cannot be closed mid-stream by buffer drain,
- stops reading pipelined input while a stream is active, preserving
  strict response ordering.

The stream source contract requires forward progress: a non-EOF call
must return a non-empty chunk; violations close the connection.

## Consequences

- Arbitrarily large responses are possible with bounded memory: at most
  `max_out_size` queued plus one in-flight chunk.
- Request bodies remain fully buffered (`max_body_size` bound); the
  asymmetry is deliberate — upload streaming needs a different flow
  (flow-controlled request-body events) deferred to a later ADR.
- `stream` and `body` are mutually exclusive; `stream` wins and `body`
  is ignored when both are set.
- ABI: additive fields on `cfw_response_t` and one new typedef; existing
  users are unaffected.

## Verification

- `server_streaming_response`: chunked framing on HTTP/1.1, all chunks
  delivered, keep-alive preserved for a subsequent request.
- `server_streaming_http10`: no `Transfer-Encoding` header, close-
  delimited body, connection close after drain.
- All presets (debug, sanitizers, shared) pass.
