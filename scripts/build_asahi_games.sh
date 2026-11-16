#!/usr/bin/env bash
# Build one runtime binary per game profile for the Asahi launcher.
#   scripts/build_asahi_games.sh 010015100B514000 0100A6301214E000 ...
#   RUNTIME_DIR=/path scripts/build_asahi_games.sh <ids...>
# Each binary is written as <RUNTIME_DIR>/nxnative-<title id>; the launcher
# selects it by the profile's title ID. The generic build-asahi/nxnative-asahi
# is still used by the CLI and by the launcher when no per-game binary exists.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${RUNTIME_DIR:-"$root/build-asahi/runtimes"}
[ "$#" -gt 0 ] || { echo "usage: $0 <base title id> [more...]" >&2; exit 2; }
mkdir -p "$out"
for id in "$@"; do
  case "$id" in
    [0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]) ;;
    *) echo "not a title ID: $id" >&2; exit 2 ;;
  esac
  b="$root/build-asahi-$id"
  echo "=== $id: $b"
  cmake -S "$root" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DNXNATIVE_BUILD_HOST=ON -DNXNATIVE_BUILD_TESTS=OFF -DNXNATIVE_GAME="$id" >/dev/null
  cmake --build "$b" -j"$(nproc)"
  install -m 755 "$b/nxnative-asahi" "$out/nxnative-$id"
  echo "=== $id: $out/nxnative-$id"
done
echo "Runtimes in $out; start the launcher with NXNATIVE_RUNTIME_DIR=$out"
