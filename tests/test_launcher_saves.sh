#!/usr/bin/env bash
# Save management from the launcher CLI: backup creates a sibling copy and
# delete removes only the game's own save folders.
set -euo pipefail
launcher=$1
tmp=$(mktemp -d /tmp/opencode/launcher-saves.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
id=0100000000000000
mkdir -p "$tmp/games/$id/save" "$tmp/games/$id/save_device"
printf 'title_id\t%s\nname\tSave test\n' "$id" > "$tmp/games/$id/game.tsv"
echo one > "$tmp/games/$id/save/a.bin"
echo two > "$tmp/games/$id/save_device/b.bin"
export NXNATIVE_GAMES_DIR="$tmp/games"
out=$("$launcher" --backup "$id")
echo "$out"
ls -d "$tmp/games/$id/save.backup-"* >/dev/null
ls -d "$tmp/games/$id/save_device.backup-"* >/dev/null
echo changed > "$tmp/games/$id/save/a.bin"
out=$("$launcher" --restore-save "$id")
echo "$out"
grep -q one "$tmp/games/$id/save/a.bin"
out=$("$launcher" --delete-save "$id")
echo "$out"
[ ! -d "$tmp/games/$id/save" ]
[ ! -d "$tmp/games/$id/save_device" ]
echo "LAUNCHER SAVE TEST PASSED"
