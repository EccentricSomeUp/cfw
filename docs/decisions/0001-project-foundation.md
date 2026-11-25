<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0001: Project foundation

- Status: Accepted
- Date: 2026-11-25

## Context

`cfw` is intended to be a from-scratch C web server framework prioritizing
security, stability, and performance. The project will develop slowly and
require regular audits and a custom test suite.

## Decisions

### Name and API prefix

The project and public API prefix are `cfw`. Public functions use `cfw_` names;
public constants use `CFW_`; public types use `cfw_` snake-case typedef names
where consistent with the API being designed.

### Language standard

The implementation targets C11. Compiler extensions are not part of the public
API and should be avoided in portable code. Platform-specific code must be
isolated behind internal abstraction layers.

### License

The project uses PolyForm Noncommercial License 1.0.0. This permits broad
noncommercial use, modification, and distribution while prohibiting commercial
use and closed-source redistribution. GPL-family licenses were rejected because
they still permit commercial use, which conflicts with the requested licensing
constraint.

### Dependencies

The framework implementation has no third-party runtime dependencies. CMake is
used only as the build system. Any future dependency requires an ADR covering
necessity, license compatibility, maintenance, supply-chain risk, and audit
impact.

### Build system

CMake 3.25+ is the only supported build system. Presets provide debug, release,
and sanitizer configurations. CTest runs the custom test suite but does not
define test semantics.

### Platform support

The launch goal is cross-platform support. Before networking code is
implemented, a platform support matrix and abstraction requirements must be
recorded in a later ADR. Initial work should avoid unnecessary POSIX-only or
Windows-only assumptions.

### Testing

Tests are custom C tests integrated with CTest. The suite will grow through
unit, integration, security, regression, property, and fuzz categories described
in `docs/process/testing.md`.

## Consequences

- Contributors must preserve C11 portability and avoid implicit platform APIs.
- Public API design must be conservative because ABI stability begins at the
  first stable release.
- Security review and audit selection are mandatory for risky changes.
- Build and package configuration must remain installable and consumable
  through CMake package config files.
