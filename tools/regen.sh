#!/usr/bin/env bash
# Generate the game code locally from your own disc (SLES_538.60); generated code is never committed.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
profile=dw5xl
executable=SLES_538.60
config="$root/recomp/$profile/$profile.toml"
if [[ ! -f "$root/game/$profile/$executable" ]]; then
    echo "Missing your disc executable: game/$profile/$executable" >&2
    exit 1
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
awk -v root="$root" -v profile="$profile" -v executable="$executable" -v output="$tmp/output/" '
    /^input = / { print "input = \"" root "/game/" profile "/" executable "\""; next }
    /^ghidra_output = / { print "ghidra_output = \"" root "/recomp/" profile "/ghidra_functions.code.csv\""; next }
    /^output = / { print "output = \"" output "\""; next }
    { print }
' "$config" > "$tmp/config.toml"
cd "$root"
"$root/external/PS2Recomp/build/ps2xRecomp/ps2_recomp" "$tmp/config.toml" > "$root/recomp/$profile/recomp.log" 2>&1
mkdir -p "$root/recomp/$profile/output"
rsync -rc --delete --itemize-changes "$tmp/output/" "$root/recomp/$profile/output/" > "$tmp/changes.txt"
head -50 "$tmp/changes.txt"
