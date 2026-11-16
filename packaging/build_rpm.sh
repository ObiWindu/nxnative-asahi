#!/usr/bin/env bash
# Build the Fedora Asahi package from the working tree.
#   packaging/build_rpm.sh                 # platform + launcher package
#   packaging/build_rpm.sh 010015100B514000 # package with that profile's host
# RPM_OUT=<dir> selects where the RPMs are copied (default: ./dist).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
game=${1:-}
out=${RPM_OUT:-"$root/dist"}
top=$(mktemp -d "${TMPDIR:-/tmp}/nxnative-rpm.XXXXXX")
trap 'rm -rf "$top"' EXIT
mkdir -p "$top"/{SOURCES,SPECS,BUILD,RPMS,SRPMS} "$out"
name=nxnative-asahi-0.1.0
tar -cf "$top/SOURCES/$name.tar" -C "$root" \
  --transform "s,^\.,$name," \
  --exclude=.git --exclude=build --exclude=build-asahi --exclude='build-asahi-*' \
  --exclude=dist --exclude='docs/images' --exclude='dm/games' --exclude='*.o' --exclude='__pycache__' .
if [ -n "$game" ]; then
  [ -f "$root/dm/games/$game/game.tsv" ] || { echo "no profile dm/games/$game" >&2; exit 2; }
  tar -rf "$top/SOURCES/$name.tar" -C "$root" --transform "s,^,$name/," "dm/games/game.py" "dm/games/$game"
fi
gzip -f "$top/SOURCES/$name.tar"
cp "$root/packaging/fedora-asahi/nxnative-asahi.spec" "$top/SPECS/"
defs=(-D "_topdir $top")
[ -n "$game" ] && defs+=(-D "game $game")
rpmbuild -ba "${defs[@]}" "$top/SPECS/nxnative-asahi.spec"
find "$top/RPMS" -name '*.rpm' -exec cp {} "$out/" \;
echo "RPMs in $out"
