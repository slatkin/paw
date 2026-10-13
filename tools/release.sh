#!/usr/bin/env bash
# Usage: tools/release.sh
#
# Cuts the next paw release from master. A release is named
# v<base>-paw.<n>, for example v1.28.0-paw.1. <base> is the newest upstream
# tag that the fork contains. <n> counts paw releases on that base.
#
# Bumps meson.build, moves ## Unreleased in CHANGELOG.md to the new version,
# commits, and pushes master and the tag. The tag annotation records the
# upstream commit. release.yml uses it in the release notes.
set -euo pipefail

if [[ "$(git branch --show-current)" != "master" ]]; then
  echo "error: releases are cut from master" >&2
  exit 1
fi

if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
  echo "error: uncommitted changes to tracked files" >&2
  exit 1
fi

git fetch --quiet --tags origin
git fetch --quiet --tags upstream

if [[ "$(git rev-parse HEAD)" != "$(git rev-parse origin/master)" ]]; then
  echo "error: HEAD is not origin/master; push or pull first" >&2
  exit 1
fi

# The fork base is where master meets upstream. Its newest upstream tag names
# the release. The match skips paw tags, which start with v.
BASE_SHA="$(git merge-base HEAD upstream/master)"
BASE="$(git describe --tags --abbrev=0 --match '[0-9]*' "${BASE_SHA}")"
UPSTREAM_SHA="$(git rev-parse --short "${BASE_SHA}")"

LAST="$(git tag -l "v${BASE}-paw.*" | sed 's/.*-paw\.//' | sort -n | tail -n 1)"
VERSION="${BASE}-paw.$(( ${LAST:-0} + 1 ))"
TAG="v${VERSION}"

echo "==> Releasing ${TAG} (upstream base ${BASE} at ${UPSTREAM_SHA})"

sed -i "s/^\( *version: \)'[^']*',/\1'${VERSION}',/" meson.build

if ! grep -qx '## Unreleased' CHANGELOG.md; then
  echo "error: CHANGELOG.md has no ## Unreleased heading" >&2
  exit 1
fi
ANCHOR="${VERSION//./-}"
sed -i \
  -e "s/^\* \[Unreleased\](#unreleased)$/&\n* [${VERSION}](#${ANCHOR})/" \
  -e "s/^## Unreleased$/## Unreleased\n\n## ${VERSION}/" \
  CHANGELOG.md

git add meson.build CHANGELOG.md
git commit -m "release: ${VERSION}"
git tag -a "${TAG}" -m "paw ${VERSION}" -m "Upstream base: ${BASE} (${UPSTREAM_SHA})"

git push origin master
git push origin "${TAG}"

echo "==> Pushed ${TAG}. release.yml builds the GitHub release from the tag."
