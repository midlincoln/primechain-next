#!/usr/bin/env bash
set -euo pipefail

expected_path="/home/ovanes/primechain-next"
expected_remote="git@github.com:midlincoln/primechain-next.git"

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

if [[ "${actual_remote}" != "${expected_remote}" ]]; then
  echo "ERROR: expected origin ${expected_remote}" >&2
  exit 1
fi

echo "OK: this is the PrimeChain Next repo."
