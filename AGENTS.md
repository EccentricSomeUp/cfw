# Agent and contributor rules

These rules apply to every change in this repository.

## Project constraints

- Target C11 only. Do not use compiler extensions in public or portable code.
- Do not add third-party runtime, build, test, or analysis dependencies without
  an approved architecture decision record.
- CMake is the only supported build system.
- Public API names use the `cfw_` prefix. Types use `cfw_` or `CFW_` naming as
  appropriate for the API style.
- Keep platform-specific code isolated behind internal abstraction boundaries.
- Preserve ABI stability once a symbol or structure is released. Additive changes
  are preferred; breaking changes require a new major version and migration plan.

## Change discipline

- Plan non-trivial changes before editing. State the goal, affected subsystems,
  security implications, compatibility impact, and verification plan.
- Prefer small, reviewable changes. Do not mix unrelated refactors with feature
  work.
- Security, memory safety, parser, concurrency, authentication/authorization,
  cryptography, and public ABI changes require explicit security review.
- Do not weaken warnings, sanitizers, hardening, license terms, or audit checks
  merely to make a build pass.
- Do not commit secrets, credentials, private keys, generated credentials, or
  user data.

## Required verification

For C changes, run at minimum:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

For memory-safety-sensitive changes, also run the sanitizer preset:

```sh
cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure
```

Add or update tests for every behavioral change. New tests must fail before the
fix and pass afterward when practical.

## Audit expectations

Security-relevant changes must identify applicable audits from
`docs/security/audit-catalog.md` and record results in the change description or
pull request. Regular audits should cover, at minimum:

- memory safety and undefined behavior
- integer overflow, truncation, and bounds handling
- parser and protocol state machines
- API misuse, ownership, and lifetime rules
- concurrency and cancellation
- platform-specific behavior
- denial-of-service resistance
- license and supply-chain compliance

## Versioning and releases

- Releases follow `docs/process/versioning.md` (SemVer; `project(VERSION)`
  in `CMakeLists.txt` is the single source of truth).
- Never hand-edit versions or create release tags manually; use
  `scripts/cfw-release.sh`, which enforces changelog, version, and tag
  consistency and runs all preset verification before tagging.
- Every user-visible change lands in `CHANGELOG.md` under
  `## [Unreleased]` before a release is cut.

## Documentation

Update documentation when architecture, API behavior, build requirements,
security assumptions, or operational procedures change. Record durable design
decisions in `docs/decisions` using the existing numbered format.
