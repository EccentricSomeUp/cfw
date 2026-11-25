<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# cfw

`cfw` is a planned security-focused web server framework written from scratch in
C11 with no third-party runtime dependencies.

The project is currently at repository-bootstrap stage. Only foundational
version/status APIs, checked arithmetic helpers, byte-span primitives, build
configuration, and test infrastructure exist; no HTTP, socket, routing, or
server functionality is implemented yet.

## Priorities

1. Security
2. Stability
3. Extensibility

These priorities are ordered intentionally. Features that weaken memory safety,
input validation, predictable failure behavior, or auditability require an
explicit design decision and security review before implementation.

## License

This project is licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE).

The license permits broad noncommercial use, modification, and distribution but
prohibits commercial use and does not permit closed-source redistribution of the
software. See [SECURITY.md](SECURITY.md) for vulnerability reporting and
[docs/security](docs/security) for the security process.

## Requirements

- A C11-capable C compiler
- CMake 3.25 or newer
- A platform-native build tool such as Make or Ninja
- No third-party libraries are required

## Quick start

Configure, build, and test the debug preset:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Build and test the sanitizer preset where supported:

```sh
cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure
```

List or run selected unit tests directly:

```sh
./build/debug/tests/cfw_unit_tests --list
./build/debug/tests/cfw_unit_tests --filter version
```

The exact test executable path may differ on Windows; use the path reported by
the selected generator.

## Project layout

```text
include/cfw/       Public C API headers
src/cfw/           Library implementation
src/cfw/private/   Internal headers and implementation details
tests/             Custom test suites and test harness
cmake/             CMake modules and package configuration
docs/              Architecture, process, security, and decision records
scripts/           Repository automation
```

Public API functions and types use the `cfw_` prefix. Internal symbols must not
use the public prefix unless they are intentionally part of the stable API.

## Documentation

- [Architecture](docs/architecture.md)
- [Development process](docs/process/development.md)
- [Testing strategy](docs/process/testing.md)
- [Release checklist](docs/process/release-checklist.md)
- [Security process](docs/security/process.md)
- [Threat model](docs/security/threat-model.md)
- [Audit catalog](docs/security/audit-catalog.md)
- [Roadmap](docs/roadmap.md)
- [Decision records](docs/decisions)

## Contribution expectations

Changes are expected to be small, planned, reviewed, and tested. Security- or
stability-sensitive changes require explicit rationale, regression coverage, and
applicable audit evidence. See [AGENTS.md](AGENTS.md) and
[CONTRIBUTING.md](CONTRIBUTING.md).
