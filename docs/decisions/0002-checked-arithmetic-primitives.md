<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0002: Checked arithmetic primitives

- Status: Accepted
- Date: 2026-11-25

## Context

`cfw` will parse untrusted network input and perform length, offset, capacity,
and allocation-size arithmetic. C unsigned integer wraparound is defined, but
wraparound in security-sensitive arithmetic is still a defect because it can
produce undersized buffers, truncated lengths, or bypass limits. The project
needs a small, auditable primitive before buffer, string, parser, and platform
code are added.

## Decision

Expose checked arithmetic helpers for the unsigned widths most relevant to the
framework: `uint32_t`, `uint64_t`, and `size_t`. Each helper performs one
operation and reports success through `cfw_status_t`:

- `CFW_OK` on success
- `CFW_ERROR_INVALID_ARGUMENT` when the output pointer is `NULL`
- `CFW_ERROR_OVERFLOW` when the mathematical result is not representable

On error, helpers do not modify the output object. Overflow is reported by a
new `CFW_ERROR_OVERFLOW` status appended after existing status values so prior
enumerator values remain unchanged.

Allocation wrappers are intentionally not part of this decision. The allocator
interface, ownership rules, zeroing policy, failure behavior, and custom
allocator hooks require a separate design before public allocation functions
are introduced.

## Alternatives considered

- **Compiler builtins or macros:** rejected for the public API because behavior
  varies by compiler and macro APIs are harder to audit and document.
- **A single `uintmax_t` API:** rejected as the only interface because callers
  would still need range checks to recover narrower types, and `size_t` needs
  explicit treatment for allocation-size arithmetic.
- **Wrapping arithmetic with caller-side checks:** rejected because it spreads
  security-critical checks across call sites and is easy to get wrong.
- **Public allocation wrappers now:** deferred because allocator policy is a
  larger compatibility and security decision than checked arithmetic.

## Consequences

- Future buffer, parser, protocol, and platform code can use one reviewed
  arithmetic boundary instead of ad-hoc checks.
- Public API surface grows additively; no existing symbols or enum values are
  changed.
- The API is intentionally narrow: only add/subtract/multiply for `u32`, `u64`,
and `size_t` are provided initially. Additional widths or operations require
  evidence of need and the same audit treatment.
- Security review must cover integer overflow, truncation, signedness,
  allocation-size multiplication, API misuse, and documentation/ABI accuracy.
