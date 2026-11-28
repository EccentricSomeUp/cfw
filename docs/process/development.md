<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Development process

`cfw` will be developed slowly and deliberately. The goal is not rapid feature
coverage; it is a codebase that can be audited, reasoned about, and maintained
for a long period.

## Change lifecycle

### 1. Plan

Every non-trivial change starts with a written plan covering:

- Objective and non-goals
- Affected modules and public API surface
- Security, memory-safety, concurrency, and portability risks
- Compatibility impact
- Required tests and audits
- Rollback or fallback strategy for risky changes

### 2. Decide

Durable design decisions belong in `docs/decisions`. A new decision record is
required for:

- New dependencies or external tools
- Public API or ABI changes
- Security model changes
- Platform support changes
- Cryptography, TLS, authentication, or authorization design
- Major parser, protocol, or concurrency architecture choices

### 3. Implement

- Keep changes small and focused.
- Prefer explicit state machines and bounded loops.
- Avoid hidden global state.
- Add comments only where they explain non-obvious invariants or rationale.
- Do not weaken compiler warnings, sanitizers, or hardening to make code pass.

### 4. Test

- Add regression tests for every bug fix.
- Add unit tests for pure logic and state transitions.
- Add integration tests for cross-module behavior.
- Add security tests for attacker-controlled inputs and failure paths.
- Run debug and sanitizer presets for memory-safety-sensitive changes.
- Run the `tsan` preset for concurrency, threading, or shutdown-path
  changes (Clang required; see README).

### 5. Audit

Select audits from `docs/security/audit-catalog.md` based on the changed
subsystem. Record the selected audits and results in the change description or
pull request.

### 6. Review

Review should verify correctness, ownership, error paths, platform behavior,
API consistency, test quality, and documentation completeness. Reviewers should
reject changes that cannot explain their security impact.

## Risk tiers

### Tier 0: Documentation and process

Examples: README updates, audit templates, decision records.

Requirements: documentation review only.

### Tier 1: Internal implementation

Examples: private helpers, tests, build fixes that do not alter public API.

Requirements: tests and normal review.

### Tier 2: Public API or ABI

Examples: new public headers, functions, structures, callbacks.

Requirements: API design review, compatibility analysis, documentation,
regression tests, and security audit selection.

### Tier 3: Security-critical subsystem

Examples: parsers, sockets, TLS boundary, authentication, cryptography,
concurrency, memory management.

Requirements: detailed design or decision record, threat-model review,
multiple audit categories, sanitizer and fuzz testing where applicable, and
staged rollout.

## Branch and release expectations

- Keep development history reviewable.
- Do not force-push shared branches.
- Tag releases only through `scripts/cfw-release.sh` per
  `docs/process/versioning.md`, and only after the relevant audit
  checklist has been completed.
- Releases before `1.0` (`0.x`) could break API/ABI with documented
  migration notes; `1.0` and later follow `docs/api-stability.md`.
