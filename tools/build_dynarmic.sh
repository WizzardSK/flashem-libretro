#!/bin/sh
# Builds dynarmic (external/dynarmic, with the externals it bundles) as static
# libraries for the core's JIT (make -f Makefile.libretro JIT=1). Writes the
# list of libraries to link, in a valid order, to <out>/libs.txt.
#   tools/build_dynarmic.sh [out dir] [extra cmake arguments...]
set -e
OUT=${1:-build/dynarmic}
[ $# -gt 0 ] && shift
cmake -S external/dynarmic -B "$OUT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DDYNARMIC_TESTS=OFF \
    -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON \
    -DDYNARMIC_WARNINGS_AS_ERRORS=OFF \
    -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF \
    -DDYNARMIC_FRONTENDS=A32 \
    "$@"
cmake --build "$OUT" --target dynarmic -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
find "$OUT" -name '*.a' | sort > "$OUT/libs.txt"
cat "$OUT/libs.txt"
