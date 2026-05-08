#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: bump-version.sh <new-version>" >&2
    exit 2
fi

new_version="$1"

if [[ ! "$new_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "bump-version.sh: refusing to write non-semver value '$new_version'" >&2
    exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake_file="$repo_root/CMakeLists.txt"

if [[ ! -f "$cmake_file" ]]; then
    echo "bump-version.sh: CMakeLists.txt not found at $cmake_file" >&2
    exit 1
fi

# Portable in-place sed (works on both BSD/macOS and GNU/Linux).
sed -E -i.bak "s/^([[:space:]]*VERSION )[0-9]+\.[0-9]+\.[0-9]+/\1${new_version}/" "$cmake_file"
rm -f "${cmake_file}.bak"

if ! grep -Eq "^[[:space:]]*VERSION ${new_version}([[:space:]]|$)" "$cmake_file"; then
    echo "bump-version.sh: failed to update VERSION to $new_version" >&2
    exit 1
fi

echo "bumped CMakeLists.txt VERSION to $new_version"
