"""Made-up recordings for the unit tests: the fixture's behaviour (hot phase, wait, wake-up) written in each recorder's
exact text format, with the matching ground truth (truth.json layout) and marker file. Nothing here comes from a real
recording: times start near zero, ids and addresses are invented, paths are placeholders."""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import common

FIXTURE = 'dw5xl_profile_fixture'
MS = 1_000_000
PID = 100
BASE = 0x55D000000000          # perf: where the made-up executable is mapped
PE_BASE = 0x140000000          # ETW: the made-up module's base


def timeline(cycles, hot_ms, wait_ms, t0):
    """Per cycle: thread ids and the five ground-truth moments (ns)."""
    out = []
    start = t0
    for i in range(cycles):
        hot_end = start + hot_ms * MS
        wait_start = hot_end + MS // 10
        signal = wait_start + wait_ms * MS
        out.append({'hot': PID + 1 + 3 * i, 'waiter': PID + 2 + 3 * i, 'waker': PID + 3 + 3 * i,
                    'hot_start_ns': start, 'hot_end_ns': hot_end, 'wait_start_ns': wait_start,
                    'signal_ns': signal, 'woken_ns': signal + 10_000})
        start = signal + 5 * MS
    return out


def truth_of(cycles):
    keys = ('hot_start_ns', 'hot_end_ns', 'wait_start_ns', 'signal_ns', 'woken_ns')
    return {'schema': 1, 'pid': PID, 'markers': True, 'cycles': [{k: c[k] for k in keys} for c in cycles]}


def markers_bytes(cycles, os_freq, os_ticks_of):
    """A marker file (runtime/ps2_profile_markers.h layout) with the fixture's markers: event 100, seq 10*(cycle+1)+k."""
    recs = []
    for i, c in enumerate(cycles):
        for k, key, tid in ((1, 'hot_start_ns', c['hot']), (2, 'hot_end_ns', c['hot']), (3, 'wait_start_ns', c['waiter']),
                            (4, 'signal_ns', c['waker']), (5, 'woken_ns', c['waiter'])):
            recs.append(common.MARKER_RECORD.pack(c[key], 10 * (i + 1) + k, tid, 0, 0, 100, 0))
    first, last = cycles[0]['hot_start_ns'], cycles[-1]['woken_ns']
    header = common.MARKER_HEADER.pack(b'DW5XLPM1', 1, common.MARKER_RECORD.size, len(recs), 0,
                                       first, os_ticks_of(first), os_freq, last, os_ticks_of(last), os_freq)
    return header + b''.join(recs)


# ---------- Linux: perf script text ----------

def perf_text(cycles=3, hot_ms=20, wait_ms=10, wakeups=True, other=True):
    """(lines, truth, markers bytes); perf times are CLOCK_MONOTONIC = steady_clock (marker offset 0)."""
    cyc = timeline(cycles, hot_ms, wait_ms, 1_000 * MS)
    exe = f'/x/{FIXTURE}'
    lines = []

    def head(comm, pid, tid, t):
        return f'{comm:>16} {pid:>7}/{tid:<7} {t // 1_000_000_000}.{t % 1_000_000_000:09d}: '

    def sample(comm, tid, t, frames):
        lines.append(head(comm, PID, tid, t) + 'cpu-clock/period=1000000/u: \n')
        for addr, sym, dso in frames:
            lines.append(f'\t{addr:16x} {sym} ({dso})\n')
        lines.append('\n')

    t = cyc[0]['hot_start_ns'] - MS
    lines.append(head('python3', 99, 99, t) + f'PERF_RECORD_FORK({PID}:{PID}):(99:99)\n')
    lines.append(head('python3', PID, PID, t + 100) + f'PERF_RECORD_COMM exec: {FIXTURE[:15]}:{PID}/{PID}\n')
    lines.append(head(FIXTURE[:15], PID, PID, t + 200) +
                 f'PERF_RECORD_MMAP2 {PID}/{PID}: [{BASE + 0x1000:#x}(0x5000) @ 0x1000 <00>]: r-xp {exe}\n')
    lines.append(head(FIXTURE[:15], PID, PID, t + 300) +
                 f'PERF_RECORD_MMAP2 {PID}/{PID}: [0x7f0000001000(0x9000) @ 0x1000 <00>]: r-xp /x/libc.so\n')
    if other:   # another program's lines: never kept
        lines.append(head('otherprog', 200, 200, t + 400) + 'PERF_RECORD_COMM exec: otherprog:200/200\n')
        lines.append(head('otherprog', 200, 200, t + 500) + 'cpu-clock/period=1000000/u: \n')
        lines.append(f'\t{0x400000:16x} other_work (/x/otherprog)\n\n')
    hot = [(BASE + 0x1234, 'fixture_hot_spin', exe), (BASE + 0x1400, 'fixture_hot_outer', exe),
           (0x7F0000005000, '[unknown]', '[unknown]')]
    for c in cyc:
        for role, name in (('hot', 'fixture-hot'), ('waiter', 'fixture-waiter'), ('waker', 'fixture-waker')):
            ts = c['hot_start_ns'] - 50_000 if role == 'hot' else c['hot_end_ns'] + 20_000
            lines.append(head(FIXTURE[:15], PID, c[role], ts) + f'PERF_RECORD_FORK({PID}:{c[role]}):({PID}:{PID})\n')
            lines.append(head(name, PID, c[role], ts + 1000) + f'PERF_RECORD_COMM: {name}:{PID}/{c[role]}\n')
        s = c['hot_start_ns'] + MS // 2
        while s < c['hot_end_ns']:
            sample('fixture-hot', c['hot'], s, hot)
            s += MS
        lines.append(head('fixture-waiter', PID, c['waiter'], c['wait_start_ns'] + 3_000) + 'PERF_RECORD_SWITCH OUT        \n')
        s = c['wait_start_ns'] + MS // 2
        while s < c['signal_ns']:
            sample('fixture-waker', c['waker'], s, [(BASE + 0x1238, 'fixture_hot_spin', exe),
                                                   (BASE + 0x1500, 'fixture_wake_work', exe)])
            s += MS
        if wakeups:
            lines.append(head('fixture-waker', PID, c['waker'], c['signal_ns'] + 2_000) +
                         f'sched:sched_waking: comm=fixture-waiter pid={c["waiter"]} prio=120 target_cpu=003\n')
            for addr, sym, dso in ((0x7F0000006000, 'pthread_cond_signal', '/x/libc.so'),
                                   (0x7F0000007000, 'std::condition_variable::notify_one()', '/x/libstdc++.so'),
                                   (BASE + 0x1600, '(anonymous namespace)::fixture_wake_signal()', exe)):
                lines.append(f'\t{addr:16x} {sym} ({dso})\n')
            lines.append('\n')
        lines.append(head('fixture-waiter', PID, c['waiter'], c['woken_ns'] - 3_000) + 'PERF_RECORD_SWITCH IN         \n')
    return lines, truth_of(cyc), markers_bytes(cyc, 1_000_000_000, lambda ns: ns)


# ---------- Windows: xperf dumper text and QPC anchor lines ----------

QPC_FREQ = 10_000_000
QPC_ORIGIN = 5_000_000          # QPC tick at trace time 0: trace time 0 = 500 ms on steady_clock

HEADER = """BeginHeader
                P-Start,  TimeStamp,     Process Name ( PID),  ParentPID,  SessionID,          UniqueKey, DirectoryTableBase, Flags, UserSid, Command Line, Package Full Name, Package Relative Application Id
                T-Start,  TimeStamp,     Process Name ( PID),   ThreadID,          StackBase,         StackLimit,         UsrStkBase,          UsrStkLmt,            TebBase, SubProcessTag, BasePri, PagePri, IoPri, Flags,          ThreadStartImage!Function
                I-Start,  TimeStamp,     Process Name ( PID),           BaseAddr,            EndAddr,   Checksum, TimeDateStamp,        DefaultBase, FileName, Relocated, NT FileName
             ThreadName,  TimeStamp,     Process Name ( PID),   ThreadID, ThreadName
                CSwitch,  TimeStamp, New Process Name ( PID),    New TID, NPri, NQnt, TmSinceLast, WaitTime, Old Process Name ( PID),    Old TID, OPri, OQnt,        OldState,      Wait Reason, Swapable, InSwitchTime, CPU, IdealProc, OldRemQnt, NewPriDecr, PrevCState, OldThrdBamQosLevel, NewThrdBamQosLevel
            ReadyThread,  TimeStamp,     Process Name ( PID),   ThreadID, Rdy Process Name ( PID),    Rdy TID, AdjustReason, AdjustIncrement, CPU, InDPC
         SampledProfile,  TimeStamp,     Process Name ( PID),   ThreadID,           PrgrmCtr, CPU, ThreadStartImage!Function,            Image!Function, Count, SampledProfile type
                  Stack,  TimeStamp,   ThreadID, No.,            Address,            Image!Function
EndHeader
"""


def us_of(ns):
    return (ns - QPC_ORIGIN * 1_000_000_000 // QPC_FREQ) // 1000


def etw_text(cycles=3, hot_ms=20, wait_ms=10):
    """(events text, anchor lines, truth, markers bytes); the export times are microseconds since trace start."""
    cyc = timeline(cycles, hot_ms, wait_ms, 600 * MS)
    proc = f'{FIXTURE}.exe ({PID:5d})'
    out = [HEADER]

    def ev(kind, ns, rest):
        out.append(f'{kind:>23}, {us_of(ns):10d}, {rest}\n')

    def stack(ns, tid, addrs):
        for n, a in enumerate(addrs, 1):
            out.append(f'{"Stack":>23}, {us_of(ns):10d}, {tid:10d}, {n:3d}, 0x{a:016x}\n')

    t = cyc[0]['hot_start_ns'] - 2 * MS
    ev('P-Start', t, f'{proc}, 50, 1, 0x0, 0x0, 0x00000000, S-1-0-0, "C:\\x\\{FIXTURE}.exe", , ')
    ev('I-Start', t + 1000, f'{proc}, 0x{PE_BASE:016x}, 0x{PE_BASE + 0x100000:016x}, 0x00000000, 0x00000000, '
                            f'0x{PE_BASE:016x}, "C:\\x\\{FIXTURE}.exe", 0, \\Device\\x\\{FIXTURE}.exe')
    hot = [PE_BASE + 0x1234, PE_BASE + 0x1400, 0xFFFFF80000001000]
    for c in cyc:
        for role, name in (('hot', 'fixture-hot'), ('waiter', 'fixture-waiter'), ('waker', 'fixture-waker')):
            ts = c['hot_start_ns'] - 50_000 if role == 'hot' else c['hot_end_ns'] + 20_000
            ev('T-Start', ts, f'{proc}, {c[role]}, 0x0, 0x0, 0x0, 0x0, 0x0, 0x00000000, 8, 5, 2, 0x00, '
                              f'{FIXTURE}.exe!0x{PE_BASE + 0x1000:016x}')
            ev('ThreadName', ts + 1000, f'{proc}, {c[role]}, {name}')
        s = c['hot_start_ns'] + MS // 2
        while s < c['hot_end_ns']:
            ev('SampledProfile', s, f'{proc}, {c["hot"]}, 0x{hot[0]:016x}, 2, -, -, 1, Unbatched')
            stack(s, c['hot'], hot)
            s += MS
        ev('CSwitch', c['wait_start_ns'] + 3_000, f'Idle (    0), 0, 0, -1, 0, 0, {proc}, {c["waiter"]}, 8, -1, '
                                                  'Waiting, UserRequest, NonSwap, 0, 2, 2, 0, 0, 2, Utility, Important')
        s = c['wait_start_ns'] + MS // 2
        while s < c['signal_ns']:
            ev('SampledProfile', s, f'{proc}, {c["waker"]}, 0x{PE_BASE + 0x1238:016x}, 3, -, -, 1, Unbatched')
            stack(s, c['waker'], [PE_BASE + 0x1238, PE_BASE + 0x1500])
            s += MS
        ev('ReadyThread', c['signal_ns'] + 2_000, f'{proc}, {c["waker"]}, {proc}, {c["waiter"]}, Unwait, 0, 2, ')
        stack(c['signal_ns'] + 2_000, c['waker'], [0xFFFFF80000002000, PE_BASE + 0x1600])
        ev('CSwitch', c['woken_ns'] - 3_000, f'{proc}, {c["waiter"]}, 8, -1, 0, 0, Idle (    0), 0, 0, -1, '
                                             'Running, Executive, NonSwap, 0, 2, 2, 0, 0, 2, Utility, Important')
    anchors = ['# Cpu #       RawTimeStamp #       TimeStamp[ns] #           EventKey\n']
    for k in range(4):
        ns = k * 250 * MS
        anchors.append(f'#   0 # 0x{QPC_ORIGIN + ns * QPC_FREQ // 1_000_000_000:016x} # {ns:19d} # 0x0000000000000000\n')
    ticks = lambda ns: ns * QPC_FREQ // 1_000_000_000
    return ''.join(out), anchors, truth_of(cyc), markers_bytes(cyc, QPC_FREQ, ticks)
