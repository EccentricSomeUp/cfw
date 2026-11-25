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
- Checked arithmetic helpers for `u32`, `u64`, and `size_t` addition,
  subtraction, and multiplication
- `CFW_ERROR_OVERFLOW` status for representability failures
- CMake build, presets, sanitizer configuration, install rules, and CMake
  package config
- Custom unit-test harness with debug, release, and sanitizer verification
- Development, testing, security, audit, roadmap, and decision-record
  documentation
