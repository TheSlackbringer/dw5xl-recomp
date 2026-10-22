#!/usr/bin/env python3
"""Linux recorder export -> samples.jsonl (profiling tools; format in CONTRACT.md).

Input: `perf script` text (perf_record.script_command: --ns, callchains, task/switch/mmap/lost events) of a recording
made with `-k CLOCK_MONOTONIC`. Perf times are CLOCK_MONOTONIC nanoseconds, the clock steady_clock uses on Linux; the
marker anchors give the (near zero) offset, so every event lands on steady_clock nanoseconds as on Windows.
Only programs on the allowed list are kept (as on Windows, other processes' data never reaches samples.jsonl).
Process names come from the executable mapped after exec (perf's comm is cut to 15 characters); thread names from
comm (pthread_setname_np). Stack frames are the names perf resolved (strings; Windows has addresses).
Switches: "OUT" without "preempt" = the thread went to sleep (old_state "Waiting"), "IN" ends it. Wake-ups
(`ready`) come from sched:sched_waking, recorded in the waker's context, when the recording has them.

Usage: perf_samples.py <perf script text> <out samples.jsonl> [--markers markers.bin ...] [--programs a,b]"""
import argparse, json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

HEAD = re.compile(r'^\s*(?P<comm>.*?)\s+(?P<pid>-?\d+)/(?P<tid>-?\d+)\s+(?:\[(?P<cpu>\d+)\]\s+)?'
                  r'(?P<sec>\d+)\.(?P<frac>\d+):\s*(?P<rest>.*?)\s*$')
FRAME = re.compile(r'^\s+(?P<addr>[0-9a-fA-F]+)\s+(?P<sym>.*?)\s+\((?P<dso>[^()]*)\)\s*$')
INLINE_FRAME = re.compile(r'(?P<addr>[0-9a-fA-F]+)\s+(?P<sym>.*?)\s+\((?P<dso>[^()]*)\)\s*$')
COMM = re.compile(r'^PERF_RECORD_COMM( exec)?: (?P<name>.*):(?P<pid>-?\d+)/(?P<tid>-?\d+)$')
FORK = re.compile(r'^PERF_RECORD_FORK\((?P<pid>-?\d+):(?P<tid>-?\d+)\):\((?P<ppid>-?\d+):(?P<ptid>-?\d+)\)')
MMAP = re.compile(r'^PERF_RECORD_MMAP2? (?P<pid>-?\d+)/(?P<tid>-?\d+): \[(?P<start>0x[0-9a-fA-F]+)\((?P<len>0x[0-9a-fA-F]+)\)'
                  r' @ \S+[^\]]*\]: (?P<prot>[r-][w-][x-])\S* (?P<path>.*)$')
LOST = re.compile(r'^PERF_RECORD_LOST(?:_SAMPLES)?\D*(?P<n>\d+)')
SYMOFF = re.compile(r'\+0x[0-9a-fA-F]+$')


def frame_name(sym, dso):
    if dso.startswith('[kernel') or sym.startswith('[kernel'):
        return '[kernel]'
    sym = SYMOFF.sub('', sym.strip())
    if not sym or sym == '[unknown]':
        return f'<{os.path.basename(dso)}>' if dso and dso != '[unknown]' else '[unknown]'
    return sym


def convert(lines, out_path, programs, steady_offset=0, cap_bytes=None):
    """perf script lines -> samples.jsonl; keeps only processes whose file name is in `programs`.
    Returns counts per record type (+ 'lost', and 'truncated' when the cap stopped the output)."""
    counts = {}
    written = 0
    truncated = False
    pid_of = {}          # tid -> pid
    exec_comm = {}       # pid -> comm at exec (15 characters)
    proc_name = {}       # pid -> executable file name
    seen_threads = set()
    out = open(out_path, 'w', encoding='utf-8')

    def name_of(pid):
        return proc_name.get(pid) or exec_comm.get(pid)

    def ours(pid):
        return name_of(pid) in programs

    def emit(rec):
        nonlocal written, truncated
        if truncated:
            return
        line = json.dumps(rec, separators=(',', ':')) + '\n'
        if cap_bytes is not None and written + len(line) > cap_bytes:
            truncated = True
            return
        written += len(line)
        counts[rec['type']] = counts.get(rec['type'], 0) + 1
        out.write(line)

    def thread(t, pid, tid, name):
        if ours(pid):
            emit({'type': 'thread', 't_ns': t, 'pid': pid, 'tid': tid, 'process': name_of(pid), 'name': name})
            seen_threads.add((pid, tid))

    pending = None   # sample or wake-up waiting for its callchain lines

    def finish():
        nonlocal pending
        if pending is not None:
            rec, frames = pending
            pending = None
            if frames:
                rec['pc'] = frames[0][0]
                rec['stack'] = [n for _, n in frames]
            if ours(rec['pid']):
                if rec['type'] == 'sample' and (rec['pid'], rec['tid']) not in seen_threads:
                    thread(rec['t_ns'], rec['pid'], rec['tid'], rec.pop('_comm'))
                rec.pop('_comm', None)
                if rec['type'] == 'ready':
                    rec['rdy_pid'] = pid_of.get(rec['rdy_tid'], -1)
                    rec['rdy_process'] = name_of(rec['rdy_pid'])
                emit(rec)

    for raw in lines:
        if raw.startswith('#'):
            continue
        if pending is not None:
            m = FRAME.match(raw)
            if m:
                pending[1].append((int(m.group('addr'), 16), frame_name(m.group('sym'), m.group('dso'))))
                continue
            finish()
        m = HEAD.match(raw)
        if not m:
            continue
        pid, tid = int(m.group('pid')), int(m.group('tid'))
        t = int(m.group('sec')) * 1_000_000_000 + int(m.group('frac').ljust(9, '0')[:9]) + steady_offset
        rest = m.group('rest')
        if pid > 0 and tid > 0:
            pid_of[tid] = pid
        if rest.startswith('PERF_RECORD_'):
            if rest.startswith('PERF_RECORD_SWITCH'):
                if 'CPU_WIDE' in rest or not ours(pid):
                    continue
                if ' OUT' in rest:
                    emit({'type': 'switch', 't_ns': t, 'old_pid': pid, 'old_tid': tid, 'old_process': name_of(pid),
                          'old_state': 'Ready' if 'preempt' in rest else 'Waiting', 'wait_reason': None,
                          'new_pid': None, 'new_tid': None, 'new_process': None})
                else:
                    emit({'type': 'switch', 't_ns': t, 'new_pid': pid, 'new_tid': tid, 'new_process': name_of(pid),
                          'old_pid': None, 'old_tid': None, 'old_process': None, 'old_state': None, 'wait_reason': None})
                continue
            c = COMM.match(rest)
            if c:
                cpid, ctid, name = int(c.group('pid')), int(c.group('tid')), c.group('name')
                pid_of[ctid] = cpid
                if c.group(1):   # exec: a new program in this process
                    exec_comm[cpid] = name
                    proc_name.pop(cpid, None)
                    seen_threads.difference_update({k for k in seen_threads if k[0] == cpid})
                else:
                    thread(t, cpid, ctid, name)
                continue
            f = FORK.match(rest)
            if f:
                fpid, ftid = int(f.group('pid')), int(f.group('tid'))
                pid_of[ftid] = fpid
                if fpid != int(f.group('ppid')):   # a new process starts as a copy of its parent
                    exec_comm[fpid] = exec_comm.get(int(f.group('ppid')), m.group('comm'))
                    if int(f.group('ppid')) in proc_name:
                        proc_name[fpid] = proc_name[int(f.group('ppid'))]
                elif (fpid, ftid) not in seen_threads:
                    thread(t, fpid, ftid, None)
                continue
            mm = MMAP.match(rest)
            if mm:
                mpid = int(mm.group('pid'))
                path = mm.group('path').strip()
                if 'x' not in mm.group('prot') or path.startswith('['):
                    continue
                base = os.path.basename(path)
                comm = exec_comm.get(mpid)
                if mpid not in proc_name and comm and base[:15] == comm[:15]:
                    proc_name[mpid] = base
                    if ours(mpid):
                        emit({'type': 'process', 't_ns': t, 'pid': mpid, 'process': base})
                if ours(mpid):
                    start = int(mm.group('start'), 16)
                    emit({'type': 'module', 't_ns': t, 'pid': mpid, 'process': name_of(mpid), 'base': start,
                          'end': start + int(mm.group('len'), 16), 'path': path})
                continue
            lo = LOST.match(rest)
            if lo:
                counts['lost'] = counts.get('lost', 0) + int(lo.group('n'))
            continue
        # a sample: "<event>: [trace fields]" then callchain lines (or ip/sym/dso on the same line)
        event, _, fields = rest.partition(': ')
        event = event.rstrip(':')
        frames = []
        same_line = INLINE_FRAME.search(fields) if fields else None
        if 'sched_waking' in event or 'sched_wakeup' in event:
            w = re.search(r'\bpid=(\d+)', fields)
            if not w:
                continue
            rec = {'type': 'ready', 't_ns': t, 'pid': pid, 'tid': tid, 'process': name_of(pid),
                   'rdy_pid': -1, 'rdy_tid': int(w.group(1)), 'rdy_process': None, 'reason': event, 'stack': []}
        else:
            rec = {'type': 'sample', 't_ns': t, 'pid': pid, 'tid': tid, 'process': name_of(pid), 'pc': 0,
                   'stack': [], '_comm': m.group('comm')}
            if same_line:
                frames.append((int(same_line.group('addr'), 16), frame_name(same_line.group('sym'), same_line.group('dso'))))
        pending = (rec, frames)
    finish()
    out.close()
    if truncated:
        counts['truncated'] = True
    return counts


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('script', help='perf script text ("-" for stdin)')
    ap.add_argument('out')
    ap.add_argument('--markers', action='append', default=[])
    ap.add_argument('--programs', default=','.join(common.ALLOWED_PROGRAMS))
    a = ap.parse_args(argv)
    anchors = []
    for path in a.markers:
        h, _ = common.read_markers(path)
        if h:
            anchors += h['anchors']
    src = sys.stdin if a.script == '-' else open(a.script, encoding='utf-8', errors='replace')
    counts = convert(src, a.out, {p for p in a.programs.split(',') if p}, common.steady_offset_ns(anchors))
    print(json.dumps({'records': counts}))


if __name__ == '__main__':
    main()
