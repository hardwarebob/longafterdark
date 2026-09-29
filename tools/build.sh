#!/usr/bin/env bash
# Configure, build, and test the tree with the bootstrapped llvm-mingw.
#   bash tools/build.sh [extra cmake --build args]
# Env: AD_BUILD_DIR (default build/win), AD_NO_TESTS=1 to skip ctest,
#      AD_COMPONENTS="host/cpu;host/loader" to build a subset,
#      AD_CTEST_ARGS extra ctest args (e.g. "-R cpu").
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${AD_BUILD_DIR:-$ROOT/build/win}"
TC="$ROOT/third_party/toolchains"
if [ "$(uname -s)" = "Linux" ]; then
  export PATH="$TC/ninja:$PATH"
else
  export PATH="$TC/ninja:/c/Program Files/CMake/bin:$PATH"
fi

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/llvm-mingw.cmake" \
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
  -DAD_COMPONENTS="${AD_COMPONENTS:-}" >/dev/null
cmake --build "$BUILD" "$@"
if [ -z "${AD_NO_TESTS:-}" ]; then
  (cd "$BUILD" && eval "ctest --output-on-failure ${AD_CTEST_ARGS:-}")
fi
