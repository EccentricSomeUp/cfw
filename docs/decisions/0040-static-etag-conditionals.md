<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0040: Static file entity-tag validators and conditionals

## Status

Accepted.

## Context

ADR 0039 added `Last-Modified`-based conditional requests and
single-range support to the static middleware, and explicitly deferred
entity-tag conditionals (`If-Match`, `If-None-Match`, `If-Range`
entity-tags), noting that the scratch slot reserved space for a future
etag. RFC 9110 defines entity-tags as the preferred cache validator —
stronger than timestamps because they do not depend on clock
agreement — and `If-Range` entity-tags are what resumable-download
clients typically send.

Constraints inherited from ADR 0039:

- No per-request allocation; dynamic header values live in the fixed
  scratch ring carved from the mount arena.
- `cfw_static_mount_t` layout cannot change.
- Conditional evaluation must follow RFC 9110 section 13.2.2
  precedence: `If-Match` → `If-Unmodified-Since` →
  `If-None-Match` → `If-Modified-Since` → `If-Range` → `Range`.

## Decision

### ETag format

The mount emits a **strong** entity-tag `"<mtime>-<size>"` formatted
into the scratch slot's spare region (maximum 45 bytes, well inside
the 82-byte reserve). This is a heuristic validator in the same
family as common static servers: it changes whenever the
modification time or size does. It is not a content hash — two
different contents sharing mtime and size would produce the same
tag — which is an accepted trade-off for a dependency-free static
middleware and is documented here rather than hidden.

`ETag` is emitted on every response that also carries
`Last-Modified` (200, 206, 304, 416).

### Conditional evaluation

`cfw_static_plan` now evaluates, in order:

1. `If-Match` — comma-separated entity-tag list, **strong**
   comparison (`*` matches any current representation; `W/` members
   never match). A valid non-match → 412; a match or absent/ignorable
   header proceeds.
2. `If-Unmodified-Since` — evaluated only when `If-Match` is absent
   or ignorable; `mtime > date` → 412.
3. `If-None-Match` — entity-tag list, **weak** comparison for
   GET/HEAD (`W/` prefixes ignored on both sides; `*` matches). A
   match → 304; a valid non-match suppresses `If-Modified-Since`.
4. `If-Modified-Since` — evaluated only when `If-None-Match` is
   absent or ignorable; `mtime <= date` → 304.
5. `If-Range` — HTTP-date uses the ADR 0039 rule; otherwise the
   value is treated as a single entity-tag requiring **strong**
   comparison (a `W/` tag never matches → 200). Match → honor
   `Range`; mismatch → 200.
6. `Range` — unchanged from ADR 0039 (single `bytes=` range → 206;
   unsatisfiable → 416; ignored forms → 200).

Malformed list members are skipped; a header with no usable member is
treated as absent rather than fatal, preserving the ADR 0039
"malformed → ignore" posture. Empty members and surrounding OWS are
tolerated.

### Comparison helpers

- `cfw_static_etag_opaque` extracts the quoted opaque-tag from one
  list member, enforcing `etagc` (`%x21 / %x23-7E / obs-text`) and
  requiring the member to end at the closing quote.
- `cfw_static_etag_list_match` iterates comma-separated members with
  OWS trimming, returns 1 (match) / 0 (valid list, no match) /
  -1 (absent or no usable member).
- `cfw_static_format_etag` writes the tag; `cfw_static_i64_dec`
  handles the signed mtime (pre-1970 files) without overflow.

## Alternatives considered

- **Weak etag `W/"<mtime>-<size>"`**: rejected. `If-Range` requires
  strong comparison, so a weak tag would permanently disable ranged
  resume for clients echoing our validator. Emitting strong is the
  conventional static-server choice (nginx, Apache).
- **Content hash etag (SHA-1/xxhash)**: rejected — needs a crypto or
  hash dependency decision and costs a full file read per request;
  mtime+size is sufficient for the static-file threat model.
- **Inode/dev in the tag**: rejected — platform-specific, unstable
  across renames/copies, and unnecessary.

## Consequences

- Positive: complete RFC 9110 conditional coverage for the static
  middleware; clients get cache validators immune to clock skew;
  `If-Range` entity-tags (the common resume form) now work.
- Negative: the etag is a heuristic — mtime granularity is seconds
  and same-size rewrites within one second keep the tag; documented
  limitation, same class as timestamp validators.
- Compatibility: no API or ABI change; purely additive header and
  conditional behavior. Clients that previously ignored `ETag` are
  unaffected; clients that send `If-None-Match` now get 304s.

## Verification plan

- Unit tests: etag emission, `If-None-Match` (exact/`*`/weak/list/
  non-match), `If-Match` (exact/`*`/wrong/weak), precedence
  suppression (`INM` over `IMS`, `IM` over `IUS`), `If-Range`
  entity-tag gating (match/mismatch/weak).
- Sanitizers + shared presets; audit record per
  `docs/security/audit-catalog.md` (parser/protocol focus).
