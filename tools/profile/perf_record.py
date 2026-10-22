#!/usr/bin/env python3
"""Linux recorder control for capture.py (profiling tools; CONTRACT.md "Linux").

perf runs as the user, unprivileged: it records only its own child, perf_launch.py, and the programs that one
starts, so an unrelated program is never recorded. The recording starts disabled (-D -1) and is switched with
perf's control fifo: "enable" just before the programs start, "disable" at the time limit (the programs keep
running; perf records nothing more and ends when they end). Kernel setting perf_event_paranoid decides what is
possible: <= 2 user-space samples of our own child; <= 1 also kernel frames; wake-ups (sched:sched_waking with the
waker's stack) need readable tracefs and <= -1 or CAP_PERFMON (a system setting an administrator decides)."""
import errno, os, select, shutil, subprocess, time

SAMPLE_PERIOD_NS = 1_000_000
OUTPUT_CAP_BYTES = 8 * 1024 ** 3
TRACEFS = ('/sys/kernel/tracing', '/sys/kernel/debug/tracing')
SCRIPT_FIELDS = 'comm,pid,tid,time,event,ip,sym,dso'   # + trace when wake-ups are recorded


def perf_path():
    return shutil.which('perf')


def version(perf=None):
    perf = perf or perf_path()
    if not perf:
        return None
    try:
        return subprocess.run([perf, '--version'], capture_output=True, text=True, timeout=30).stdout.strip() or None
    except (OSError, subprocess.TimeoutExpired):
        return None


def paranoid():
    try:
        return int(open('/proc/sys/kernel/perf_event_paranoid').read().strip())
    except (OSError, ValueError):
        return None


def _has_caps(path):
    try:
        return bool(path) and bool(os.getxattr(os.path.realpath(path), 'security.capability'))
    except OSError:
        return False


def wakeups_possible(perf=None, level=None):
    """Whether sched:sched_waking (with the waker's stack) can be recorded by this user."""
    level = paranoid() if level is None else level
    readable = any(os.access(os.path.join(t, 'events', 'sched', 'sched_waking', 'format'), os.R_OK) for t in TRACEFS)
    return readable and (os.geteuid() == 0 or (level is not None and level <= -1) or _has_caps(perf or perf_path()))


def settings(level, wakeups):
    """Recorder settings for the manifest and the command line."""
    # Call stacks are user space only (--user-callchains): this kernel refuses unprivileged call stacks otherwise
    # (EACCES at paranoid 2, seen with Linux 7.2), and a sample taken in a system call is then charged to the user
    # function that made it, as on Windows.
    return {'sample_event': 'cpu-clock' if level is not None and level <= 1 else 'cpu-clock:u',
            'sample_period_ns': SAMPLE_PERIOD_NS, 'call_graph': 'fp', 'callchains': 'user', 'clock': 'CLOCK_MONOTONIC',
            'switch_events': True, 'wakeups': 'sched:sched_waking' if wakeups else None,
            'wakeup_call_graph': 'dwarf' if wakeups else None,
            'perf_event_paranoid': level, 'max_size_bytes': OUTPUT_CAP_BYTES, 'buildid_cache': False}


def record_command(perf, data, ctl, ack, workload, s):
    ev = s['sample_event']
    name, _, mods = ev.partition(':')
    cmd = [perf, 'record', '-e', f'{name}/period={s["sample_period_ns"]}/{mods}']
    if s['wakeups']:
        # one sample per wake-up (perf's default for trace events; it takes no period= here). The waker's stack is
        # unwound with DWARF (8 KB stack copy per wake-up): the system C++ library has no frame pointers, so frame
        # pointer stacks stop inside notify_one and lose the caller.
        cmd += ['-e', f'{s["wakeups"]}/call-graph=dwarf/']
    cmd += ['--call-graph', s['call_graph'], '--user-callchains', '-k', s['clock'], '--switch-events', '-D', '-1',
            f'--control=fifo:{ctl},{ack}', f'--max-size={s["max_size_bytes"]}B', '-N', '-o', data]
    return cmd + ['--'] + list(workload)


def script_command(perf, data, wakeups=False):
    fields = SCRIPT_FIELDS.replace('event,', 'event,trace,') if wakeups else SCRIPT_FIELDS
    return [perf, 'script', '-i', data, '--ns', '-F', fields, '--show-task-events', '--show-switch-events',
            '--show-mmap-events', '--show-lost-events', '--no-inline']


def running():
    """Whether a `perf record` is running anywhere on this account (any capture, or a manual one)."""
    for d in os.listdir('/proc'):
        if not d.isdigit():
            continue
        try:
            argv = open(f'/proc/{d}/cmdline', 'rb').read().split(b'\0')
        except OSError:
            continue
        if len(argv) > 1 and os.path.basename(argv[0]) == b'perf' and argv[1] == b'record':
            return True
    return False


class Control:
    """perf's control fifo (commands) and ack fifo; the ack fifo is opened before perf starts, because perf opens
    it for writing and would otherwise block."""

    def __init__(self, folder):
        self.ctl = os.path.join(folder, 'perf-ctl.fifo')
        self.ack = os.path.join(folder, 'perf-ack.fifo')
        for p in (self.ctl, self.ack):
            if os.path.exists(p):
                os.remove(p)
            os.mkfifo(p, 0o600)
        self.ack_fd = os.open(self.ack, os.O_RDONLY | os.O_NONBLOCK)

    def send(self, word, timeout=10.0):
        """Sends a command and waits for perf's "ack"; returns the monotonic time of the ack, None on timeout."""
        end = time.monotonic() + timeout
        fd = None
        while fd is None:
            try:
                fd = os.open(self.ctl, os.O_WRONLY | os.O_NONBLOCK)
            except OSError as e:
                if e.errno != errno.ENXIO or time.monotonic() > end:
                    return None
                time.sleep(0.02)
        try:
            os.write(fd, (word + '\n').encode())
        finally:
            os.close(fd)
        got = b''
        while b'ack' not in got:
            left = end - time.monotonic()
            if left <= 0:
                return None
            if select.select([self.ack_fd], [], [], left)[0]:
                chunk = os.read(self.ack_fd, 64)
                if not chunk:
                    time.sleep(0.01)   # no writer yet / perf gone
                got += chunk
        return time.monotonic()

    def close(self):
        try:
            os.close(self.ack_fd)
        except OSError:
            pass
        for p in (self.ctl, self.ack):
            try:
                os.remove(p)
            except OSError:
                pass


def tell_launcher(go_fifo, word, timeout=10.0):
    """Writes "go" or "abort" to perf_launch.py's fifo; False if it is not listening (ended) within the timeout."""
    end = time.monotonic() + timeout
    while True:
        try:
            fd = os.open(go_fifo, os.O_WRONLY | os.O_NONBLOCK)
            break
        except OSError as e:
            if e.errno != errno.ENXIO or time.monotonic() > end:
                return False
            time.sleep(0.02)
    try:
        os.write(fd, word.encode())
    finally:
        os.close(fd)
    return True
