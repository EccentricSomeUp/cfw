<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Versioning and release process

This document is the single policy for how `cfw` is versioned, tagged, and
released. Releases are executed mechanically by `scripts/cfw-release.sh`;
humans and agents follow the procedure below rather than improvising.

## Version scheme

The project uses [Semantic Versioning](https://semver.org/) `MAJOR.MINOR.PATCH`.

- The authoritative version lives in exactly one place:
  `project(cfw VERSION ...)` in `CMakeLists.txt`. `cfw_get_version*()` and
  the generated `cfw_config.h` derive from it; `CHANGELOG.md` section
  headings and git tags must match it.
- Git tags are annotated tags named `vMAJOR.MINOR.PATCH` (e.g. `v0.2.0`),
  created on `main` only. Lightweight and moving tags are not used.
- `vMAJOR.MINOR.PATCH-rc.N` prerelease tags are permitted for CI validation
  but are excluded from `.github/workflows/release.yml` and never produce a
  GitHub release. The final `vX.Y.Z` tag supersedes them.
- `CFW_ABI_VERSION` (the shared-library `SOVERSION`) is independent of the
  project version and changes only when the public ABI breaks. It is
  currently `1`.

## Bump rules

| Change landed since last tag                          | Bump     |
| ----------------------------------------------------- | -------- |
| Bug fixes, docs, internal refactors, test-only work    | PATCH    |
| Additive public API/ABI, new modules, new features     | MINOR    |
| Breaking public API/ABI change                        | MAJOR    |

Additional rules while the version is `0.x` (pre-freeze, see
`docs/api-stability.md`):

- `0.x` MINOR releases **may** contain breaking changes; every breaking
  change must be called out in `CHANGELOG.md` with a migration note.
- `0.x` PATCH releases must not break the public API.
- `0.x` MAJOR bumps are not used; the `0 → 1` transition is the freeze
  itself and additionally requires the "Required before 1.0" items in
  `docs/process/release-checklist.md`.
- After `1.0.0`, the table above applies strictly and deprecations follow
  `docs/api-stability.md`.

When in doubt between PATCH and MINOR, choose MINOR: an unnecessary MINOR
is cheap, an undersold breaking change is not.

## Release procedure

All releases — whether cut by a human or an agent — go through the script:

```sh
# 1. Land all changes on main; populate CHANGELOG.md [Unreleased].
# 2. Decide the bump per the table above.
scripts/cfw-release.sh 0.2.0 --push
```

The script, in order:

1. Verifies it runs on `main` with a clean working tree.
2. Verifies the requested version is valid semver, exceeds the current
   `CMakeLists.txt` version, and has no existing local or remote tag.
3. Verifies `CHANGELOG.md` has a non-empty `[Unreleased]` section.
4. Moves the `[Unreleased]` entries under `## [X.Y.Z] - <date>` and leaves
   a fresh empty `[Unreleased]` section.
5. Sets `project(VERSION)` in `CMakeLists.txt`.
6. Configures, builds, and runs `ctest` for **all four presets**
   (`debug`, `release`, `sanitizers`, `shared`). Any failure aborts the
   release and restores the edited files.
7. Commits `release: vX.Y.Z` (containing only the changelog and version
   bump) and creates the annotated tag `vX.Y.Z`.
8. With `--push`, pushes the branch and the tag. The tag push triggers
   `release.yml`, which re-runs every preset on Ubuntu and macOS, builds
   source archives, and publishes the GitHub release with generated notes.

`--dry-run` performs all checks and prints the plan without modifying
anything — use it to validate a proposed version before committing to it.

### What still requires judgment

The script enforces mechanics, not content. Before running it, the releaser
must ensure:

- `CHANGELOG.md [Unreleased]` accurately describes every user-visible
  change since the last tag, using Keep a Changelog categories.
- The bump level matches the actual API/ABI impact of the changes.
- The security-sensitive items in `docs/process/release-checklist.md`
  are complete (audits in `audits/`, clean fuzz corpus, etc.). The script
  verifies builds and tests; it cannot verify that audits happened.

## Hotfixes and post-1.0 maintenance

Until `1.0.0`, all releases are cut from `main`. After the freeze, patch
releases for supported lines may be cut from a `release/X.Y` branch; that
workflow will be defined by an ADR when needed. Do not create such branches
pre-1.0.
