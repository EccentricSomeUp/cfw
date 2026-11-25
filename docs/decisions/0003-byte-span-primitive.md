<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0003: Immutable byte span primitive

- Status: Accepted
- Date: 2026-11-25

## Context

Future HTTP parsing and protocol code needs to refer to contiguous input bytes
without copying them. The project needs a minimal representation with explicit
lengths and bounds-checked slicing before parsers are introduced.

## Decision

Add `cfw_span_t`, an immutable byte span containing a `const uint8_t *data`
pointer and `size_t size`. The public API is intentionally small:

- `cfw_span_init` validates the output span and rejects `NULL` data when size
  is nonzero.
- `cfw_span_subspan` validates the source span, offset, and length, then
  returns a span describing a subrange without allocation or copying.

Spans are values, not owned objects. They do not allocate, free, or retain
memory; callers must keep the underlying storage alive for the span's lifetime.
`cfw_span_subspan` rejects invalid spans, out-of-range offsets, and lengths
that exceed the remaining bytes with `CFW_ERROR_INVALID_ARGUMENT`.

## Alternatives considered

- **Copying buffers for every slice:** rejected because parser hot paths should
  avoid allocation and copying.
- **`const void *` public data pointer:** deferred in favor of `uint8_t` to make
  byte orientation explicit while `cfw_span_init` still accepts `const void *`
  for convenience.
- **Opaque span handle:** rejected because spans are lightweight values and do
  not need allocation or opaque storage.
- **Mutable buffer now:** deferred until allocation and ownership policy is
  designed.

## Consequences

- Parser code can operate on borrowed byte ranges with explicit bounds.
- The public struct layout becomes part of the API/ABI surface once released;
  fields are therefore kept minimal and immutable-by-convention.
- Callers must respect lifetime rules: spans never own or extend storage.
- Security review must cover bounds checks, invalid span states, pointer
  arithmetic, API misuse, and documentation/ABI accuracy.
