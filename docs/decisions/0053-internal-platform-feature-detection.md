<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0053: internal platform and feature detection

## Status

Accepted.

## Context

Platform conditionals were expressed by scattering raw compiler/OS
macros (`_WIN32`, `__linux__`, `__APPLE__`, `__FreeBSD__`, ...) through
the sources, and feature use (`MSG_NOSIGNAL`, `SOCK_CLOEXEC`,
`accept4`, `openat`/`O_NOFOLLOW`, `getrandom`, `arc4random_buf`,
`clock_gettime` clocks, `sysconf` page-size keys) was guarded by ad-hoc
`#ifdef`s at each call site. This made the codebase hard to audit, easy
to get wrong on platforms the Linux dev host cannot compile (the
kqueue backend nesting bug fixed alongside this change is one example),
and impossible to compile-test backend paths on foreign hosts.

Faking the raw macros with `-D` is not a substitute for real detection:
the same macros steer system headers and the toolchain, so a synthetic
`__APPLE__` build on Linux fails inside glibc headers long before it
reaches framework code.

## Decision

Introduce a single internal detection layer:

- `cmake/cfw_platform_detection.cmake` runs `check_c_source_compiles`
  and `check_symbol_exists` probes at configure time (against the target
  toolchain/sysroot when cross-compiling) and generates
  `cfw_platform_internal.h` into the build tree — internal only, never
  installed.
- The generated header defines normalized `CFW_PLATFORM_*` OS flags
  (`WINDOWS`, `MACOS`, `LINUX`, `ANDROID`, `FREEBSD`, `OPENBSD`,
  `NETBSD`, `DRAGONFLY`, `POSIX`) and `CFW_HAVE_*` capability flags
  (`EPOLL`, `KQUEUE`, `POLL`, `GETRANDOM`, `ARC4RANDOM_BUF`, `ACCEPT4`,
  `GETADDRINFO`, `SOCK_NONBLOCK`, `SOCK_CLOEXEC`, `MSG_NOSIGNAL`,
  `MSG_DONTWAIT`, `SO_NOSIGPIPE`, `O_NOFOLLOW`, `O_CLOEXEC`, `OPENAT`,
  `CLOCK_MONOTONIC`, `CLOCK_REALTIME`, `SC_PAGESIZE`, `SC_PAGE_SIZE`).
- `src/cfw/private/platform_detect.h` is the only header allowed to
  name raw predefined macros indirectly: it includes the generated
  header and establishes the feature-test macros (`_GNU_SOURCE`,
  `_POSIX_C_SOURCE`, `_DARWIN_C_SOURCE`, `_BSD_SOURCE`,
  `_FILE_OFFSET_BITS`) before any system header. Every platform-
  conditional translation unit includes it first.
- Pollset backend selection is explicit: `CFW_POLLF_BACKEND` accepts
  `epoll`, `kqueue`, `poll`, or `none` as a compile-only override for
  fixtures; otherwise selection follows `CFW_HAVE_EPOLL` →
  `CFW_HAVE_KQUEUE` → `CFW_HAVE_POLL` → none.
- Test targets gain compile fixtures that force each pollset backend.
  On hosts without `<sys/event.h>`, the kqueue fixture compiles the real
  kqueue code path against `tests/stubs/sys/event.h`, a minimal
  compile-only ABI stub — it verifies declarations and `#if` nesting,
  not runtime behavior; runtime kqueue coverage remains on macOS/BSD CI.

The public `cfw_platform_detect()` API (ADR 0010) is unchanged: it
remains the runtime-facing OS/arch/endianness report and may still use
compiler builtins for architecture and endianness. Compiler-identity
builtins used purely for attribute/pragma portability (`__GNUC__`,
`_MSC_VER` in `include/cfw/export.h` and `src/cfw/log.c`) are likewise
out of scope — they describe the compiler, not the target platform.

## Alternatives considered

- **Keep raw macros at call sites:** rejected; it is the status quo
  that produced the defects this change fixes and cannot be audited
  against the platform matrix.
- **Hand-maintained internal header instead of CMake checks:** rejected
  because feature availability genuinely varies within an OS family
  (libc version, kernel headers, configure-detected symbols), and
  `check_c_source_compiles`/`check_symbol_exists` already run against
  the correct target when cross-compiling.
- **Install the internal header:** rejected; it is a build artifact of
  the cfw build itself, not a consumer-facing contract.

## Consequences

- All platform branches in `src/` key off `CFW_PLATFORM_*`/`CFW_HAVE_*`;
  raw predefined macros appear only in `platform_detect.h` (selection)
  and `platform.c` (public runtime detection).
- Feature-test macro defaults are centralized, fixing latent
  `_POSIX_C_SOURCE`/`_DARWIN_C_SOURCE`/`_BSD_SOURCE` drift.
- New feature probes are added by extending the detection module and
  the generated template; new platforms extend the `CFW_PLATFORM_*`
  list.
- Compile fixtures catch unused helpers and bad `#if` nesting for every
  pollset backend on every host; real kqueue runtime behavior still
  requires macOS/BSD CI.
