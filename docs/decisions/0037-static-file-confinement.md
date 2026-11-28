<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0037: Static file path confinement

## Status

Accepted.

## Context

ADR 0036 added static file serving with lexical URL validation: decoded
suffixes reject `..` segments, NUL bytes, and backslashes before being
joined to the document root. Audit `2026-11-26-static-files-audit.md`
accepted two residual risks for v1:

- Symlinks inside the document root could point outside it; path-based
  `stat`/`open` followed them.
- Validation and open were separate path operations, leaving a
  stat-then-open race for swapped filesystem objects.

Constraints:

- The public filesystem boundary (`cfw/fs.h`) is portable and does not
  expose descriptor-relative or no-follow opens.
- Static serving must remain allocation-free per request and bounded by
  the existing stream-slot pool.
- Windows remains unsupported for the server; filesystem code must still
  compile there.

## Decision

Add an internal `cfw_file_open_confined` primitive behind
`src/cfw/private/` and use it for every static GET/HEAD file access.

On POSIX the primitive:

- opens the trusted document root once as a directory descriptor;
- walks each `/`-separated suffix component with `openat`;
- opens intermediate components with `O_RDONLY | O_NOFOLLOW` and
  verifies each is a directory with `fstat`;
- opens the final component with `O_RDONLY | O_NOFOLLOW` and verifies
  the resulting descriptor is a regular file with `fstat`;
- returns the already-open descriptor in `cfw_file_t`, so callers never
  reopen the path.

The static middleware keeps its existing lexical validation (decoded,
no `..`, no NUL/backslash, bounded length) and directory→`index.html`
resolution for path selection and status classification, but the actual
file handle always comes from the confined descriptor. HEAD now opens
and closes the confined file instead of relying on path metadata, so
HEAD and GET agree on whether a path is servable.

On Windows the same internal entry point performs equivalent lexical
validation and opens the joined path through the CRT `_open`/`_fstat64`
boundary. Windows has no `openat`/`O_NOFOLLOW` equivalent in this
codebase, so symlink confinement there remains best-effort and out of
scope while Windows server support is deferred.

## Alternatives considered

- **Canonicalize then prefix-check:** rejected. `realpath` introduces
  allocation, retains a canonicalize-then-open race, and has
  platform-dependent case/Unicode semantics.
- **`openat2`/`RESOLVE_BENEATH`:** rejected as the only mechanism. It is
  Linux-specific and unavailable on the other POSIX targets; component
  `openat` with `O_NOFOLLOW` is portable across the supported POSIX set.
- **Keep path stat and open:** rejected; it preserves the accepted
  symlink and swap gaps this ADR exists to close.
- **Public filesystem API:** rejected for now. The primitive is an
  internal hardening boundary for static serving; promoting it would
  expand the stable API surface before 1.0 without a demonstrated
  portable contract.

## Consequences

- Positive: symlinked files or directories inside the document root can
  no longer be served on POSIX; the opened descriptor is `fstat`-verified
  as a regular file; HEAD and GET share the same confinement decision.
- Negative: serving a file now costs one `openat`/`fstat` pair per path
  component (bounded by the 4 KiB path cap); Windows remains
  best-effort; hard links to in-root files are still served because they
  are indistinguishable from regular files by path.
- Compatibility: additive internal API only; no public ABI change. The
  static middleware's observable behavior changes only for symlinked or
  swapped paths, which now return 404 instead of following the link.
- Follow-up: update the static-file audit record and keep Range /
  conditional-request work (deferred in ADR 0036) for a later change.
