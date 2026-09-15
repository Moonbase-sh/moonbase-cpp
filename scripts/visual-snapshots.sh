#!/usr/bin/env bash
#
# Build the offscreen UI snapshot harness, render every activation-UI state to
# PNGs, and (with --upload) push the folder to Argos for visual comparison.
#
#   ./scripts/visual-snapshots.sh            # build + render into ./ui-snapshots
#   ./scripts/visual-snapshots.sh --upload   # …then `argos upload`
#
# Env knobs:
#   BUILD_DIR              cmake build dir          (default: build-visual)
#   OUT_DIR               PNG output dir            (default: <repo>/ui-snapshots)
#   MOONBASE_JUCE_VERSION  JUCE tag to build against (default: the CMake default)
#   MOONBASE_OSX_SYSROOT  -DCMAKE_OSX_SYSROOT=…     (local toolchain workaround)
#   ARGOS_TOKEN           required for --upload
#
# Only the default (JUCE 8) render is a pixel baseline: JUCE 8 replaced the font
# backend, so an older JUCE renders the same layout with different glyph raster
# and must not be fed to Argos. Point MOONBASE_JUCE_VERSION at 6.1.3 or 7.0.12 to
# smoke-render those instead.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-${repo_root}/build-visual}"
out_dir="${OUT_DIR:-${repo_root}/ui-snapshots}"

cmake_args=(
  -B "${build_dir}"
  -S "${repo_root}"
  -DMOONBASE_BUILD_TESTS=OFF
  -DMOONBASE_BUILD_EXAMPLES=OFF
  -DMOONBASE_BUILD_UI_SNAPSHOTS=ON
)
if [[ -n "${MOONBASE_JUCE_VERSION:-}" ]]; then
  cmake_args+=( -DMOONBASE_JUCE_VERSION="${MOONBASE_JUCE_VERSION}" )
fi
if [[ -n "${MOONBASE_OSX_SYSROOT:-}" ]]; then
  cmake_args+=( -DCMAKE_OSX_SYSROOT="${MOONBASE_OSX_SYSROOT}" )
  export SDKROOT="${MOONBASE_OSX_SYSROOT}"
fi

# The repo's header-only SDK target requires OpenSSL at configure time (the JUCE
# module itself does not). Help CMake find Homebrew's copy on macOS.
if [[ "$(uname)" == "Darwin" && -z "${OPENSSL_ROOT_DIR:-}" ]] && command -v brew >/dev/null 2>&1; then
  if brew --prefix openssl@3 >/dev/null 2>&1; then
    cmake_args+=( -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" )
  fi
fi

echo "==> Configuring"
cmake "${cmake_args[@]}"

echo "==> Building MoonbaseUiSnapshots"
cmake --build "${build_dir}" --target MoonbaseUiSnapshots -j

bin="$(find "${build_dir}" -type f -name MoonbaseUiSnapshots -perm +111 2>/dev/null | head -1)"
if [[ -z "${bin}" ]]; then
  # Linux find has no -perm +111; fall back to any executable match.
  bin="$(find "${build_dir}" -type f -name MoonbaseUiSnapshots 2>/dev/null | head -1)"
fi
if [[ -z "${bin}" ]]; then
  echo "error: could not locate the MoonbaseUiSnapshots binary under ${build_dir}" >&2
  exit 1
fi

echo "==> Rendering snapshots into ${out_dir}"
rm -rf "${out_dir}"
mkdir -p "${out_dir}"
"${bin}" "${out_dir}"
ls -1 "${out_dir}"

# Cheap guard on the animation seam, which has two implementations (juce_animation
# when the build has it, the module's own otherwise) that have to agree.
echo "==> Checking the animation seam"
"${bin}" --anim-check

if [[ "${1:-}" == "--upload" ]]; then
  echo "==> Uploading to Argos"
  npx --yes @argos-ci/cli upload "${out_dir}"
fi
