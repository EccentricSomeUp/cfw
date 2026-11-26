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

## Required before 1.0

- [ ] Public API and ABI stability policy is accepted
- [ ] All critical and high audit findings are fixed or explicitly accepted
- [ ] Migration and deprecation policy is documented
- [ ] Security reporting process has been tested
