#!/usr/bin/env bash
# Pack the ignored build inputs a Windows checkout needs: the dependency
# sources in app/deps (imgui, raylib, rlImGui and the pinned paraLLEl-GS with
# its Granite submodules) and the C++ generated locally from your disc
# (recomp/dw5xl/output and the OVL00-OVL02 outputs). Run it on Linux after
# tools/regen.sh. On Windows, extract it in the checkout root:
#   tar -xzf dw5xl-windows-inputs.tar.gz
#
#   tools/windows/pack-inputs.sh [--full] [output.tar.gz]
#
# --full also packs the source files of this working tree (tracked files and
# untracked files that are not ignored), so a Windows folder without Git gets
# exactly the Linux state, including uncommitted changes.
#
# The archive contains code generated from your game: keep it private and do
# not publish it. Disc files (game/, discs/), notes/, Ghidra projects and build
# directories are never included; copy your own game/dw5xl separately.
set -euo pipefail
full=0
if [[ ${1:-} == --full ]]; then full=1; shift; fi
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$root/dw5xl-windows-inputs.tar.gz}
case "$out" in /*) ;; *) out="$PWD/$out" ;; esac
cd "$root"

inputs=(
    app/deps/imgui-src app/deps/raylib-src app/deps/rlimgui-src app/deps/parallel-gs
    recomp/dw5xl/output
    recomp/dw5xl/overlays/ovl00/output recomp/dw5xl/overlays/ovl01/output recomp/dw5xl/overlays/ovl02/output
)
for path in "${inputs[@]}"; do
    [[ -e "$path" ]] || { echo "Missing $path (see README.md: dependencies, then tools/regen.sh all)" >&2; exit 1; }
done
for file in recomp/dw5xl/output/ps2_recompiled_functions.h \
            recomp/dw5xl/overlays/ovl00/output/register_functions.cpp \
            recomp/dw5xl/overlays/ovl01/output/register_functions.cpp \
            recomp/dw5xl/overlays/ovl02/output/register_functions.cpp \
            app/deps/parallel-gs/gs/gs_interface.cpp \
            app/deps/parallel-gs/Granite/third_party/volk/volk.c; do
    [[ -f "$file" ]] || { echo "Incomplete input: $file is missing" >&2; exit 1; }
done

list=$(mktemp)
stamp_dir=$(mktemp -d)
trap 'rm -f -- "$list"; rm -rf -- "$stamp_dir"' EXIT
printf '%s\n' "${inputs[@]}" > "$list"
if (( full )); then
    # Never pack disc data, notes, analysis databases, inputs (already listed)
    # or builds, even when a worktree symlink makes Git report them untracked.
    git ls-files -z --cached --others --exclude-standard |
        tr '\0' '\n' |
        grep -Ev '^(game|discs|notes|ghidra|ghidra_iop|logs)(/|$)|^app/deps(/|$)|^app/build|^external/PS2Recomp/build|/output(/|$)|(^|/)dw5xl-windows-inputs[^/]*$' |
        while IFS= read -r path; do [[ -f "$path" ]] && printf '%s\n' "$path"; done >> "$list"
fi

# Provenance stamp, extracted to app/deps (ignored by Git on Windows too).
mkdir -p "$stamp_dir/app/deps"
{
    echo "packed: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "dirty files: $(git status --porcelain --untracked-files=no 2>/dev/null | wc -l)"
    echo "full source: $full"
    echo "paraLLEl-GS: $(git -C app/deps/parallel-gs rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "generated sources: $(find -L recomp/dw5xl/output recomp/dw5xl/overlays/*/output -name '*.cpp' | wc -l)"
} > "$stamp_dir/app/deps/WINDOWS_INPUTS.txt"

# -h follows symlinks (worktrees link app/deps and the outputs); Git metadata
# of the dependency checkouts is not needed.
tar -czhf "$out" --exclude=.git --exclude='*.o' -T "$list" -C "$stamp_dir" app/deps/WINDOWS_INPUTS.txt
echo "Wrote $out ($(du -h "$out" | cut -f1)); keep it private."
