#!/usr/bin/env bash
# Generate locally from your own disc; never hand-edit or commit generated C++.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
selection=${1:-all}
case "$selection" in
    main|ovl00|ovl01|ovl02|all) ;;
    *) echo 'Usage: tools/regen.sh [main|ovl00|ovl01|ovl02|all]' >&2; exit 2 ;;
esac
regen() (
    local name=$1 profile config executable expected actual tmp
    if [[ "$name" == main ]]; then
        profile=dw5xl
        config="$root/recomp/$profile/dw5xl.toml"
        executable=SLES_538.60
    else
        profile="dw5xl/overlays/$name"
        config="$root/recomp/$profile/$name.toml"
        case "$name" in
            ovl00) executable=OVL_00.BIN ;;
            ovl01) executable=OVL_01.BIN ;;
            ovl02) executable=OVL_02.BIN ;;
        esac
    fi
    if [[ ! -f "$root/game/dw5xl/$executable" ]]; then
        echo "Missing your disc file: game/dw5xl/$executable" >&2
        return 1
    fi
    if [[ "$name" != main ]]; then
        expected=$(awk '{print $1}' "$root/recomp/$profile/input.sha256")
        actual=$(sha256sum "$root/game/dw5xl/$executable" | awk '{print $1}')
        if [[ "$actual" != "$expected" ]]; then
            echo "Unsupported $name; input SHA256 does not match the profile" >&2
            return 1
        fi
    fi
    tmp=$(mktemp -d)
    trap 'rm -rf -- "$tmp"' EXIT
    awk -v root="$root" -v profile="$profile" -v executable="$executable" -v output="$tmp/output/" '
        /^input = / { print "input = \"" root "/game/dw5xl/" executable "\""; next }
        /^ghidra_output = / { print "ghidra_output = \"" root "/recomp/" profile "/ghidra_functions.code.csv\""; next }
        /^output = / { print "output = \"" output "\""; next }
        { print }
    ' "$config" > "$tmp/config.toml"
    if ! "$root/external/PS2Recomp/build/ps2xRecomp/ps2_recomp" "$tmp/config.toml" > "$root/recomp/$profile/recomp.log" 2>&1; then
        echo "Generation failed; see recomp/$profile/recomp.log" >&2
        return 1
    fi
    mkdir -p "$root/recomp/$profile/output"
    rsync -rc --delete --itemize-changes "$tmp/output/" "$root/recomp/$profile/output/" > "$tmp/changes.txt"
    head -50 "$tmp/changes.txt"
 )
cd "$root"
if [[ "$selection" == main || "$selection" == all ]]; then regen main; fi
if [[ "$selection" == ovl00 || "$selection" == all ]]; then regen ovl00; fi
if [[ "$selection" == ovl01 || "$selection" == all ]]; then regen ovl01; fi
if [[ "$selection" == ovl02 || "$selection" == all ]]; then regen ovl02; fi
if [[ "$selection" == main || "$selection" == all ]]; then
    # Translate verified VU1 payloads from your own PAL executable into ignored local C++ (never commit it), with
    # the C++ translator (issue 025 Step 6; build it first: cmake --build app/build --target dw5xl_vu1lift).
    # Stop on input/translation errors rather than silently compiling stale lifted output.
    lifter="${DW5XL_VU1LIFT:-$root/app/build/dw5xl_vu1lift}"
    mkdir -p "$root/recomp/dw5xl/vu1" "$root/recomp/dw5xl/vu0"
    "$lifter" --sharded "$root/game/dw5xl/SLES_538.60" "$root/recomp/dw5xl/vu1"
    # Issue 025: the VU0 microprograms (VCALLMS) from the same executable, into recomp/dw5xl/vu0/.
    "$lifter" --vu0 "$root/game/dw5xl/SLES_538.60" "$root/recomp/dw5xl/vu0/dw5xl_vu0_lifted.cpp"
fi
