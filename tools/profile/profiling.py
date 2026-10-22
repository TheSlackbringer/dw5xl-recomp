#!/usr/bin/env python3
"""Status and switch for the 025 profiling recorder (profiling tools; CONTRACT.md).

  profiling.py status [--json]      whether recording is set up, switched on or running; rate limit, free space,
                                    raw recordings kept, recordings not started through capture.py ("unexplained")
  profiling.py off                  switch off and cancel any running recording (unsaved)
  profiling.py test raw-request [--switched]   issue-test hooks: start the task directly (optionally switched on
  profiling.py test raw-switch <text>          first, without a capture ledger entry), write the switch verbatim,
  profiling.py test decide --free-gb N --json  or ask the protected script's decision for N GB free
Windows only for everything but status (Linux uses perf, which needs no switch). Linux status: perf version,
perf_event_paranoid, whether kernel frames and wake-ups can be recorded, whether a perf recording is running."""
import argparse, datetime, json, os, shutil, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

TASK_RECORD = r'\DW5XL\Profile Record'
TASK_CANCEL = r'\DW5XL\Profile Cancel'
BIN = os.path.join(common.PROTECTED, 'bin')
OUT = os.path.join(common.PROTECTED, 'out')
POWERSHELL = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe')


def config():
    return common.load_json(os.path.join(BIN, 'config.json'), {}) or {}


def user_dir():
    return config().get('user_dir') or common.USER_DIR


def utc(dt):
    return dt.strftime('%Y-%m-%dT%H:%M:%SZ')


def parse_utc(text):
    try:
        return datetime.datetime.strptime(text.strip(), '%Y-%m-%dT%H:%M:%SZ').replace(tzinfo=datetime.timezone.utc)
    except (ValueError, AttributeError):
        return None


def now():
    return datetime.datetime.now(datetime.timezone.utc)


def switch_path():
    return os.path.join(user_dir(), 'switch.txt')


def switch_on(seconds):
    os.makedirs(user_dir(), exist_ok=True)
    with open(switch_path(), 'w', encoding='ascii') as f:
        f.write(utc(now() + datetime.timedelta(seconds=seconds)))


def switch_off():
    try:
        os.remove(switch_path())
    except FileNotFoundError:
        pass


def write_stop(word):
    """'stop' saves the recording, 'cancel' discards it. The protected script only accepts a newer flag."""
    os.makedirs(user_dir(), exist_ok=True)
    with open(os.path.join(user_dir(), 'stop.flag'), 'w', encoding='ascii') as f:
        f.write(word)


def run_task(name):
    return subprocess.run(['schtasks', '/run', '/tn', name], capture_output=True, text=True).returncode


def task_exists(name):
    return subprocess.run(['schtasks', '/query', '/tn', name], capture_output=True, text=True).returncode == 0


def current():
    return common.load_json(os.path.join(OUT, 'current.json'), {}) or {}


def audit():
    try:
        with open(os.path.join(OUT, 'audit.jsonl'), encoding='utf-8-sig') as f:
            return [json.loads(l) for l in f if l.strip()]
    except (OSError, ValueError):
        return []


def ledger_path():
    return os.path.join(user_dir(), 'captures.jsonl')


def ledger():
    try:
        with open(ledger_path(), encoding='utf-8') as f:
            return [json.loads(l) for l in f if l.strip()]
    except (OSError, ValueError):
        return []


def ledger_add(entry):
    os.makedirs(user_dir(), exist_ok=True)
    with open(ledger_path(), 'a', encoding='utf-8') as f:
        f.write(json.dumps(entry) + '\n')


def switch_state():
    try:
        text = open(switch_path(), encoding='ascii', errors='replace').read()
    except OSError:
        return 'off', None
    end = parse_utc(text)
    if end is None:
        return 'malformed', text.strip()
    return ('on' if end > now() else 'expired'), utc(end)


def status():
    if not common.WINDOWS:
        import perf_record
        perf = perf_record.perf_path()
        version = perf_record.version(perf)
        level = perf_record.paranoid()
        return {'recorder': 'perf', 'set_up': bool(version) and level is not None and level <= 2, 'perf': perf,
                'perf_version': version, 'perf_event_paranoid': level, 'kernel_frames': level is not None and level <= 1,
                'wakeups': perf_record.wakeups_possible(perf, level), 'switch': None, 'recording': perf_record.running(),
                'recording_limit_s': common.RECORDING_LIMIT_S, 'output_cap_bytes': perf_record.OUTPUT_CAP_BYTES,
                'free_gb': shutil.disk_usage(common.REPO).free / 1024 ** 3, 'min_free_gb': common.MIN_FREE_GB}
    cfg = config()
    limit = int(cfg.get('rate_limit_per_hour', common.RATE_LIMIT_PER_HOUR))
    entries = audit()
    hour_ago = now() - datetime.timedelta(hours=1)
    recent = sorted(t for t in (parse_utc(a.get('time', '')) for a in entries if a.get('accepted')) if t and t >= hour_ago)
    wait = 0.0
    if len(recent) >= limit:
        wait = max(0.0, (recent[len(recent) - limit] + datetime.timedelta(hours=1) - now()).total_seconds())
    known = {e.get('stamp') for e in ledger()}
    state, end = switch_state()
    cur = current()
    drive = os.path.splitdrive(common.PROTECTED)[0] + os.sep
    return {'recorder': 'wpr', 'set_up': bool(cfg) and task_exists(TASK_RECORD), 'switch': 'on' if state == 'on' else 'off',
            'switch_state': state, 'switch_end': end, 'recording': cur.get('state') in ('recording', 'exporting'),
            'current': cur, 'recordings_last_hour': len(recent), 'rate_limit_per_hour': limit, 'next_slot_in_s': wait,
            'free_gb': shutil.disk_usage(drive).free / 1024 ** 3, 'min_free_gb': cfg.get('min_free_gb', common.MIN_FREE_GB),
            'raw_kept': common.load_json(os.path.join(OUT, 'raw-index.json'), []) or [],
            'exports_kept': exports_kept(),
            'unexplained': [a for a in entries if a.get('accepted') and a.get('stamp') and a['stamp'] not in known],
            'audit_log': os.path.join(OUT, 'audit.jsonl')}


def wait_state(pred, timeout):
    end = time.time() + timeout
    while time.time() < end:
        cur = current()
        if pred(cur):
            return cur
        time.sleep(0.2)
    return current()


def exports_kept():
    try:
        return sorted(d for d in os.listdir(OUT) if os.path.isdir(os.path.join(OUT, d)))
    except OSError:
        return []


def clean():
    """Recordings are deleted after use: the protected cleanup task deletes every raw recording
    and every export. Runs after each capture, once its export has been converted."""
    if not common.WINDOWS:
        return
    run_task(TASK_CANCEL)
    end = time.time() + 60
    while time.time() < end and exports_kept():
        time.sleep(0.2)


def off():
    """Switch off, cancel any running recording and leave nothing behind (cleanup task)."""
    switch_off()
    if current().get('state') in ('recording', 'exporting'):
        write_stop('cancel')
        cur = wait_state(lambda c: c.get('state') not in ('recording', 'exporting'), 30)
        if cur.get('state') in ('recording', 'exporting'):
            run_task(TASK_CANCEL)
            wait_state(lambda c: c.get('state') not in ('recording', 'exporting'), 30)
    clean()


def main(argv=None):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('status')
    s.add_argument('--json', action='store_true')
    sub.add_parser('off')
    t = sub.add_parser('test')
    t.add_argument('hook', choices=['raw-request', 'raw-switch', 'decide'])
    t.add_argument('text', nargs='?')
    t.add_argument('--switched', action='store_true')
    t.add_argument('--free-gb', type=float)
    t.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    if a.cmd == 'status':
        st = status()
        print(json.dumps(st, indent=None if a.json else 1))
        return 0
    if not common.WINDOWS:
        print('nothing to switch on Linux', file=sys.stderr)
        return 0
    if a.cmd == 'off':
        off()
        return 0
    if a.hook == 'raw-switch':
        os.makedirs(user_dir(), exist_ok=True)
        with open(switch_path(), 'w', encoding='utf-8') as f:
            f.write(a.text or '')
        return 0
    if a.hook == 'decide':
        p = subprocess.run([POWERSHELL, '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
                            os.path.join(BIN, 'record-task.ps1'), '-DecideOnly', '-SimulatedFreeGb', str(a.free_gb)],
                           capture_output=True, text=True, timeout=120)
        lines = [l for l in p.stdout.splitlines() if l.strip().startswith('{')]
        print(lines[-1] if lines else '{}')
        return 0
    before = len(audit())
    previous = (current().get('stamp'), current().get('time'))
    if a.switched:
        switch_on(common.RECORDING_LIMIT_S + 120)
    run_task(TASK_RECORD)
    if a.switched:
        wait_state(lambda c: (c.get('stamp'), c.get('time')) != previous and c.get('state') in ('recording', 'refused', 'error'), 30)
    else:
        end = time.time() + 30
        while time.time() < end and len(audit()) == before:
            time.sleep(0.2)
    return 0


if __name__ == '__main__':
    sys.exit(main())
