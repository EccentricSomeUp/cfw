<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0055: per-request completion audit hook

## Status

Accepted.

## Context

The roadmap's Phase 5 exit criteria deferred "structured audit
logging": `cfw_server_t` exposes aggregate counters
(`accepted_total`, `requests_total`, `errors_total`) and optional
lifecycle log lines, but applications have no per-request visibility —
no status code, byte counts, timing, or peer attribution per
transaction. Operators need this for access logs, anomaly detection,
and security auditing; retrofitting it later would be an ABI-visible
change to `cfw_server_options_t` semantics.

Constraints:

- The event loop is single-threaded and nonblocking; the hook must be
  cheap, synchronous, and never block.
- Borrowed views into per-connection buffers are only valid until the
  next request on that connection; the API must state the lifetime
  contract explicitly rather than promise copies.
- Streaming responses (ADR 0031) finalize long after dispatch
  returns; audit timing must reflect when bytes are actually queued,
  not when the handler returned.
- Protocol upgrades (ADR 0034) end HTTP framing; the 101 transaction
  still deserves exactly one record.
- Rejected requests (missing Host, bad Expect, oversize bodies,
  protocol errors) produce server-generated responses and must be
  auditable — they are the most security-relevant events.

## Decision

Add an optional `cfw_request_done_fn` hook plus `request_done_user` to
`cfw_server_options_t`. The callback receives a
`cfw_request_record_t` describing one completed request:

- `method`, `target`, `version`: copied out of the parsed request
  line. `method`/`target` are borrowed `cfw_strview_t`s valid only
  for the duration of the callback (they reference the connection's
  read buffer).
- `status`: the final HTTP status code the server determined
  (`cfw_http_status_t` value), or 0 when the connection ended before
  any response status was produced (e.g. timeout mid-body, client
  abort, dispatch failure).
- `request_bytes`: decoded request-body bytes received for this
  request (0 for bodyless requests; counts chunked-decoded bytes).
- `response_bytes`: bytes of the final response handed to the HTTP/1.1
  writer — serialized head plus body payload. Chunked-transfer
  framing overhead and interim `100 Continue` responses are excluded;
  for streams this is the payload bytes emitted up to the record.
- `duration`: monotonic time from request-head receipt to record
  emission.
- `remote_addr`: the connection's formatted peer address strview
  (same lifetime rules as `cfw_request_t.remote_addr`; empty when
  formatting failed).
- `upgraded`: nonzero when the transaction ended in a 101 protocol
  takeover.

Emission is exactly once per fully parsed request head, at the
earliest of:

1. the final response being fully queued (ordinary and server-error
   responses, including rejections);
2. an upgraded connection being activated (101 + hook configured);
3. an active response stream finishing (all payload bytes emitted);
4. the connection closing while a record is still pending — covers
   mid-stream aborts, request timeouts, drain teardown, and
   `cfw_server_free`.

The hook runs on the server's event-loop thread inside the same
call stack that produces the response; it must not block, must not
call back into the same server, and must copy anything it retains.
Like the upload and upgrade hooks, `request_done` may be NULL.

Implementation notes:

- Per-connection audit state (start timepoint, byte counters, pending
  flag, deferred-stream flag, last status) lives in the private
  `cfw_server_conn_t`; `cfw_server_options_t` grows by two pointer
  fields (additive; `0.x` ABI is unfrozen).
- Byte counters accumulate at the queue boundary inside
  `cfw_server_handle_done`, `cfw_server_send_error`, and
  `cfw_server_stream_pump`, so accounting covers every response the
  server produces without touching the HTTP/1.1 writer.
- `cfw_timepoint_diff` supplies the duration; clock failure degrades
  to a zero duration rather than failing the request.

## Alternatives considered

- **Structured log lines through `cfw_logger_t`**: rejected as the
  primary mechanism — the facade is free-form text with caller-side
  formatting, rate limiting that could drop audit records, and no
  typed fields. Apps needing JSON access logs can format records
  themselves in the hook; a structured sink can layer on later
  without API change.
- **Hook on `cfw_http1_conn_t` instead of the server**: rejected —
  `http1_conn` is a transport-level reader/writer with no notion of
  requests, responses, dispatch status, or peer addresses; the audit
  record is inherently a server-level concept.
- **Emission only at dispatch return**: rejected — streaming
  responses would report before bytes are sent, and aborted streams
  would be invisible. Deferred emission at stream end/close gives
  accurate `response_bytes`.
- **Copying strings into the record**: rejected — per-request
  allocation violates the zero-allocation request path (ADR 0036
  discipline); borrowed views plus an explicit lifetime contract match
  the existing `cfw_request_t`/`cfw_upload_*` conventions.

## Consequences

- Positive: complete per-request observability (status, bytes,
  latency, peer) for access logs and security monitoring; covers
  error and rejected paths, not just successful dispatches; zero cost
  when the hook is NULL.
- Negative: `cfw_server_options_t` grows (additive, pre-1.0);
  `response_bytes` excludes chunked framing overhead (documented);
  records for upgraded connections cover only the HTTP transaction.
- Security: the hook receives peer addresses and request targets —
  applications must apply their own redaction before persisting
  records (consistent with `cfw/log.h` redaction rules). The hook
  itself adds no new parsing or allocation surface.
- Follow-up: a convenience access-log formatter (e.g. common-log
  format sink) may be added as a separate, reviewed change.
