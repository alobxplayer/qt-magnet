#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARCH="$(uname -m)"
case "${ARCH}" in
    x86_64|amd64)
        DEFAULT_PLATFORM="linux_64"
        ;;
    aarch64|arm64)
        DEFAULT_PLATFORM="linux_arm64"
        ;;
    *)
        DEFAULT_PLATFORM="linux_${ARCH}"
        ;;
esac

PLATFORM="${DEFAULT_PLATFORM}"
CONFIG="Release"
DO_TEST=0
DO_CLEAN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --test|-t)
            DO_TEST=1
            shift
            ;;
        --clean|-c)
            DO_CLEAN=1
            shift
            ;;
        --debug)
            CONFIG="Debug"
            shift
            ;;
        --platform|-p)
            PLATFORM="$2"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1"
            echo "Usage: ./build.sh [--test] [--clean] [--debug] [--platform <name>]"
            exit 1
            ;;
    esac
done

BUILD_DIR="${ROOT_DIR}/build/${PLATFORM}"
DIST_DIR="${ROOT_DIR}/dist/${PLATFORM}"

if [[ $DO_CLEAN -eq 1 ]]; then
    echo "Cleaning ${BUILD_DIR} and ${DIST_DIR}..."
    rm -rf "${BUILD_DIR}" "${DIST_DIR}"
fi

mkdir -p "${BUILD_DIR}" "${DIST_DIR}"

GENERATOR="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
fi

echo "Configuring CMake (${PLATFORM}, ${CONFIG})..."
cmake -B "${BUILD_DIR}" \
      -G "${GENERATOR}" \
      -DCMAKE_BUILD_TYPE="${CONFIG}" \
      -DCMAKE_INSTALL_PREFIX="${DIST_DIR}"

echo "Building project..."
cmake --build "${BUILD_DIR}" --config "${CONFIG}"

echo "Installing to ${DIST_DIR}..."
cmake --install "${BUILD_DIR}" --config "${CONFIG}"

echo "Build complete: ${DIST_DIR}"

if [[ $DO_TEST -eq 1 ]]; then
    echo "Running tests..."
    "${BUILD_DIR}/smoketests"
fi
