#!/usr/bin/env python3
"""Summarise / compare run logs with PS2X_STATS and PS2X_EE_DISPATCH_TRACE output.

DW5XL-TEST helper for sync vs async (PS2X_VU1_ASYNC=0/1) performance A/B runs.

    tools/perf_ab.py A.log                       # summary of one run
    tools/perf_ab.py A.log B.log                 # A/B comparison (B relative to A)
    tools/perf_ab.py A.log B.log --from-tick 1000 --to-tick 5000 --labels sync,async
    tools/perf_ab.py A.log --per-window          # one row per dispatch window
    tools/perf_ab.py A.log B.log --tag vu1:worker   # also summarise "[vu1:worker] k=v ..." lines

Parsed lines (anywhere in a line; terminal escape noise is ignored):
    [stats] t=<s> tick=<n> vsync/s=<x> flips/s=<y>
    [ee:dispatch-time] field=<tick> windowMs=<ms> tid=<id> callback=<0|1> calls=<n> hostNs=<ns>
                       dmaCalls=<n> dmaNs=<ns> dmaMaxNs=<ns>
A dispatch window is one ~1 s flush: consecutive lines with the same field/windowMs.
Per-thread values are averaged over all windows in range (a thread absent from a
window counts as 0). busy% = hostNs / window wall time. Tick filters use the
stats tick and the dispatch field (= vsync tick at flush).
"""

import argparse
import math
import re
import statistics
import sys
from collections import OrderedDict, defaultdict

STATS_RE = re.compile(r"\[stats\] t=([\d.]+) tick=(\d+) vsync/s=([\d.]+) flips/s=([\d.]+)")
DISPATCH_RE = re.compile(r"\[ee:dispatch-time\]((?: [A-Za-z_]+=-?[\d.]+)+)")
KV_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(-?[\d.]+(?:e[+-]?\d+)?)")


def number(text):
    try:
        return int(text)
    except ValueError:
        return float(text)


def percentile(values, fraction):
    if not values:
        return math.nan
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(math.ceil(fraction * len(ordered))) - 1))
    return ordered[index]


class Run:
    def __init__(self, path, label, from_tick, to_tick, tags):
        self.path = path
        self.label = label
        self.stats = []  # (t, tick, vsync, flips)
        self.windows = []  # dicts: field, windowMs, threads{(tid, cb): {...}}
        self.tags = {tag: [] for tag in tags}
        self.parse(from_tick, to_tick)

    def in_range(self, tick, from_tick, to_tick):
        return (from_tick is None or tick >= from_tick) and (to_tick is None or tick <= to_tick)

    def parse(self, from_tick, to_tick):
        tag_res = {tag: re.compile(r"\[" + re.escape(tag) + r"\]((?: [A-Za-z_][A-Za-z0-9_]*=[^\s]+)+)")
                   for tag in self.tags}
        current = None
        with open(self.path, "r", errors="replace") as handle:
            for line in handle:
                match = STATS_RE.search(line)
                if match:
                    t, tick, vsync, flips = match.groups()
                    if self.in_range(int(tick), from_tick, to_tick):
                        self.stats.append((float(t), int(tick), float(vsync), float(flips)))
                    continue
                match = DISPATCH_RE.search(line)
                if match:
                    kv = {k: number(v) for k, v in KV_RE.findall(match.group(1))}
                    field = kv.get("field", 0)
                    window_ms = kv.get("windowMs", 0)
                    if current is None or current["field"] != field or current["windowMs"] != window_ms:
                        current = {"field": field, "windowMs": window_ms, "threads": {}}
                        if self.in_range(field, from_tick, to_tick):
                            self.windows.append(current)
                    key = (kv.get("tid", 0), kv.get("callback", 0))
                    current["threads"][key] = kv
                    continue
                for tag, tag_re in tag_res.items():
                    match = tag_re.search(line)
                    if match:
                        kv = {k: number(v) for k, v in KV_RE.findall(match.group(1))}
                        tick = kv.get("field", kv.get("tick"))
                        if tick is None or self.in_range(tick, from_tick, to_tick):
                            self.tags[tag].append(kv)

    # ---- summaries -------------------------------------------------------
    def vsync_summary(self):
        vs = [s[2] for s in self.stats]
        flips = [s[3] for s in self.stats]
        if not vs:
            return OrderedDict()
        out = OrderedDict()
        out["stats samples"] = len(vs)
        out["tick range"] = "%d..%d" % (self.stats[0][1], self.stats[-1][1])
        out["vsync/s mean"] = statistics.fmean(vs)
        out["vsync/s median"] = statistics.median(vs)
        out["vsync/s p5"] = percentile(vs, 0.05)
        out["vsync/s min"] = min(vs)
        out["samples <49.5 vsync/s"] = sum(1 for v in vs if v < 49.5)
        out["flips/s mean"] = statistics.fmean(flips)
        return out

    def thread_keys(self):
        keys = set()
        for window in self.windows:
            keys.update(window["threads"].keys())
        return sorted(keys)

    def thread_summary(self):
        """Per (tid, callback): averages per window."""
        n = len(self.windows)
        result = OrderedDict()
        if n == 0:
            return result
        for key in self.thread_keys():
            calls = host = dma_calls = dma = 0
            dma_max = 0
            busy = []
            present = 0
            for window in self.windows:
                kv = window["threads"].get(key)
                wall_ns = max(1, window["windowMs"]) * 1e6
                if kv is None:
                    busy.append(0.0)
                    continue
                present += 1
                calls += kv.get("calls", 0)
                host += kv.get("hostNs", 0)
                dma_calls += kv.get("dmaCalls", 0)
                dma += kv.get("dmaNs", 0)
                dma_max = max(dma_max, kv.get("dmaMaxNs", 0))
                busy.append(100.0 * kv.get("hostNs", 0) / wall_ns)
            result[key] = OrderedDict([
                ("calls/win", calls / n),
                ("hostMs/win", host / n / 1e6),
                ("busy%", statistics.fmean(busy)),
                ("busy% max", max(busy)),
                ("dmaCalls/win", dma_calls / n),
                ("dmaMs/win", dma / n / 1e6),
                ("dmaMaxMs", dma_max / 1e6),
                ("windows", present),
            ])
        return result

    def total_summary(self):
        n = len(self.windows)
        if n == 0:
            return OrderedDict()
        host = [sum(kv.get("hostNs", 0) for kv in w["threads"].values()) / 1e6 for w in self.windows]
        wall = [w["windowMs"] for w in self.windows]
        base = [sum(kv.get("hostNs", 0) for (tid, cb), kv in w["threads"].items() if cb == 0) / 1e6
                for w in self.windows]
        cbk = [sum(kv.get("hostNs", 0) for (tid, cb), kv in w["threads"].items() if cb != 0) / 1e6
               for w in self.windows]
        calls = [sum(kv.get("calls", 0) for kv in w["threads"].values()) for w in self.windows]
        out = OrderedDict()
        out["dispatch windows"] = n
        out["field range"] = "%d..%d" % (self.windows[0]["field"], self.windows[-1]["field"])
        out["window ms mean"] = statistics.fmean(wall)
        out["EE host ms/win (all)"] = statistics.fmean(host)
        out["EE host ms/win (base)"] = statistics.fmean(base)
        out["EE host ms/win (callback)"] = statistics.fmean(cbk)
        out["EE host ms/win max"] = max(host)
        out["EE busy% mean"] = statistics.fmean(100.0 * h / max(1, w) for h, w in zip(host, wall))
        out["dispatch calls/win"] = statistics.fmean(calls)
        return out

    def tag_summary(self, tag):
        rows = self.tags.get(tag, [])
        out = OrderedDict()
        if not rows:
            return out
        keys = []
        for row in rows:
            for key in row:
                if key not in keys:
                    keys.append(key)
        out["lines"] = len(rows)
        for key in keys:
            values = [row[key] for row in rows if key in row]
            if key in ("field", "tick", "windowMs", "tid", "callback"):
                continue
            out[key + " mean"] = statistics.fmean(values)
            out[key + " max"] = max(values)
        return out


def thread_name(key):
    tid, cb = key
    return "tid=%s %s" % (tid, "callback" if cb else "base")


def fmt(value):
    if isinstance(value, str):
        return value
    if isinstance(value, float):
        if math.isnan(value):
            return "-"
        if abs(value) >= 1000:
            return "%.0f" % value
        return "%.2f" % value
    return str(value)


def delta(a, b):
    if isinstance(a, str) or isinstance(b, str) or a is None or b is None:
        return ""
    if a == 0:
        return "" if b == 0 else "new"
    return "%+.1f%%" % (100.0 * (b - a) / abs(a))


def print_section(title, runs, getter):
    tables = [getter(run) for run in runs]
    keys = []
    for table in tables:
        for key in table:
            if key not in keys:
                keys.append(key)
    if not keys:
        return
    print("\n" + title)
    labels = [run.label for run in runs]
    header = "  %-28s" % "" + "".join("%14s" % label[:14] for label in labels)
    if len(runs) == 2:
        header += "%10s" % "B vs A"
    print(header)
    for key in keys:
        values = [table.get(key) for table in tables]
        line = "  %-28s" % key + "".join("%14s" % ("-" if v is None else fmt(v)) for v in values)
        if len(runs) == 2:
            line += "%10s" % delta(values[0], values[1])
        print(line)


def print_threads(runs):
    summaries = [run.thread_summary() for run in runs]
    keys = sorted(set().union(*[set(s.keys()) for s in summaries]))
    if not keys:
        print("\n(no [ee:dispatch-time] lines in range; run with PS2X_EE_DISPATCH_TRACE=1)")
        return
    print("\nPer EE thread (averages over all dispatch windows in range; 'windows' = windows where it ran)")
    if len(runs) == 1:
        metrics = ["calls/win", "hostMs/win", "busy%", "busy% max", "dmaCalls/win", "dmaMs/win", "dmaMaxMs", "windows"]
        print("  %-18s" % "thread" + "".join("%13s" % m for m in metrics))
        for key in keys:
            row = summaries[0][key]
            print("  %-18s" % thread_name(key) + "".join("%13s" % fmt(row[m]) for m in metrics))
        return
    metrics = ["calls/win", "hostMs/win", "busy%", "dmaMs/win"]
    a_label, b_label = runs[0].label[:6], runs[1].label[:6]
    print("  %-18s" % "" + "".join("%31s" % ("-- " + metric + " --") for metric in metrics))
    print("  %-18s" % "thread" + "".join("%12s%10s%9s" % (a_label, b_label, "delta") for _ in metrics))
    for key in keys:
        line = "  %-18s" % thread_name(key)
        for metric in metrics:
            a = summaries[0].get(key, {}).get(metric)
            b = summaries[1].get(key, {}).get(metric)
            line += "%12s%10s%9s" % ("-" if a is None else fmt(a), "-" if b is None else fmt(b),
                                      delta(a if a is not None else 0, b if b is not None else 0))
        print(line)


def print_windows(run):
    keys = run.thread_keys()
    print("\nPer-window host ms: %s (%s)" % (run.label, run.path))
    print("  %8s %8s %9s " % ("field", "winMs", "totalMs") + " ".join("%14s" % thread_name(k) for k in keys))
    stats_by_tick = {s[1]: s for s in run.stats}
    for window in run.windows:
        total = sum(kv.get("hostNs", 0) for kv in window["threads"].values()) / 1e6
        cells = " ".join("%14s" % ("%.2f" % (window["threads"][k]["hostNs"] / 1e6) if k in window["threads"] else "-")
                         for k in keys)
        print("  %8s %8s %9.2f %s" % (window["field"], window["windowMs"], total, cells))
    del stats_by_tick


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logs", nargs="+", help="one or two run logs (A [B])")
    parser.add_argument("--labels", help="comma-separated labels (default A,B)")
    parser.add_argument("--from-tick", type=int)
    parser.add_argument("--to-tick", type=int)
    parser.add_argument("--per-window", action="store_true", help="print one row per dispatch window")
    parser.add_argument("--tag", action="append", default=[],
                        help="also summarise numeric key=value fields of '[TAG] ...' lines (repeatable)")
    args = parser.parse_args()
    if len(args.logs) > 2:
        parser.error("give one or two logs")
    labels = args.labels.split(",") if args.labels else ["A", "B"][: len(args.logs)]
    if len(labels) != len(args.logs):
        parser.error("--labels needs one label per log")

    runs = [Run(path, label, args.from_tick, args.to_tick, args.tag) for path, label in zip(args.logs, labels)]
    for run in runs:
        print("%s = %s  (%d stats samples, %d dispatch windows)" % (run.label, run.path, len(run.stats),
                                                                   len(run.windows)))
    if args.from_tick is not None or args.to_tick is not None:
        print("tick filter: %s..%s" % (args.from_tick, args.to_tick))
    print_section("Frame pacing ([stats])", runs, Run.vsync_summary)
    print_section("EE dispatch totals ([ee:dispatch-time])", runs, Run.total_summary)
    print_threads(runs)
    for tag in args.tag:
        print_section("[%s]" % tag, runs, lambda run, tag=tag: run.tag_summary(tag))
    if args.per_window:
        for run in runs:
            print_windows(run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
