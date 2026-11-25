<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0009: Bump-allocator memory regions

- Status: Accepted
- Date: 2026-11-25

## Context

Phase 1 calls for memory-region abstractions. Parsers, header processing,
and per-request state benefit from many small, short-lived allocations with
bulk reclamation rather than individual `malloc`/`free` pairs, which are
both slower and easier to leak or double-free.

## Decision

Add `include/cfw/arena.h` and `src/cfw/arena.c` defining an opaque
`cfw_arena_t` bump allocator:

- `cfw_arena_create(block_size)` allocates the arena handle; `block_size`
  of 0 selects a 64 KiB default. Returns NULL on allocation failure.
- `cfw_arena_alloc` and `cfw_arena_alloc_zeroed` carve aligned storage from
  the current block; requests larger than `block_size` receive a dedicated
  block sized to the request.
- `cfw_arena_reset` rewinds every retained block for reuse.
- `cfw_arena_destroy` frees all blocks and the handle; NULL-safe.

All allocations are aligned to `_Alignof(max_align_t)`, enforced by padding
the block header offset and rounding every request size up with checked
arithmetic. Requests that would overflow `size_t` return
`CFW_ERROR_OVERFLOW`; allocation failure returns `CFW_ERROR_NOMEM`. A zero
`size` request succeeds and returns a valid (unused) pointer.

When the current block cannot satisfy a request, a new block is prepended
to the block list and the remainder of the old block is stranded until
reset or destroy. This keeps allocation O(1) and avoids free lists.

The arena is not thread-safe; callers needing concurrent allocation must
synchronize externally or use per-thread arenas.

## Alternatives considered

- **Public fixed-size arena struct:** rejected because block-list internals
  would become ABI-visible and constrain future growth strategies.
- **Free-list or segregated allocator:** rejected for now; parsers primarily
  need bump semantics, and a free list adds complexity and audit surface.
- **Caller-supplied backing storage:** deferred; an arena that allocates its
  own blocks is simpler to use correctly, and a static-backing variant can be
  added later without breaking this API.
- **Alignment parameter per allocation:** deferred; `max_align_t` covers all
  scalar and standard-layout needs, and unusual alignment can be added as a
  new function if a concrete consumer requires it.

## Consequences

- Parser and per-request code gains O(1) allocation with deterministic bulk
  cleanup and no per-object destructor requirements.
- Memory held by an arena is bounded by peak usage; long-lived arenas must
  be reset or destroyed to avoid unbounded retention.
- Dedicated oversized blocks are retained across resets; workloads with
  rare huge requests may prefer destroy/recreate cycles.
- Memory-safety audits must cover block sizing arithmetic, header-offset
  alignment, list traversal on destroy/reset, and failure paths.
