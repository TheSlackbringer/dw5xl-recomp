#!/usr/bin/env bash
# DW5XL-TEST: replay one VIF1 capture in synchronous and asynchronous device
# mode and compare the GS streams and final VU1/VIF1 state.
#   tools/gsreplay/compare_modes.sh <capture.v1cap> [build-dir] [out-dir]
# Exit status 0 = identical. Extra replay flags via REPLAY_ARGS (e.g. "--drain each").
set -euo pipefail
capture=${1:?usage: compare_modes.sh <capture.v1cap> [build-dir] [out-dir]}
build=${2:-$(dirname "$0")/../../app/build-gsreplay}
out=${3:-$(mktemp -d)}
mkdir -p "$out"
replay="$build/dw5xl_vif1_replay"
gsrec="$build/dw5xl_gsrec"
for mode in sync async; do
    env -u PS2X_VIF1_CAPTURE -u PS2X_GS_RECORD "$replay" "$capture" --mode "$mode" \
        --out "$out/$mode.rec" --state-out "$out/$mode.state" ${REPLAY_ARGS:-}
done
status=0
"$gsrec" diff "$out/sync.rec" "$out/async.rec" --strict --report 3 || status=1
if cmp -s "$out/sync.state" "$out/async.state"; then
    echo "final VU1/VIF1 state: IDENTICAL"
else
    echo "final VU1/VIF1 state: DIFFERENT ($out/sync.state vs $out/async.state)"
    status=1
fi
echo "outputs in $out"
exit $status
