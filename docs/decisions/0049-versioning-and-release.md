<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0049: versioning and release process

## Status

Accepted.

## Context

The project already had the ingredients of a release process — SemVer in
`project(VERSION)`, a Keep a Changelog `CHANGELOG.md`, a manual release
checklist (`docs/process/release-checklist.md`), and a tag-triggered
`release.yml` workflow — but no written policy connecting them and no
automation enforcing it. Version numbers could drift between
`CMakeLists.txt`, the changelog, and tags; bump-level decisions were
implicit; and cutting a release required remembering a multi-step manual
procedure. The project is maintained with agent assistance, so the process
needed to be executable by an agent end-to-end with minimal human
judgment.

## Decision

Adopt `docs/process/versioning.md` as the governing policy:

- Semantic Versioning `MAJOR.MINOR.PATCH`; `project(VERSION)` in
  `CMakeLists.txt` is the single source of truth. Annotated git tags use
  `vX.Y.Z` on `main`; `vX.Y.Z-rc.N` prerelease tags are allowed for
  validation but excluded from `release.yml`.
- Explicit bump rules: PATCH for fixes/internal work, MINOR for additive
  API/features, MAJOR for breaking changes. Pre-1.0 (`0.x`), MINOR may
  break with a documented migration note; PATCH may not; `0 → 1` is the
  API/ABI freeze per `docs/api-stability.md` and requires the extra
  checklist items.
- All releases are cut by `scripts/cfw-release.sh`, which enforces the
  mechanical parts: clean tree on `main`, valid and increasing version,
  no duplicate local/remote tag, non-empty changelog `[Unreleased]`,
  changelog rollover, version bump, full four-preset build+test
  verification, release commit, annotated tag, and optional push. The
  tag push triggers the existing `release.yml` to publish the GitHub
  release — no secrets or manual CI steps required.
- `release.yml` tag filter is tightened to `vX.Y.Z` finals so stray
  prerelease-style tags cannot publish releases.

## Alternatives considered

- **Keep the process fully manual**: rejected — the checklist existed but
  nothing enforced version/changelog/tag consistency, and every release
  re-derived the same steps.
- **A tag-driven or CI-driven version bump (e.g. `release-please`)**:
  rejected — adds a third-party action/dependency (requires an ADR per
  AGENTS.md) and generates its own changelog format, conflicting with the
  curated Keep a Changelog entries the project maintains.
- **CalVer or datestamp versions**: rejected — SemVer already matches the
  documented API/ABI stability model and the `v*` release trigger.
- **Monorepo-style per-module versioning**: rejected — single library,
  single ABI version (`CFW_ABI_VERSION`); per-module versions add cost
  with no current benefit.

## Consequences

- Positive: releases are reproducible and agent-executable; version
  consistency between CMake, changelog, and tags is mechanically
  enforced; the four-preset gate runs before any tag exists rather than
  only after pushing.
- Negative: the script builds all four presets serially, so a release
  takes longer than a single-preset check; agents must still exercise
  judgment on bump level and changelog accuracy, which the script
  cannot verify.
- Compatibility: no public API/ABI impact. `CHANGELOG.md` gains dated
  `## [X.Y.Z]` sections going forward; `CFW_ABI_VERSION` unchanged.
- Security: the script performs no network operations unless `--push` is
  given, runs no untrusted code, and the release workflow's existing
  `contents: write` scope is unchanged. Tag pattern narrowing reduces
  the chance of accidental release publication.

## Verification plan

- `bash -n` syntax check and `--dry-run` exercise of all validation
  paths (bad version, dirty tree, empty changelog, duplicate tag).
- First real release through the script verifies end-to-end; subsequent
  releases confirm the `release.yml` trigger and GitHub release output.
