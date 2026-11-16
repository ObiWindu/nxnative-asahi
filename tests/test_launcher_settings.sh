#!/usr/bin/env bash
# Launcher settings: a per-game config is turned into the runtime environment.
set -euo pipefail
launcher=$1
tmp=$(mktemp -d /tmp/opencode/launcher-settings.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
id=0100000000000000
mkdir -p "$tmp/games/$id" "$tmp/config/nxnative/launcher"
printf 'title_id\t%s\nname\tSettings test\n' "$id" > "$tmp/games/$id/game.tsv"
printf 'fps=30\nscale=2\nmute=1\n' > "$tmp/config/nxnative/launcher/$id.conf"
export NXNATIVE_GAMES_DIR="$tmp/games" XDG_CONFIG_HOME="$tmp/config" XDG_DATA_HOME="$tmp/data" NXNATIVE_RUNTIME=/bin/true
out=$("$launcher" --dry-run "$id")
echo "$out"
grep -q 'DM_FPS_TARGET=30' <<<"$out"
grep -q 'DM_RENDER_SCALE=0.75' <<<"$out"
grep -q 'DM_AUDIO_MUTE=1' <<<"$out"
echo "LAUNCHER SETTINGS TEST PASSED"
