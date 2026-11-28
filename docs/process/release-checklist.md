<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Release checklist

No release should be tagged until the applicable items below are complete.
Releases are governed by `docs/process/versioning.md` and executed with
`scripts/cfw-release.sh`, which mechanically enforces the build, test,
changelog, and tag-consistency items; the judgment items below remain the
releaser's responsibility.

## Required for every release

- [ ] All configured presets build cleanly with warnings enabled
- [ ] `ctest` passes for debug, release, and sanitizer presets
- [ ] Public headers compile as C11 and C++ consumers where supported
- [ ] Install and CMake package-config smoke test passes
- [ ] License headers and SPDX identifiers are present
- [ ] Dependency and supply-chain audit has no unresolved findings
- [ ] Documentation matches implemented behavior
- [ ] Changelog or release notes describe user-visible changes

## Required for security-sensitive releases

- [ ] Applicable audits from `docs/security/audit-catalog.md` are complete
- [ ] Audit records are stored under `audits/`
- [ ] Fuzz corpora are current and crashes are fixed or tracked
- [ ] Resource-limit and denial-of-service tests pass
- [ ] Platform matrix verification is complete for supported platforms

## Post-release verification

- [ ] The `release.yml` run for the tag concluded successfully
- [ ] `git tag -v vX.Y.Z` verifies the annotated tag signature
- [ ] `gh release view vX.Y.Z` shows the published release with notes
      matching the `CHANGELOG.md` section

## Required before 1.0

- [x] Public API and ABI stability policy is accepted and frozen at
      `1.0.0` (`docs/api-stability.md`)
- [x] All critical and high audit findings are fixed or explicitly
      accepted (`audits/2026-11-27-1.0-candidate-audit.md` and
      `audits/2026-11-28-1.0-final-verification-audit.md` — none open)
- [x] Migration and deprecation policy is documented
      (`docs/api-stability.md` + `docs/process/versioning.md`; no
      migration needed for the `1.0.0` transition)
- [x] Security reporting process has been tested — channel documented
      in `docs/security/process.md` and GitHub Private Vulnerability
      Reporting enabled on the repository. Maintainer-side flow verified
      end-to-end via API: draft advisory `GHSA-4wpj-qx2j-6598` created
      and closed as a channel test (2026-11-27). Reporter-side PVR
      submission cannot be exercised by a repo admin (GitHub rejects it
      by design; admins are directed to draft advisories instead), so
      the reporter path remains untested until an external report
      arrives — the channel itself is confirmed live.
