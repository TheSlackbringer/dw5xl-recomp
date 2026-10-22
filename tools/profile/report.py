#!/usr/bin/env python3
"""samples.jsonl (+ markers) -> report.json and report.txt (profiling tools; format in CONTRACT.md).

Function names: stack addresses are rebased to each module's file ImageBase and looked up with llvm-symbolizer
(PDB next to the module when present, else the file's own debug info). Modules without names show as
"<module file>", kernel addresses as "[kernel]". A frame that is already a name (Linux: perf resolves them) is
only normalized.

Usage: report.py <samples.jsonl> <out dir> [--markers markers.bin ...] [--programs a.exe,b.exe]"""
import argparse, bisect, collections, json, ntpath, os, re, statistics, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

KERNEL = 0xFFFF800000000000
SPAN_GAP_INTERVALS = 5


def normalize(name):
    """Demangled base name: no anonymous-namespace prefix, no parameter list."""
    name = name.replace("`anonymous namespace'::", '').replace('(anonymous namespace)::', '')
    depth = 0
    for i, ch in enumerate(name):
        if ch in '<':
            depth += 1
        elif ch == '>':
            depth -= 1
        elif ch == '(' and depth == 0 and i > 0:
            return name[:i].strip()
    return name.strip()


class Symbols:
    def __init__(self, modules, search_dirs=()):
        self.search_dirs = list(search_dirs)
        # pid -> sorted [(base, end, path)]
        self.modules = collections.defaultdict(list)
        for m in modules:
            self.modules[m['pid']].append((m['base'], m['end'], m['path']))
        for v in self.modules.values():
            v.sort()
        self.cache = {}
        self.symbolizer = common.tool_path('llvm-symbolizer')

    def module_of(self, pid, addr):
        for base, end, path in self.modules.get(pid, []):
            if base <= addr < end:
                return base, path
        return None, None

    def resolve_all(self, wanted):
        """wanted: {(pid, addr)}; fills self.cache[(pid, addr)] = label."""
        by_file = collections.defaultdict(list)
        for pid, addr in wanted:
            if isinstance(addr, str) or (pid, addr) in self.cache:
                continue
            if addr >= KERNEL:
                self.cache[(pid, addr)] = '[kernel]'
                continue
            base, path = self.module_of(pid, addr)
            if path is None:
                self.cache[(pid, addr)] = '[unknown]'
                continue
            by_file[(path, base)].append((pid, addr))
        for (path, base), items in by_file.items():
            label = ntpath.basename(path)   # recorded paths may be Windows paths (a Windows capture read on Linux)
            local = path if os.path.exists(path) else next(
                (os.path.join(d, label) for d in self.search_dirs if os.path.exists(os.path.join(d, label))), None)
            image_base = common.pe_image_base(local) if local else None
            names = {}
            if local and self.symbolizer and image_base is not None:
                pdb = os.path.splitext(local)[0] + '.pdb'
                cmd = [self.symbolizer, f'--obj={local}', '-C', '--inlining=false']
                if os.path.exists(pdb):
                    cmd.append(f'--pdb={pdb}')
                addrs = sorted({a for _, a in items})
                text = subprocess.run(cmd, input='\n'.join(hex(a - base + image_base) for a in addrs) + '\n',
                                      capture_output=True, text=True).stdout
                blocks = [b.strip().splitlines() for b in text.strip().split('\n\n')]
                for a, block in zip(addrs, blocks):
                    if block and block[0] not in ('??', ''):
                        names[a] = normalize(block[0])
            for pid, addr in items:
                self.cache[(pid, addr)] = names.get(addr, f'<{label}>')

    def label(self, pid, addr):
        if isinstance(addr, str):
            return addr if addr.startswith(('[', '<')) else normalize(addr)
        return self.cache.get((pid, addr), '[unknown]')


def build(records, marker_files, programs, search_dirs=()):
    threads, names, modules = {}, {}, []
    samples, switches, readies = [], [], []
    for r in records:
        t = r['type']
        if t == 'thread':
            threads.setdefault((r['pid'], r['tid']), r['process'])
            if r.get('name'):
                names[(r['pid'], r['tid'])] = r['name']
        elif t == 'module':
            modules.append(r)
        elif t == 'sample':
            samples.append(r)
        elif t == 'switch':
            switches.append(r)
        elif t == 'ready':
            readies.append(r)
    ours = {r['pid'] for r in samples if not programs or r['process'] in programs}
    ours |= {pid for (pid, _), proc in threads.items() if not programs or proc in programs}
    sym = Symbols(modules, search_dirs)
    wanted = {(r['pid'], a) for r in samples for a in r['stack'] or [r['pc']]}
    wanted |= {(r['new_pid'], a) for r in switches for a in r.get('stack', [])}
    wanted |= {(r['pid'], a) for r in readies for a in r.get('stack', [])}
    sym.resolve_all(wanted)

    def stack_labels(pid, stack):
        return [sym.label(pid, a) for a in stack]

    # sample interval: median gap between consecutive samples of the same thread (gaps over 5 ms ignored)
    by_thread = collections.defaultdict(list)
    for r in samples:
        by_thread[(r['pid'], r['tid'])].append(r)
    gaps = []
    for lst in by_thread.values():
        lst.sort(key=lambda r: r['t_ns'])
        gaps += [b['t_ns'] - a['t_ns'] for a, b in zip(lst, lst[1:]) if 0 < b['t_ns'] - a['t_ns'] < 5_000_000]
    interval = int(statistics.median(gaps)) if gaps else 1_000_000

    report_threads, spans = [], []
    for key in sorted(set(by_thread) | {k for k in threads if k[0] in ours}):
        pid, tid = key
        lst = by_thread.get(key, [])
        self_c, total_c = collections.Counter(), collections.Counter()
        leaf_runs = collections.defaultdict(list)
        for r in lst:
            labels = stack_labels(pid, r['stack'] or [r['pc']])
            # own time: the first frame outside the kernel (a sample taken in a system call is charged to the
            # user function that made it)
            leaf = next((l for l in labels if l != '[kernel]'), labels[0] if labels else '[unknown]')
            self_c[leaf] += 1
            for l in set(labels):
                total_c[l] += 1
            leaf_runs[leaf].append(r['t_ns'])
        for fn, times in leaf_runs.items():
            start = prev = times[0]
            for t in times[1:]:
                if t - prev > SPAN_GAP_INTERVALS * interval:
                    spans.append({'pid': pid, 'tid': tid, 'function': fn, 'start_ns': start, 'end_ns': prev})
                    start = t
                prev = t
            spans.append({'pid': pid, 'tid': tid, 'function': fn, 'start_ns': start, 'end_ns': prev})
        report_threads.append({'name': names.get(key, str(tid)), 'process': threads.get(key) or
                               (lst[0]['process'] if lst else None), 'pid': pid, 'tid': tid, 'samples': len(lst),
                               'self': self_c.most_common(), 'total': total_c.most_common(), 'waits': []})
    index = {(t['pid'], t['tid']): t for t in report_threads}

    # waits: switched out while Waiting -> switched in again; the waker is the last ReadyThread in between
    switches.sort(key=lambda r: r['t_ns'])
    readies.sort(key=lambda r: r['t_ns'])
    woken = collections.defaultdict(list)       # woken thread -> its ReadyThread events, in time order
    for r in readies:
        woken[(r['rdy_pid'], r['rdy_tid'])].append(r)
    woken_t = {k: [r['t_ns'] for r in v] for k, v in woken.items()}
    out_at = {}
    partners = set()
    for s in switches:
        old, new = (s['old_pid'], s['old_tid']), (s['new_pid'], s['new_tid'])
        if old in index and s['old_state'] == 'Waiting':
            out_at[old] = s['t_ns']
        if new in index and new in out_at:
            start, end = out_at.pop(new), s['t_ns']
            ts = woken_t.get(new, [])
            i = bisect.bisect_right(ts, end) - 1
            w = woken[new][i] if i >= 0 and ts[i] >= start else None
            index[new]['waits'].append({
                'start_ns': start, 'end_ns': end,
                'waker_thread': (names.get((w['pid'], w['tid'])) or (str(w['tid']) if w['pid'] in ours else None)) if w else None,
                'waker_process': w['process'] if w else None,
                'waker_stack': stack_labels(w['pid'], w.get('stack', [])) if w and w['pid'] in ours else []})
        for side in ((s['old_pid'], s['old_process']), (s['new_pid'], s['new_process'])):
            if side[0] not in ours:
                partners.add(side[1])
    for r in readies:
        if r['pid'] not in ours and r['rdy_pid'] in ours:
            partners.add(r['process'])

    markers = []
    for path in marker_files:
        _, recs = common.read_markers(path)
        markers += [{'event': m['event'], 'seq': m['seq'], 'tid': m['tid'], 't_ns': m['t_ns'], 'kind': m['kind'],
                     'program': m['program'], 'vu1_cycles': m['vu1_cycles']} for m in recs]
    markers.sort(key=lambda m: m['t_ns'])
    return {'schema': 1, 'sample_interval_ns': interval, 'threads': report_threads, 'spans': spans,
            'markers': markers, 'processes_with_samples': sorted({r['process'] for r in samples}),
            'switch_partner_processes': sorted(partners - {None})}


def text_report(rep):
    lines = [f'sample interval {rep["sample_interval_ns"] / 1e6:.3f} ms; processes with samples: '
             f'{", ".join(rep["processes_with_samples"]) or "none"}', '']
    for t in sorted(rep['threads'], key=lambda t: -t['samples']):
        if not t['samples'] and not t['waits']:
            continue
        lines.append(f'{t["process"]} pid {t["pid"]} thread {t["name"]} ({t["tid"]}): {t["samples"]} samples, '
                     f'{len(t["waits"])} waits')
        for fn, n in t['self'][:10]:
            lines.append(f'    {n:7d}  {100 * n / max(1, t["samples"]):5.1f} %  {fn}')
    lines.append('')
    lines.append(f'markers: {len(rep["markers"])}; switch partners: {", ".join(rep["switch_partner_processes"]) or "none"}')
    return '\n'.join(lines) + '\n'


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('samples')
    ap.add_argument('out')
    ap.add_argument('--markers', action='append', default=[])
    ap.add_argument('--programs', default=','.join(common.ALLOWED_PROGRAMS))
    ap.add_argument('--symbol-dir', action='append', default=[], help='where to look for a module missing at its recorded path')
    a = ap.parse_args(argv)
    records = [json.loads(l) for l in open(a.samples, encoding='utf-8') if l.strip()]
    rep = build(records, a.markers, set(p for p in a.programs.split(',') if p), a.symbol_dir)
    os.makedirs(a.out, exist_ok=True)
    common.write_json(os.path.join(a.out, 'report.json'), rep)
    with open(os.path.join(a.out, 'report.txt'), 'w', encoding='utf-8') as f:
        f.write(text_report(rep))
    print(f'report: {len(rep["threads"])} threads, {sum(t["samples"] for t in rep["threads"])} samples, '
          f'interval {rep["sample_interval_ns"]} ns')


if __name__ == '__main__':
    main()
