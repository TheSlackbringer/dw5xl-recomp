#!/usr/bin/env python3
"""Per-second load of the running dw5xl: CPU % per thread name and GPU busy % (issue 015).

  tools/thread_load.py --out <file.csv> [--gpu auto|cardN] [--wait 60] [--max-seconds N]

Waits for a dw5xl process, then once a second writes one CSV row: wall time, the CPU % of each thread name
(threads with the same name are summed, e.g. the Vulkan driver's), and the mean GPU busy % of the 10 samples
taken in that second (amdgpu sysfs gpu_busy_percent; 'auto' picks the card with the most VRAM). Stops when the
game exits. Reads /proc and sysfs only; nothing is sent to the game or the desktop.
"""
import argparse
import glob
import os
import subprocess
import sys
import time

TICK = os.sysconf('SC_CLK_TCK')


def find_pid(wait):
    end = time.time() + wait
    while time.time() < end:
        out = subprocess.run(['pgrep', '-x', 'dw5xl'], capture_output=True, text=True).stdout.split()
        if out:
            return int(out[0])
        time.sleep(0.2)
    return None


def gpu_path(choice):
    if choice != 'auto':
        return f'/sys/class/drm/{choice}/device/gpu_busy_percent'
    best, size = None, -1
    for card in glob.glob('/sys/class/drm/card[0-9]'):
        try:
            vram = int(open(card + '/device/mem_info_vram_total').read())
        except OSError:
            continue
        if vram > size and os.path.exists(card + '/device/gpu_busy_percent'):
            best, size = card + '/device/gpu_busy_percent', vram
    return best


def thread_times(pid):
    times = {}
    for task in glob.glob(f'/proc/{pid}/task/*'):
        try:
            comm = open(task + '/comm').read().strip()
            fields = open(task + '/stat').read().rsplit(')', 1)[1].split()
        except OSError:
            continue
        # fields[11], [12] = utime, stime (stat fields 14 and 15)
        times[comm] = times.get(comm, 0) + int(fields[11]) + int(fields[12])
    return times


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--gpu', default='auto')
    ap.add_argument('--wait', type=float, default=60.0, help='seconds to wait for the game to start')
    ap.add_argument('--max-seconds', type=float, default=0.0, help='stop after this long (0: when the game exits)')
    args = ap.parse_args()
    pid = find_pid(args.wait)
    if pid is None:
        sys.exit('no dw5xl process')
    busy_path = gpu_path(args.gpu)
    names, rows = [], []
    prev, prev_t = thread_times(pid), time.monotonic()
    start = prev_t
    while os.path.exists(f'/proc/{pid}') and not (args.max_seconds and prev_t - start >= args.max_seconds):
        gpu = []
        for _ in range(10):
            time.sleep(0.1)
            if busy_path:
                try:
                    gpu.append(int(open(busy_path).read()))
                except OSError:
                    pass
        now, now_t = thread_times(pid), time.monotonic()
        if not now:
            break
        dt = now_t - prev_t
        load = {k: 100.0 * (v - prev.get(k, 0)) / TICK / dt for k, v in now.items()}
        for k in load:
            if k not in names:
                names.append(k)
        rows.append((now_t - start, load, sum(gpu) / len(gpu) if gpu else -1.0))
        prev, prev_t = now, now_t
    with open(args.out, 'w') as f:
        f.write('t,gpu_busy,' + ','.join(names) + '\n')
        for t, load, gpu in rows:
            f.write(f'{t:.1f},{gpu:.1f},' + ','.join(f'{load.get(n, 0.0):.1f}' for n in names) + '\n')
    print(f'{len(rows)} samples -> {args.out}')


if __name__ == '__main__':
    main()
