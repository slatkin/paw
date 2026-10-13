#!/usr/bin/env bash
# Usage: tools/sync-upstream.sh
#
# Merges upstream/master (codeberg.org/dnkl/foot) into a sync/upstream-<date>
# branch when upstream has commits that master lacks. Pushes the branch and
# opens a PR. It never merges to master. Does nothing when master already
# contains upstream/master.
#
# UPSTREAM_URL overrides the upstream repository. SYNC_DRY_RUN=1 stops after the
# merge, before the push, for local tests.
set -euo pipefail

UPSTREAM_URL="${UPSTREAM_URL:-https://codeberg.org/dnkl/foot.git}"

git fetch --quiet --no-tags "${UPSTREAM_URL}" '+refs/heads/master:refs/remotes/upstream/master'

if git merge-base --is-ancestor upstream/master HEAD; then
  echo "==> master already contains upstream/master"
  exit 0
fi

BRANCH="sync/upstream-$(date -u +%F)"
git switch --quiet -c "${BRANCH}"

if ! git merge --quiet --no-ff --no-edit upstream/master; then
  git merge --abort
  echo "error: upstream/master conflicts with master. Merge it by hand." >&2
  exit 1
fi

# AGENTS.md: new user-visible "foot strings from the fork show up here.
FOOT_STRINGS="$(git diff upstream/master -- '*.c' | grep -n '^+.*"foot' || true)"

echo "==> ${BRANCH} merges upstream/master at $(git rev-parse --short upstream/master)"

if [[ "${SYNC_DRY_RUN:-0}" == 1 ]]; then
  exit 0
fi

BODY="$(mktemp)"
trap 'rm -f "${BODY}"' EXIT
{
  echo "Merges upstream/master (codeberg.org/dnkl/foot) at \`$(git rev-parse --short upstream/master)\` into master."
  echo
  echo "Check from AGENTS.md (\"foot strings in fork-only C changes\"):"
  echo
  if [[ -n "${FOOT_STRINGS}" ]]; then
    echo '```'
    echo "${FOOT_STRINGS}"
    echo '```'
  else
    echo "None."
  fi
  echo
  echo "CI runs by workflow_dispatch on this branch. A PR opened by GITHUB_TOKEN does not start pull_request workflows."
} > "${BODY}"

git push --quiet origin "${BRANCH}"
gh pr create --base master --head "${BRANCH}" --title "sync: merge upstream foot" --body-file "${BODY}"
gh workflow run ci.yml --ref "${BRANCH}"
