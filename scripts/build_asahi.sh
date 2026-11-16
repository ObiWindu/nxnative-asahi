#!/usr/bin/env bash
# Build and test the Asahi Linux port.
#   GAME=<base title ID> scripts/build_asahi.sh   # build the guest host too
#   scripts/build_asahi.sh                        # platform + graphics + tests
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
build=${BUILD_DIR:-"$root/build-asahi"}
args=(-S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNXNATIVE_BUILD_TESTS=ON)
if [[ -n "${GAME:-}" ]]; then
  args+=(-DNXNATIVE_BUILD_HOST=ON -DNXNATIVE_GAME="$GAME")
fi
cmake "${args[@]}"
cmake --build "$build" -j"$(nproc)"
ctest --test-dir "$build" --output-on-failure
