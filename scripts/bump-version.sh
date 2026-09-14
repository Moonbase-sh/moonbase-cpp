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
module_file="$repo_root/modules/moonbase_licensing/moonbase_licensing.h"

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

# The pinned GIT_TAG in the FetchContent install snippets. Every file listed here
# must carry at least one `GIT_TAG v<semver>` line and must also appear in
# .releaserc.json's git assets, or the rewrite happens in the release workflow's
# working tree and is thrown away. The grep guard below is what turns a moved or
# reformatted snippet into a failed release rather than a stale published pin.

pinned_files=(
    "README.md"
    "docs/core-sdk.md"
)

for rel in "${pinned_files[@]}"; do
    file="$repo_root/$rel"

    if [[ ! -f "$file" ]]; then
        echo "bump-version.sh: $rel not found at $file" >&2
        exit 1
    fi

    sed -E -i.bak "s|(GIT_TAG[[:space:]]+)v[0-9]+\.[0-9]+\.[0-9]+|\1v${new_version}|" "$file"
    rm -f "${file}.bak"

    if ! grep -Eq "GIT_TAG[[:space:]]+v${new_version}([^0-9]|$)" "$file"; then
        echo "bump-version.sh: failed to update $rel GIT_TAG to v$new_version" >&2
        exit 1
    fi

    echo "bumped $rel GIT_TAG to v$new_version"
done

# The JUCE module carries its own version in two places: the `version:` field the
# Projucer reads out of the BEGIN_JUCE_MODULE_DECLARATION block, and the
# MOONBASE_LICENSING_VERSION define that doubles as MOONBASE_CPP_VERSION (and so
# as the client's User-Agent) in module-only builds. Both must track the project
# version, and the file must be listed in .releaserc.json's git assets, or the
# rewrite happens in the release workflow's working tree and is thrown away.

if [[ ! -f "$module_file" ]]; then
    echo "bump-version.sh: moonbase_licensing.h not found at $module_file" >&2
    exit 1
fi

sed -E -i.bak \
    -e "s/^([[:space:]]*version:[[:space:]]+)[0-9]+\.[0-9]+\.[0-9]+/\1${new_version}/" \
    -e "s/(#define MOONBASE_LICENSING_VERSION \")[0-9]+\.[0-9]+\.[0-9]+(\")/\1${new_version}\2/" \
    "$module_file"
rm -f "${module_file}.bak"

if ! grep -Eq "^[[:space:]]*version:[[:space:]]+${new_version}([[:space:]]|$)" "$module_file"; then
    echo "bump-version.sh: failed to update the module declaration version to $new_version" >&2
    exit 1
fi

if ! grep -Fq "#define MOONBASE_LICENSING_VERSION \"${new_version}\"" "$module_file"; then
    echo "bump-version.sh: failed to update MOONBASE_LICENSING_VERSION to $new_version" >&2
    exit 1
fi

echo "bumped moonbase_licensing.h version to $new_version"
