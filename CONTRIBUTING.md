<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Contributing

`cfw` is designed to develop slowly and carefully. Contributions should improve
correctness, security, stability, portability, or extensibility without adding
unnecessary complexity or dependencies.

## Before changing code

1. Read `AGENTS.md`, `docs/process/development.md`, and the relevant security
   documents.
2. Identify the smallest coherent change that solves the problem.
3. State the compatibility and security impact.
4. Add tests before or alongside the implementation.
5. Update docs and decision records when the change affects design or process.

## Change requirements

Every change should include:

- A clear purpose and scope
- Public API or ABI impact assessment
- Security implications and applicable audits
- Regression tests for bugs
- Documentation updates where behavior or process changed
- Verification output for the relevant presets

## Review expectations

Reviewers should check for:

- Correctness under edge cases and invalid input
- Memory ownership and lifetime clarity
- Integer overflow, truncation, and allocation-size mistakes
- Error-path cleanup and predictable failure behavior
- Platform assumptions and portability
- API consistency with the `cfw_` prefix and C11 constraints
- Test coverage proportional to risk
- License and dependency compliance

## Changelog and releases

User-visible changes must be recorded under `## [Unreleased]` in
`CHANGELOG.md` using Keep a Changelog categories. Versioning, tagging, and
the release procedure are defined in `docs/process/versioning.md`; releases
are cut with `scripts/cfw-release.sh`, never by hand-tagging.

## Security issues

Do not open public issues for exploitable vulnerabilities. Follow
[SECURITY.md](SECURITY.md) and the private reporting process documented there.
