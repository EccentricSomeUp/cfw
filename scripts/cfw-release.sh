#!/usr/bin/env bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Cut a cfw release: verify the tree, roll the changelog forward, bump the
# project version, build and test every preset, commit, tag vX.Y.Z, and
# optionally push. Pushing the tag triggers .github/workflows/release.yml,
# which verifies all presets again and publishes the GitHub release.
#
# Usage: scripts/cfw-release.sh <major.minor.patch> [--push] [--dry-run]
#
# The release policy this script enforces lives in docs/process/versioning.md.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VERSION=""
DO_PUSH=0
DRY_RUN=0

usage() {
    cat >&2 <<'EOF'
Usage: scripts/cfw-release.sh <major.minor.patch> [--push] [--dry-run]

  <major.minor.patch>  Release version, e.g. 0.2.0 (must exceed the current
                        version in CMakeLists.txt).
  --push               Push the release commit and tag to origin after the
                        local tag is created. Without this flag nothing is
                        pushed and no GitHub release is produced.
  --dry-run            Run every check and print the planned actions without
                        modifying files, committing, tagging, or pushing.
EOF
    exit 64
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --push)     DO_PUSH=1 ;;
        --dry-run)  DRY_RUN=1 ;;
        -h|--help)  usage ;;
        -*)         echo "cfw-release: unknown option: $1" >&2; usage ;;
        *)
            if [[ -n "$VERSION" ]]; then
                echo "cfw-release: unexpected argument: $1" >&2
                usage
            fi
            VERSION="$1"
            ;;
    esac
    shift
done

fail() {
    echo "cfw-release: $*" >&2
    exit 1
}

[[ -n "$VERSION" ]] || usage

if ! printf '%s' "$VERSION" | grep -Eq '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'; then
    fail "invalid version '$VERSION' (expected major.minor.patch, no leading zeros)"
fi

# --- Prerequisite checks -----------------------------------------------------

git rev-parse --is-inside-work-tree >/dev/null 2>&1 \
    || fail "not inside a git work tree"

BRANCH="$(git symbolic-ref --quiet --short HEAD 2>/dev/null || true)"
[[ "$BRANCH" == "main" ]] \
    || fail "releases must be cut from 'main' (currently on '${BRANCH:-detached HEAD}')"

[[ -z "$(git status --porcelain)" ]] \
    || fail "working tree is not clean; commit or stash changes first"

CURRENT="$(sed -n -E 's/^[[:space:]]+VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' \
    CMakeLists.txt | head -n 1)"
[[ -n "$CURRENT" ]] || fail "could not determine current version from CMakeLists.txt"

version_gt() { # 0 if $1 > $2 (final releases only; prerelease tags unused)
    local IFS='.'
    local -a a b
    read -ra a <<< "${1%%-*}"
    read -ra b <<< "${2%%-*}"
    local i
    for i in 0 1 2; do
        if ((10#${a[i]:-0} > 10#${b[i]:-0})); then return 0; fi
        if ((10#${a[i]:-0} < 10#${b[i]:-0})); then return 1; fi
    done
    return 1
}

version_gt "$VERSION" "$CURRENT" \
    || fail "version $VERSION does not exceed current version $CURRENT"

git rev-parse -q --verify "refs/tags/v${VERSION}" >/dev/null 2>&1 \
    && fail "tag v${VERSION} already exists locally"

if git ls-remote --exit-code --tags origin "refs/tags/v${VERSION}" >/dev/null 2>&1; then
    fail "tag v${VERSION} already exists on origin"
fi

# The [Unreleased] section must exist and contain at least one substantive
# line (blank lines and HTML comments do not count).
if ! grep -q '^## \[Unreleased\]' CHANGELOG.md; then
    fail "CHANGELOG.md has no '## [Unreleased]' section"
fi
UNRELEASED_LINES="$(awk '
    /^## \[Unreleased\]/ { in_sec = 1; next }
    /^## \[/ && in_sec { exit }
    in_sec { print }
' CHANGELOG.md | grep -vE '^[[:space:]]*(<!--|$)' | grep -c . || true)"
[[ "${UNRELEASED_LINES:-0}" -gt 0 ]] \
    || fail "CHANGELOG.md [Unreleased] section is empty; document the release's changes first"

TODAY="$(date +%F)"
TAG="v${VERSION}"
COMMITTED=""

# The working tree was verified clean above, so on any failure before the
# release commit lands, restore the two files the script edits.
cleanup() {
    if [[ $? -ne 0 && -z "$COMMITTED" ]]; then
        git restore --staged --worktree -- CHANGELOG.md CMakeLists.txt 2>/dev/null || true
        rm -f CHANGELOG.md.tmp CMakeLists.txt.tmp
    fi
}
trap cleanup EXIT

if [[ "$DRY_RUN" -eq 1 ]]; then
    cat <<EOF
cfw-release dry run — planned actions for $TAG:

  1. Rename CHANGELOG.md '## [Unreleased]' content under '## [$VERSION] - $TODAY'
  2. Set project(VERSION) in CMakeLists.txt: $CURRENT -> $VERSION
  3. Configure, build, and test presets: debug, release, sanitizers, shared, debug-tls
  4. Commit: release: $TAG
  5. Create annotated tag: $TAG
EOF
    [[ "$DO_PUSH" -eq 1 ]] && echo "  6. Push branch and tag to origin (triggers release.yml)"
    echo "Dry run complete; no changes made."
    exit 0
fi

# --- Apply the release -------------------------------------------------------

echo "==> Rolling CHANGELOG.md forward to [$VERSION] - $TODAY"
awk -v ver="$VERSION" -v date="$TODAY" '
    !done && /^## \[Unreleased\][[:space:]]*$/ {
        print "## [Unreleased]"
        print ""
        print "## [" ver "] - " date
        done = 1
        next
    }
    { print }
' CHANGELOG.md > CHANGELOG.md.tmp && mv CHANGELOG.md.tmp CHANGELOG.md

echo "==> Bumping CMakeLists.txt project version: $CURRENT -> $VERSION"
awk -v ver="$VERSION" '
    !done && /^[[:space:]]+VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+[[:space:]]*$/ {
        sub(/[0-9]+\.[0-9]+\.[0-9]+/, ver)
        done = 1
    }
    { print }
' CMakeLists.txt > CMakeLists.txt.tmp && mv CMakeLists.txt.tmp CMakeLists.txt

grep -q "    VERSION $VERSION" CMakeLists.txt \
    || fail "CMakeLists.txt version bump did not apply cleanly"

# --- Verify ------------------------------------------------------------------

for preset in debug release sanitizers shared debug-tls; do
    echo "==> Preset '$preset': configure"
    cmake --preset "$preset"
    echo "==> Preset '$preset': build"
    cmake --build --preset "$preset"
    echo "==> Preset '$preset': test"
    ctest --preset "$preset" --output-on-failure
done

# --- Commit and tag ----------------------------------------------------------

git add CHANGELOG.md CMakeLists.txt
# Commits are SSH-signed (non-interactive, no passphrase prompt), so a
# signing failure means signing is misconfigured — fail loudly rather
# than silently producing an unsigned release commit.
git commit -m "release: ${TAG}"
COMMITTED=1
# Require the commit to carry a signature (any status other than 'N');
# whether the signature verifies depends on local signer configuration.
SIG_STATUS="$(git log -1 --format=%G?)"
[[ "$SIG_STATUS" != "N" ]] \
    || fail "release commit is not signed (commit.gpgsign may be disabled)"
git tag -a "$TAG" -m "cfw ${TAG}"

echo "==> Created commit and annotated tag ${TAG}"

if [[ "$DO_PUSH" -eq 1 ]]; then
    git push origin HEAD
    git push origin "refs/tags/${TAG}"
    echo "==> Pushed ${TAG}; .github/workflows/release.yml will build and publish the GitHub release"
else
    echo "==> Not pushed. Re-run with --push (or 'git push origin ${TAG}') to publish."
fi
