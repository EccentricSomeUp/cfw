<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0004: Allocation and ownership policy

- Status: Accepted
- Date: 2026-11-25

## Context

Future buffer, parser, platform, and server code will need memory, but the
project has not yet defined allocator behavior, ownership transfer, failure
semantics, or capacity rules. Introducing public allocation wrappers or owning
buffer types before this decision would make the API harder to keep stable and
safer to audit.

## Decision

- Use standard C allocation (`malloc`, `calloc`, `realloc`, `free`) through
  internal helpers initially. No third-party allocator or custom allocator is
  approved yet.
- Every allocation size must be computed with checked arithmetic. Overflow or
  invalid size calculations fail before allocation is attempted.
- Allocation failure is reported as `CFW_ERROR_NOMEM`. Invalid arguments are
  reported as `CFW_ERROR_INVALID_ARGUMENT`; representability failures as
  `CFW_ERROR_OVERFLOW` where applicable.
- Public allocation wrappers are deferred. The first public owning abstraction
  should define explicit initialization, capacity, growth, cleanup, and
  ownership-transfer rules rather than exposing raw allocator functions.
- Spans remain non-owning borrowed views. Owning types must state lifetime,
  thread-safety, cleanup, and whether they may be moved or copied.
- Future owning APIs should define empty/invalid states, idempotent cleanup
  where practical, and deterministic behavior after failed initialization.
- No hidden global allocator state is introduced. A custom allocator, arena,
  pool, or allocation limits require a later ADR with security review.

## Alternatives considered

- **Expose `cfw_malloc`/`cfw_free` now:** rejected because raw wrappers add API
  surface without solving ownership, capacity, or growth semantics.
- **Custom allocator immediately:** rejected as premature and unnecessary for
  the current bootstrap stage.
- **Avoid allocation entirely:** rejected because bounded buffers and server
  state will require owned memory.
- **C++-style RAII ownership:** not applicable to the C11 implementation.

## Consequences

- Buffer and memory-region design can proceed with explicit ownership rules.
- Memory-safety audits must cover allocation-size arithmetic, cleanup paths,
  ownership transfer, and failure injection.
- Public buffer APIs remain Tier 2/3 changes requiring API review, tests, and
  security audit selection.
- Revisit this decision before adding TLS, authentication, plugin, or
  long-lived connection state that may impose special memory requirements.
