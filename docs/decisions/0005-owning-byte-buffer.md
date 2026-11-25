<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0005: Owning byte buffer

- Status: Accepted
- Date: 2026-11-25

## Context

`cfw_span_t` provides non-owning byte views, but parsers, serializers, and
server state also need owned, resizable byte storage. ADR 0004 deferred public
allocation wrappers until an owning abstraction defined capacity, growth,
cleanup, and ownership rules.

## Decision

Add `cfw_buffer_t`, an owning contiguous byte buffer with public `data`,
`size`, and `capacity` fields kept minimal for ABI stability. The API is:

- `cfw_buffer_init` initializes an empty buffer without allocating.
- `cfw_buffer_reserve` ensures at least the requested capacity, growing
  geometrically by doubling from the current capacity and never shrinking.
- `cfw_buffer_append` appends bytes after checked size arithmetic and reserve.
- `cfw_buffer_clear` resets size while retaining capacity.
- `cfw_buffer_free` releases storage and resets the buffer; it is safe to call
  on an empty buffer.
- `cfw_buffer_span` returns a borrowed `cfw_span_t` view of the used bytes.

Capacity growth is amortized doubling with no framework-imposed maximum.
Callers handling untrusted input must impose their own limits before appending.
Append sources must not overlap the destination buffer's storage; overlapping
append is invalid because reallocation may move the destination.

Allocation and size failures follow ADR 0004: invalid arguments return
`CFW_ERROR_INVALID_ARGUMENT`, representability failures return
`CFW_ERROR_OVERFLOW`, and allocation failure returns `CFW_ERROR_NOMEM`.
Functions that fail leave the buffer's previous size and contents intact.

## Alternatives considered

- **Exact-size growth only:** rejected because repeated small appends would
  cause quadratic reallocation behavior.
- **Fixed maximum capacity in the buffer:** rejected because appropriate limits
  are protocol- and deployment-specific and belong to higher layers.
- **Opaque buffer handle:** rejected because an embedded value type avoids a
  second allocation and matches span-style C API conventions.
- **Public raw allocator wrappers:** remains rejected by ADR 0004.

## Consequences

- Parser and protocol code gains owned byte storage without exposing allocator
  details.
- The public struct layout becomes ABI-visible; fields are therefore limited to
  `data`, `size`, and `capacity`.
- Memory-safety audits must cover reallocation, checked arithmetic, cleanup,
  invalid buffer states, append overlap rules, and failure paths.
- Higher layers remain responsible for request/body/header limits and for
  treating buffer contents as untrusted.
