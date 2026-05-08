#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${MOONBASE_CPP_BUILD_DIR:-"$repo_root/build"}"
configuration="${MOONBASE_CPP_CONFIG:-Debug}"
live_tests=0
clean=0

usage() {
    cat <<EOF
Usage: scripts/test.sh [options]

Options:
  --clean       Remove the build directory before configuring.
  --live        Enable live API tests with MOONBASE_CPP_LIVE_TESTS=1.
  --build-dir   Build directory to use. Defaults to ./build.
  -h, --help    Show this help.

Environment:
  MOONBASE_CPP_BUILD_DIR   Build directory override.
  MOONBASE_CPP_CONFIG      CMake configuration name for multi-config generators.
  CMAKE_ARGS               Extra arguments passed to cmake configure.
  CTEST_ARGS               Extra arguments passed to ctest.
EOF
}

while (($#)); do
    case "$1" in
        --clean)
            clean=1
            shift
            ;;
        --live)
            live_tests=1
            shift
            ;;
        --build-dir)
            if (($# < 2)); then
                echo "error: --build-dir requires a value" >&2
                exit 2
            fi
            build_dir="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if ((clean)); then
    rm -rf "$build_dir"
fi

if [[ -n "${CMAKE_ARGS:-}" ]]; then
    # shellcheck disable=SC2086
    cmake \
        -S "$repo_root" \
        -B "$build_dir" \
        -DMOONBASE_BUILD_EXAMPLES=ON \
        -DMOONBASE_BUILD_TESTS=ON \
        ${CMAKE_ARGS}
else
    cmake \
        -S "$repo_root" \
        -B "$build_dir" \
        -DMOONBASE_BUILD_EXAMPLES=ON \
        -DMOONBASE_BUILD_TESTS=ON
fi

cmake --build "$build_dir" --config "$configuration" --parallel

if ((live_tests)); then
    if [[ -n "${CTEST_ARGS:-}" ]]; then
        # shellcheck disable=SC2086
        MOONBASE_CPP_LIVE_TESTS=1 ctest \
            --test-dir "$build_dir" \
            --build-config "$configuration" \
            --output-on-failure \
            ${CTEST_ARGS}
    else
        MOONBASE_CPP_LIVE_TESTS=1 ctest \
            --test-dir "$build_dir" \
            --build-config "$configuration" \
            --output-on-failure
    fi
else
    if [[ -n "${CTEST_ARGS:-}" ]]; then
        # shellcheck disable=SC2086
        ctest \
            --test-dir "$build_dir" \
            --build-config "$configuration" \
            --output-on-failure \
            ${CTEST_ARGS}
    else
        ctest \
            --test-dir "$build_dir" \
            --build-config "$configuration" \
            --output-on-failure
    fi
fi
