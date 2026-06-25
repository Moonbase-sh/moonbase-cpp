#!/usr/bin/env bash
#
# Mirror the canonical Moonbase SDK headers into the self-contained JUCE module.
#
# The single source of truth is include/moonbase/. The JUCE module
# (modules/moonbase_licensing/) must be self-contained so developers can add the
# repo as a submodule and point Projucer or CMake straight at the module folder
# without any external include paths. This script copies the SDK headers into
# the module so that copy stays a faithful mirror.
#
# Run it after changing anything under include/moonbase/. CI runs it with
# --check to fail the build if the committed module is stale.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="${repo_root}/include/moonbase"
dst="${repo_root}/modules/moonbase_licensing/moonbase"

check_only=0
if [[ "${1:-}" == "--check" ]]; then
  check_only=1
fi

if [[ ! -d "${src}" ]]; then
  echo "error: source headers not found at ${src}" >&2
  exit 1
fi

if [[ "${check_only}" -eq 1 ]]; then
  tmp="$(mktemp -d)"
  trap 'rm -rf "${tmp}"' EXIT
  rsync -a --delete "${src}/" "${tmp}/moonbase/"
  if ! diff -r "${tmp}/moonbase" "${dst}" >/dev/null 2>&1; then
    echo "error: modules/moonbase_licensing/moonbase is out of sync with include/moonbase." >&2
    echo "       run scripts/sync-juce-module.sh and commit the result." >&2
    exit 1
  fi
  echo "moonbase_licensing module headers are in sync."
  exit 0
fi

mkdir -p "${dst}"
rsync -a --delete "${src}/" "${dst}/"
echo "Synced include/moonbase -> modules/moonbase_licensing/moonbase"
