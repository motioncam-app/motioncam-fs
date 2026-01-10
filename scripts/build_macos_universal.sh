#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-universal-vcpkg"

if [[ -z "${VCPKG_ROOT:-}" ]]; then
  if [[ -d "${ROOT_DIR}/vcpkg" ]]; then
    VCPKG_ROOT="${ROOT_DIR}/vcpkg"
  else
    echo "VCPKG_ROOT is not set. Export VCPKG_ROOT or place vcpkg at ${ROOT_DIR}/vcpkg." >&2
    exit 1
  fi
fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DMOTIONCAMFUSE_MACOS_UNIVERSAL=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-osx \
  -DVCPKG_OSX_ARCHITECTURES="x86_64;arm64"

cmake --build "${BUILD_DIR}" --config Release

echo "Universal build output:"
echo "  ${BUILD_DIR}/MotionCamFuse.app"
