#!/usr/bin/env bash
set -euo pipefail

expected_path="/home/primechain/primechain-next"
expected_remotes=(
  "https://github.com/midlincoln/primechain-next.git"
  "git@github.com:midlincoln/primechain-next.git"
)

actual_path="$(pwd -P)"
actual_remote="$(git remote get-url origin 2>/dev/null || true)"
branch="$(git branch --show-current 2>/dev/null || true)"

echo "PrimeChain repo safety check"
echo "  path:   ${actual_path}"
echo "  branch: ${branch:-unknown}"
echo "  origin: ${actual_remote:-missing}"

if [[ "${actual_path}" != "${expected_path}" ]]; then
  echo "ERROR: expected to be in ${expected_path}" >&2
  exit 1
fi

remote_ok=0
for expected_remote in "${expected_remotes[@]}"; do
  if [[ "${actual_remote}" == "${expected_remote}" ]]; then
    remote_ok=1
  fi
done
if [[ "${remote_ok}" -ne 1 ]]; then
  echo "ERROR: expected origin to be one of: ${expected_remotes[*]}" >&2
  exit 1
fi

echo "OK: this is the PrimeChain Next repo."
