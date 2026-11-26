<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Changelog

All notable changes to this project will be documented here. The format is based
on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
semantic versioning once a stable release policy is adopted.

## [Unreleased]

### Added

- Repository bootstrap for `cfw`, a C11 dependency-free web framework
- Public API conventions with the `cfw_` prefix
- Version and status primitives
- Checked arithmetic helpers for `u32`, `u64`, `i64`, and `size_t` addition,
  subtraction, and multiplication
- `CFW_ERROR_OVERFLOW` status for representability failures
- Immutable byte-span primitive with bounds-checked subspan extraction
- Owning byte-buffer primitive with checked growth and span views
- Monotonic duration, timepoint, and deadline primitives for timeout handling
- Non-owning string-view primitive with byte-exact and ASCII case-insensitive
  comparison plus prefix/suffix checks
- Logging facade with level filtering, bounded formatted messages, pluggable
  sinks, approximate rate limiting, and dropped-message accounting
- Bump-allocator memory regions (`cfw_arena_t`) with aligned allocation,
  oversized-request handling, zeroed allocation, and bulk reset
- Platform capability detection: OS/architecture/endianness enums,
  pointer width, diagnostic names, and OS page-size query
- Owning file-descriptor wrapper (`cfw_fd_t`) with unique-ownership close
  and release semantics per the Phase 2 design record
- Cryptographic random-source boundary: OS CSPRNG bytes, `u64` draws, and
  unbiased bounded generation via rejection sampling
- Filesystem boundary primitives for regular-file size and directory checks
- Documented platform support matrix covering Linux, macOS, BSD, and Windows
- Internal pollset abstraction: level-triggered event sets with epoll on
  Linux, poll() fallback on other POSIX, deadline-aware waits, and per-fd
  tokens
- Internal nonblocking socket primitives: numeric IPv4/IPv6 addresses,
  socket/bind/listen/accept/connect wrappers, and `CFW_ERROR_WOULDBLOCK`
  status for nonblocking I/O
- HTTP/1.x token primitives: method, version, and status enums with strict
  bounded parsers and canonical names
- Allocation-free HTTP header-block iterator with strict token grammar,
  OWS trimming, and obs-fold/bare-CRLF rejection
- CMake build, presets, sanitizer configuration, install rules, and CMake
  package config
- Custom unit-test harness with debug, release, and sanitizer verification
- Development, testing, security, audit, roadmap, and decision-record
  documentation
