<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit records

Audit records document recurring security, stability, portability, and compliance
reviews defined by `docs/security/audit-catalog.md`.

Each record should be stored as `audits/YYYY-MM-DD-<audit-name>.md` and include:

- Scope and commit range
- Audit category and goals
- Method and tools used
- Findings with severity and file/line references
- Disposition: fixed, accepted, deferred, or false positive
- Verification commands and results
- Follow-up actions and target date

Completed audits are evidence for releases and should not be deleted. Superseded
records may be marked as such but should remain for history.
