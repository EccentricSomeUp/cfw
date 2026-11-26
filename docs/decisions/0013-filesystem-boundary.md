<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0013: Filesystem boundary primitives

- Status: Accepted
- Date: 2026-11-26

## Context

Phase 2 requires a filesystem boundary before any file-serving or
configuration-loading code exists. The boundary must be portable (POSIX
`stat` vs Windows `_stat64`) and must not silently encourage unsafe path
handling.

## Decision

Add `include/cfw/fs.h` and `src/cfw/fs.c` with two metadata queries:

- `cfw_file_size(path, size)`: returns the size of a regular file;
  non-regular files return `CFW_ERROR_INVALID_ARGUMENT` and missing paths
  return `CFW_ERROR_IO`.
- `cfw_file_is_directory(path, is_dir)`: reports whether a path is a
  directory.

Windows uses `_stat64` so sizes above 2 GiB are not truncated; POSIX uses
`stat`. The API deliberately excludes path canonicalization, traversal
checks, and open-with-flags; those belong to the consumer (e.g. static file
serving) which must confine paths itself.

## Alternatives considered

- **Full path-safety helpers now:** rejected; correct confinement depends
  on deployment root and URL-decoding rules that belong to the serving
  layer.
- **`open`/`read` wrappers:** deferred until a consumer needs streaming
  file I/O.
- **Wide-character Windows paths:** deferred; UTF-8 `char*` paths are the
  project convention for now, with a documented limitation.

## Consequences

- File metadata access is portable and checked.
- Path traversal protection remains a documented consumer
  responsibility and a required audit focus when static file serving lands.
