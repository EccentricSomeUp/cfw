<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# API and ABI stability policy

This document records the stability review performed at the Phase 7 exit
and the rules governing the public surface going forward.

## Current status

The project has not reached its stable release; `cfw_version_*` reports the
development version. The public API is **not yet frozen** — additive and
breaking changes are both permitted while the version is `0.x`, including
in tagged `0.x.y` releases. The freeze applies only when `1.0.0` is tagged:
semantic versioning then governs, and breaking changes require a major
version and a migration note in `CHANGELOG.md`.

## Review findings (Phase 7)

- Public structs (`cfw_router_t`, `cfw_dispatch_t`, `cfw_server_t`,
  `cfw_http1_server_t`, `cfw_logger_t`, etc.) expose fields directly.
  This is the established project convention; callers must not mutate
  fields the documentation marks as managed. New fields may be appended
  only at the end of public structs, and only in a minor release once
  frozen — appending changes `sizeof`, which is ABI-visible for
  stack-allocated users. Prefer opaque types for new modules.
- Enum types (`cfw_status_t`, `cfw_http_method_t`,
  `cfw_http_status_t`, event kinds) are append-only; existing values
  never change.
- Function signatures are append-only post-freeze; changed signatures
  get new names (`cfw_router_add_ex` is the precedent).
- Internal headers under `src/cfw/private/` are explicitly not public
  API and may change without notice.
- Windows stubs return `CFW_ERROR_UNAVAILABLE`; this is contractual,
  not a bug.

## Deprecation and migration policy

- A public API is deprecated by marking it `CFW_DEPRECATED` (defined in
  `cfw/export.h`) and noting the replacement in its header comment and in
  `CHANGELOG.md`.
- Deprecated APIs remain functional for at least one minor release after
  the deprecation lands, and are never removed in a patch release.
- Removal happens only in a major version, and the changelog entry must
  include a migration note describing the replacement and any required
  code changes.
- Behavior changes that break documented semantics follow the same rule
  as signature changes: a new name or a major version, never silent
  reinterpretation.

## Rules for contributors

1. New public types should be opaque (heap-allocated via `_init`/
   `_create`) unless a value type is clearly required.
2. Never reorder or remove public struct fields or enum values.
3. Every public function documents ownership, lifetime, thread-safety,
   and error behavior in its header comment.
4. ABI-affecting changes (struct layout, calling convention, symbol
   removal) require an ADR.
5. Deprecations use `CFW_DEPRECATED`, document the replacement, and are
   recorded in `CHANGELOG.md` per the policy above.
