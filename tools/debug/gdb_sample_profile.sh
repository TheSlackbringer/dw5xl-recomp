#!/usr/bin/env bash
# Poor man's profiler (no perf installed): sample the dw5xl GameThread stack N times with gdb.
# usage: gdb_sample_profile.sh <pid> <samples> <out.txt>
pid=$1; n=$2; out=$3; : > "$out"
for i in $(seq "$n"); do
  gdb -p "$pid" -batch -nx -ex "set pagination off" -ex "thread apply all bt 12" 2>/dev/null \
    | awk '/^Thread .*GameThr/{p=1;next} /^Thread /{p=0} p&&/^#/' >> "$out"
  echo "----" >> "$out"
  sleep 0.3
done
