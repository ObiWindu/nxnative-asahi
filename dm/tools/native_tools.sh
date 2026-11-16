#!/bin/sh
# Builds the repository's two C# tools as native executables (NativeAOT), so running them needs no .NET:
#   dmsh_xlate → dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate   the shader translator (Ryujinx's shader library, dm/third_party/ryujinx), part of the translation chain
#   nx_extract → dm/tools/nx_extract_bin/nx_extract        NSP extraction (LibHac)
# On Asahi Linux they are AArch64 ELF executables; on macOS the old universal build is kept.
# Each embeds its own sources and prints their digest with --source-sha: tool_lock.py refuses a translator, and the NSP script an nx_extract, not built from the sources in the repository.
# The build is not byte-reproducible (the linker gives each build its own UUID): a rebuild of the same sources has another fingerprint. So rebuild only after changing a tool's sources, then:
#   dmsh_xlate: tool_lock.py --update-lock and dmsh_chain.py --header, commit the binary with the lock and the header (a chain change: rebuild the shaders);
#   nx_extract: commit the binary with the sources.
# Usage: native_tools.sh [dmsh_xlate] [nx_extract]   (default: both)   Environment: WORK=folder for the intermediate files, DOTNET=dotnet
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-$(mktemp -d -t native_tools)}
DOTNET=${DOTNET:-dotnet}
mkdir -p "$WORK"
command -v "$DOTNET" > /dev/null || { echo '[RED] building the native tools needs the .NET SDK 10 (https://dotnet.microsoft.com/download/dotnet/10.0); running them does not' >&2; exit 1; }

build_linux() {   # <name> <csproj> <output executable>
    "$DOTNET" publish "$2" -c Release -r linux-arm64 -nologo -v quiet --artifacts-path "$WORK/$1-artifacts" -o "$WORK/$1-linux-arm64" > "$WORK/$1-linux-arm64.log" 2>&1 \
        || { tail -20 "$WORK/$1-linux-arm64.log" >&2; echo "[RED] $1 did not build (log: $WORK/$1-linux-arm64.log)" >&2; exit 1; }
    cp "$WORK/$1-linux-arm64/$1" "$3"
    chmod 755 "$3"
    echo "$1: $(sha256sum "$3" | cut -c1-64), $(wc -c < "$3" | tr -d ' ') bytes"
}

build_macos() {   # <name> <csproj> <output executable>
    for rid in osx-arm64 osx-x64; do   # NativeAOT compiles one architecture per publish; the SDK cross-compiles the other
        "$DOTNET" publish "$2" -c Release -r "$rid" -nologo -v quiet --artifacts-path "$WORK/$1-artifacts" -o "$WORK/$1-$rid" > "$WORK/$1-$rid.log" 2>&1 \
            || { tail -20 "$WORK/$1-$rid.log" >&2; echo "[RED] $1 did not build for $rid (log: $WORK/$1-$rid.log)" >&2; exit 1; }
        weak=$(nm -m -u "$WORK/$1-$rid/$1" | grep -w weak || true)
        [ -z "$weak" ] || { echo "$weak" >&2; echo "[RED] $1 ($rid) imports functions its minimum macOS does not have: build with Microsoft's .NET SDK" >&2; exit 1; }
    done
    lipo -create "$WORK/$1-osx-arm64/$1" "$WORK/$1-osx-x64/$1" -output "$3"
    for a in arm64 x86_64; do echo "$1 ($a): source $(arch -"$a" "$3" --source-sha)"; done
    echo "$1: sha256 $(shasum -a 256 "$3" | cut -c1-64), $(wc -c < "$3" | tr -d ' ') bytes"
}

build() {
    if [ "$(uname -s)" = Darwin ]; then build_macos "$@"; else build_linux "$@"; fi
}

for t in ${*:-dmsh_xlate nx_extract}; do
    case $t in
        dmsh_xlate) build dmsh_xlate "$REPO/dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj" "$REPO/dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate" ;;
        nx_extract) build nx_extract "$REPO/dm/tools/nx_extract/nx_extract.csproj" "$REPO/dm/tools/nx_extract_bin/nx_extract" ;;
        *) echo "usage: native_tools.sh [dmsh_xlate] [nx_extract]" >&2; exit 2 ;;
    esac
done
