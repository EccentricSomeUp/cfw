<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0010: Platform capability detection

- Status: Accepted
- Date: 2026-11-25

## Context

Phase 2 of the roadmap begins with platform capability detection. Later
subsystems (sockets, polling, threading, filesystem) need a single,
compile-time-resolved description of the host OS, architecture, endianness,
and pointer width, plus runtime queries such as page size.

## Decision

Add `include/cfw/platform.h` and `src/cfw/platform.c`:

- `cfw_os_t`, `cfw_arch_t`, `cfw_endian_t`: closed enums covering the
  intended POSIX-like and Windows targets, with UNKNOWN/OTHER fallbacks.
- `cfw_platform_info_t`: value struct returned by `cfw_platform_detect()`.
- `cfw_os_name`, `cfw_arch_name`, `cfw_endian_name`: stable lowercase names
  for diagnostics and logging.
- `cfw_platform_page_size`: returns the OS page size via `sysconf` on
  POSIX or `GetSystemInfo` on Windows; `CFW_ERROR_UNAVAILABLE` where the
  query does not exist.

OS, architecture, and pointer width are resolved entirely at compile time
from predefined macros. Endianness prefers compiler `__BYTE_ORDER__`
macros, falls back to a known little-endian target list, and finally to a
runtime byte-order probe so detection never fails silently on unusual
compilers.

## Alternatives considered

- **Runtime-only detection (e.g. `uname`):** rejected because compile-time
  resolution is cheaper, cannot fail at runtime, and matches how the code is
  actually compiled.
- **Exposing raw predefined macros to callers:** rejected because it leaks
  compiler specifics into the public API and makes documentation brittle.
- **Including compiler identity:** deferred; no current subsystem needs it,
  and it can be added additively later.
- **Page size as a field of `cfw_platform_info_t`:** rejected because it is
  a runtime syscall-backed value, not a compile-time property.

## Consequences

- Platform-specific code paths can branch on stable enums instead of
  scattering `#ifdef` logic through callers.
- New OS/arch targets require updating detection and name tables; unknown
  values map to OTHER/UNKNOWN rather than failing.
- The portability audit must verify detection correctness on each supported
  platform in the matrix.
