#!/usr/bin/env python3
"""Windows recorder export -> samples.jsonl (profiling tools; format in CONTRACT.md).

Input: the filtered xperf dumper text (`-a dumper -stacktimeshifting`, game processes only) and the anchor lines from
`-add_rawdata` ('# cpu # 0xQPC # TimeStamp[ns] # key'). Dumper times are microseconds since the recording started;
the anchors give the QPC tick of that start, and the marker anchors (or `--qpc-freq`) give ticks per second, so every
event is placed on steady_clock nanoseconds (the clock of the game markers and the fixture).
Output lines: process, thread, module, sample, switch, ready (see CONTRACT.md). Stack addresses are raw; report.py
symbolizes them.

Usage: etw_samples.py <events.txt> <anchors.txt> <out samples.jsonl> [--markers markers.bin ...] [--qpc-freq N]"""
import argparse, json, re, sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

PROC = re.compile(r'^(.*?)\s*\(\s*(\d+)\)$')
EVENT_TYPES = ('SampledProfile', 'CSwitch', 'ReadyThread', 'ThreadName', 'T-Start', 'T-DCStart', 'I-Start',
               'I-DCStart', 'P-Start', 'P-DCStart', 'Stack')


def proc(field):
    m = PROC.match(field.strip())
    return (m.group(1).strip(), int(m.group(2))) if m else (field.strip(), -1)


def hexint(s):
    s = s.strip()
    return int(s, 16) if s.lower().startswith('0x') else int(s)


def qpc_origin(anchor_lines, freq):
    """QPC tick at trace time 0, from '# cpu # 0xQPC # ns # key' lines (median over lines)."""
    vals = []
    for line in anchor_lines:
        parts = [p.strip() for p in line.strip().lstrip('#').split('#')]
        if len(parts) >= 3 and parts[1].lower().startswith('0x'):
            try:
                vals.append(int(parts[1], 16) - int(parts[2]) * freq // 1_000_000_000)
            except ValueError:
                pass
    vals.sort()
    return vals[len(vals) // 2] if vals else None


def estimate_freq(anchor_lines):
    pts = []
    for line in anchor_lines:
        parts = [p.strip() for p in line.strip().lstrip('#').split('#')]
        if len(parts) >= 3 and parts[1].lower().startswith('0x'):
            pts.append((int(parts[2]), int(parts[1], 16)))
    if len(pts) < 2 or pts[-1][0] == pts[0][0]:
        return None
    return round((pts[-1][1] - pts[0][1]) * 1_000_000_000 / (pts[-1][0] - pts[0][0]))


def convert(events_path, anchor_lines, out_path, freq, steady_offset):
    origin = qpc_origin(anchor_lines, freq)
    if origin is None:
        raise SystemExit('no usable anchor lines (QPC) in the export')

    def t_ns(us):
        ticks = origin + int(us) * freq // 1_000_000
        return ticks * 1_000_000_000 // freq + steady_offset

    counts = {}
    pending = {}   # (time, tid) -> [event dicts] waiting for their Stack lines (the last one gets them)
    out = open(out_path, 'w', encoding='utf-8')

    def emit(rec):
        counts[rec['type']] = counts.get(rec['type'], 0) + 1
        out.write(json.dumps(rec, separators=(',', ':')) + '\n')

    def flush_pending():
        for recs in pending.values():
            for rec in recs:
                rec['stack'] = [a for _, a in sorted(rec.pop('_frames', []))]
                emit(rec)
        pending.clear()

    def hold(key, rec):
        pending.setdefault(key, []).append(rec)

    header = True
    last_time = None
    with open(events_path, encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.strip()
            if header:
                if line == 'EndHeader':
                    header = False
                continue
            kind, _, rest = line.partition(',')
            kind = kind.strip()
            if kind not in EVENT_TYPES:
                continue
            parts = [p.strip() for p in rest.split(',')]
            try:
                us = int(parts[0])
            except (ValueError, IndexError):
                continue
            if kind == 'Stack':
                # Stack, time, thread, No., address, Image!Function
                recs = pending.get((us, int(parts[1])))
                if recs:
                    recs[-1].setdefault('_frames', []).append((int(parts[2]), hexint(parts[3])))
                continue
            if last_time is not None and us != last_time:
                flush_pending()
            last_time = us
            t = t_ns(us)
            if kind == 'SampledProfile':
                name, pid = proc(parts[1])
                tid = int(parts[2])
                hold((us, tid), {'type': 'sample', 't_ns': t, 'pid': pid, 'tid': tid, 'process': name,
                                      'pc': hexint(parts[3])})
            elif kind == 'CSwitch':
                nname, npid = proc(parts[1])
                oname, opid = proc(parts[7])
                ntid, otid = int(parts[2]), int(parts[8])
                hold((us, ntid), {'type': 'switch', 't_ns': t, 'new_pid': npid, 'new_tid': ntid,
                                       'new_process': nname, 'old_pid': opid, 'old_tid': otid, 'old_process': oname,
                                       'old_state': parts[11], 'wait_reason': parts[12]})
            elif kind == 'ReadyThread':
                name, pid = proc(parts[1])
                rname, rpid = proc(parts[3])
                tid = int(parts[2])
                hold((us, tid), {'type': 'ready', 't_ns': t, 'pid': pid, 'tid': tid, 'process': name,
                                      'rdy_pid': rpid, 'rdy_tid': int(parts[4]), 'rdy_process': rname,
                                      'reason': parts[5]})
            elif kind == 'ThreadName':
                name, pid = proc(parts[1])
                emit({'type': 'thread', 't_ns': t, 'pid': pid, 'tid': int(parts[2]), 'process': name,
                      'name': ','.join(parts[3:]).strip()})
            elif kind in ('T-Start', 'T-DCStart'):
                name, pid = proc(parts[1])
                emit({'type': 'thread', 't_ns': t, 'pid': pid, 'tid': int(parts[2]), 'process': name, 'name': None})
            elif kind in ('I-Start', 'I-DCStart'):
                name, pid = proc(parts[1])
                emit({'type': 'module', 't_ns': t, 'pid': pid, 'process': name, 'base': hexint(parts[2]),
                      'end': hexint(parts[3]), 'path': parts[7].strip('"')})
            elif kind in ('P-Start', 'P-DCStart'):
                name, pid = proc(parts[1])
                emit({'type': 'process', 't_ns': t, 'pid': pid, 'process': name})
    flush_pending()
    out.close()
    return counts


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('events')
    ap.add_argument('anchors')
    ap.add_argument('out')
    ap.add_argument('--markers', action='append', default=[])
    ap.add_argument('--qpc-freq', type=int)
    a = ap.parse_args(argv)
    anchor_lines = [l for l in open(a.anchors, encoding='utf-8', errors='replace') if l.startswith('#')]
    marker_anchors = []
    for m in a.markers:
        h, _ = common.read_markers(m)
        if h:
            marker_anchors += h['anchors']
    freq = a.qpc_freq or next((x['os_freq'] for x in marker_anchors if x.get('os_freq')), None) or \
        estimate_freq(anchor_lines)
    if not freq:
        raise SystemExit('QPC frequency unknown: give --markers or --qpc-freq')
    counts = convert(a.events, anchor_lines, a.out, freq, common.steady_offset_ns(marker_anchors))
    print(json.dumps({'qpc_freq': freq, 'records': counts}))


if __name__ == '__main__':
    main()
