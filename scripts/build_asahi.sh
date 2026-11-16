#!/usr/bin/env bash
# Build and test the Asahi Linux port.
#   GAME=<base title ID> scripts/build_asahi.sh   # build the guest host too
#   scripts/build_asahi.sh                        # platform + graphics + tests
#   CTEST_ARGS='-E <test>' scripts/build_asahi.sh # pass arguments to CTest
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
build=${BUILD_DIR:-"$root/build-asahi"}
args=(-S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNXNATIVE_BUILD_TESTS=ON)
if [[ -n "${GAME:-}" ]]; then
  args+=(-DNXNATIVE_BUILD_HOST=ON -DNXNATIVE_GAME="$GAME")
fi
cmake "${args[@]}"
cmake --build "$build" -j"$(nproc)"
ctest_args=()
if [[ -n "${CTEST_ARGS:-}" ]]; then
  read -r -a ctest_args <<< "$CTEST_ARGS"   # CI skips a test whose software Vulkan driver is known to crash
fi
ctest --test-dir "$build" --output-on-failure "${ctest_args[@]}"
