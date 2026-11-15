#!/bin/sh
# Builds dynarmic (external/dynarmic, with the externals it bundles) as static
# libraries for the core's JIT (make -f Makefile.libretro JIT=1). Writes the
# list of libraries to link, in a valid order, to <out>/libs.txt.
#   tools/build_dynarmic.sh [out dir] [extra cmake arguments...]
# Boost (header-only parts: icl, variant) comes from the external/boost
# submodule when it is checked out, so nothing has to be installed; else from
# the system. CMAKE names the cmake to run (default cmake).
set -e
OUT=${1:-build/dynarmic}
# flashem's changes to dynarmic (tools/dynarmic.patch), applied once
if git -C external/dynarmic apply --check ../../tools/dynarmic.patch 2>/dev/null; then
    git -C external/dynarmic apply ../../tools/dynarmic.patch
fi
[ $# -gt 0 ] && shift
CMAKE=${CMAKE:-cmake}
if [ -f external/boost/boost/variant.hpp ]; then
    set -- -DBoost_INCLUDE_DIR="$(pwd)/external/boost" -DBoost_NO_SYSTEM_PATHS=ON "$@"
fi
$CMAKE -S external/dynarmic -B "$OUT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DDYNARMIC_TESTS=OFF \
    -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON \
    -DDYNARMIC_WARNINGS_AS_ERRORS=OFF \
    -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF \
    -DDYNARMIC_FRONTENDS=A32 \
    "$@"
$CMAKE --build "$OUT" --target dynarmic -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
find "$OUT" -name '*.a' | sort > "$OUT/libs.txt"
cat "$OUT/libs.txt"
