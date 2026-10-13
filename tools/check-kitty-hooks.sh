#!/usr/bin/env bash
# Usage: tools/check-kitty-hooks.sh
#
# Checks that each kitty graphics hook in tools/kitty-hooks.txt still appears
# in its upstream file. Run it after an upstream merge. Fails on a missing hook.
set -euo pipefail

status=0
while read -r file symbol; do
  if ! grep -q -w -F -- "${symbol}" "${file}"; then
    echo "missing: ${symbol} in ${file}" >&2
    status=1
  fi
done < <(grep -v '^#' tools/kitty-hooks.txt | grep -v '^$')

if [[ "${status}" -eq 0 ]]; then
  echo "==> all kitty hooks present"
fi
exit "${status}"
