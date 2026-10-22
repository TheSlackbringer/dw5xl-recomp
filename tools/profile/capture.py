#!/usr/bin/env python3
"""One capture: switch recording on, record while the target runs, switch off, convert, report (profiling tools, a
debugging tool; CLI and outputs in CONTRACT.md). The only supported way to record.

  capture.py --target <exe> --out <dir> [--limit-s N] [--label TEXT] [--cancel-after S] [--fail-after-start]
             [--also <exe> [--also-args "<args>"]]... [--env NAME=VALUE]... [--cwd DIR] [-- <target args>]

Exit codes: 0 done, 2 not ready (setup, busy), 3 cancelled (test hook), 4 error after start (test hook),
5 rate limit reached (see next_slot_in_s), 6 refused by the protected script (Linux: below 20 GB free), 1 other failure.
On every path the switch is off afterwards. Windows: WPR through the protected task. Linux: perf as the user, recording
only its own child (linux_main; CONTRACT.md "Linux")."""
import argparse, datetime, glob, hashlib, json, os, platform, shlex, shutil, subprocess, sys, time, traceback
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common, etw_samples, profiling, report


class CaptureError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def symbols_of(path):
    if os.path.exists(os.path.splitext(path)[0] + '.pdb'):
        return 'pdb'
    objdump = common.tool_path('llvm-objdump')
    if objdump:
        out = subprocess.run([objdump, '-h', path], capture_output=True, text=True).stdout
        if '.debug_info' in out:
            return 'dwarf'
    return 'none'


def parse_args(argv):
    if '--' in argv:
        i = argv.index('--')
        argv, target_args = argv[:i], argv[i + 1:]
    else:
        target_args = []
    ap = argparse.ArgumentParser()
    ap.add_argument('--target', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--limit-s', type=float, default=common.RECORDING_LIMIT_S)
    ap.add_argument('--label', default='capture')
    ap.add_argument('--cancel-after', type=float)
    ap.add_argument('--fail-after-start', action='store_true')
    ap.add_argument('--also', action='append', default=[])
    ap.add_argument('--also-args', action='append', default=[])
    ap.add_argument('--env', action='append', default=[])
    ap.add_argument('--cwd')
    a = ap.parse_args(argv)
    a.target_args = target_args
    a.limit_s = min(a.limit_s, common.RECORDING_LIMIT_S)
    return a


def main(argv=None):
    a = parse_args(sys.argv[1:] if argv is None else argv)
    if not common.WINDOWS:
        return linux_main(a)
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    log = open(os.path.join(out, 'capture.log'), 'a', encoding='utf-8')

    def say(msg):
        line = f'[{datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]}] {msg}'
        log.write(line + '\n')
        log.flush()
        print(line, flush=True)

    target = os.path.abspath(a.target)
    manifest = {'schema': 1, 'os': 'windows' if common.WINDOWS else 'linux', 'label': a.label,
                'recorder': {'name': 'wpr' if common.WINDOWS else 'perf', 'version': platform.version(),
                             'settings': {'profile': 'dw5xl.wprp!DW5XL', 'sampling_interval_ms': 1}},
                'target': {'path': target, 'sha256': None, 'symbols': None, 'args': a.target_args},
                'anchors': [], 'recording_limit_s': a.limit_s, 'recording_s': None, 'bytes_written': None,
                'output_cap_bytes': None, 'sample_interval_ns': None, 'samples': None, 'lost_samples': None,
                'switched_off_after': None, 'result': 'error', 'error': None, 'stamp': None,
                'started': datetime.datetime.now(datetime.timezone.utc).isoformat()}
    code = 1
    recording = False
    children = []
    try:
        if not os.path.exists(target):
            raise CaptureError(2, f'target missing: {target}')
        manifest['target']['sha256'] = sha256(target)
        manifest['target']['symbols'] = symbols_of(target)
        st = profiling.status()
        if not st.get('set_up'):
            raise CaptureError(2, 'recording is not set up (tools/profile/WINDOWS_SETUP.md)')
        if st.get('recording'):
            raise CaptureError(2, 'another recording is running')
        if st.get('next_slot_in_s', 0) > 0:
            raise CaptureError(5, f'rate limit reached; next recording possible in {st["next_slot_in_s"]:.0f} s')
        cfg = profiling.config()
        manifest['output_cap_bytes'] = cfg.get('output_cap_bytes')
        previous = (profiling.current().get('stamp'), profiling.current().get('time'))

        # switch on for this capture only; the end time also ends it if this process dies
        profiling.switch_on(a.limit_s + 120)
        say(f'switched on; starting the recording task (limit {a.limit_s:.0f} s)')
        profiling.run_task(profiling.TASK_RECORD)
        cur = profiling.wait_state(lambda c: (c.get('stamp'), c.get('time')) != previous and
                                   c.get('state') in ('recording', 'refused', 'error'), 60)
        if cur.get('state') != 'recording':
            raise CaptureError(6, f'recording refused: {cur.get("reason") or "no answer from the task"}')
        recording = True
        stamp = cur['stamp']
        manifest['stamp'] = stamp
        profiling.ledger_add({'time': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'stamp': stamp,
                              'label': a.label, 'out': out})
        say(f'recording {stamp}')
        rec_start = time.time()
        if a.fail_after_start:
            raise CaptureError(4, 'test hook: error right after the recording started')

        env = dict(os.environ)
        for kv in a.env:
            k, _, v = kv.partition('=')
            env[k] = v
        target_log = open(os.path.join(out, 'target.log'), 'w', encoding='utf-8')
        children.append(subprocess.Popen([target] + a.target_args, cwd=a.cwd or os.path.dirname(target), env=env,
                                         stdout=target_log, stderr=subprocess.STDOUT))
        for i, exe in enumerate(a.also):
            args = shlex.split(a.also_args[i], posix=False) if i < len(a.also_args) else []
            children.append(subprocess.Popen([os.path.abspath(exe)] + args, cwd=os.path.dirname(os.path.abspath(exe)),
                                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        say(f'started {len(children)} program(s)')
        stopped = False
        while True:
            alive = [c for c in children if c.poll() is None]
            elapsed = time.time() - rec_start
            if a.cancel_after is not None and elapsed >= a.cancel_after:
                profiling.write_stop('cancel')
                profiling.wait_state(lambda c: c.get('state') not in ('recording', 'exporting'), 60)
                recording = False
                for c in alive:
                    c.wait()
                raise CaptureError(3, 'test hook: cancelled')
            if not stopped and (not alive or elapsed >= a.limit_s):
                profiling.write_stop('stop')
                stopped = True
                say(f'stop requested after {elapsed:.1f} s ({"programs ended" if not alive else "limit"})')
            if not alive:
                break
            time.sleep(0.1)
        for c in children:
            say(f'program exit {c.returncode}')
        cur = profiling.wait_state(lambda c: c.get('stamp') == stamp and c.get('state') in ('done', 'cancelled', 'error'),
                                   45 * 60)
        recording = False
        if cur.get('state') != 'done':
            raise CaptureError(1, f'recording ended as {cur.get("state")}: {cur.get("reason")}')
        exported = os.path.join(profiling.OUT, stamp)
        done = common.load_json(os.path.join(exported, 'done.json'), {}) or {}
        manifest['recording_s'] = done.get('recording_s', cur.get('recording_s'))
        manifest['bytes_written'] = done.get('bytes_written')
        manifest['raw_bytes'] = done.get('etl_bytes')
        if done.get('truncated'):
            say('WARNING: export cut short by the output cap')
        say(f'exported {done.get("events_kept")} lines, {done.get("bytes_written")} bytes; converting')

        marker_files = sorted(glob.glob(os.path.join(out, '**', 'markers.bin'), recursive=True))
        anchors = []
        for m in marker_files:
            h, _ = common.read_markers(m)
            if h:
                anchors += h['anchors']
        manifest['anchors'] = anchors
        anchor_lines = [l for l in open(os.path.join(exported, 'anchors.txt'), encoding='utf-8', errors='replace')
                        if l.startswith('#')]
        freq = next((x['os_freq'] for x in anchors if x.get('os_freq')), None) or etw_samples.estimate_freq(anchor_lines)
        samples = os.path.join(out, 'samples.jsonl')
        counts = etw_samples.convert(os.path.join(exported, 'events.txt'), anchor_lines, samples, freq,
                                     common.steady_offset_ns(anchors))
        records = [json.loads(l) for l in open(samples, encoding='utf-8') if l.strip()]
        rep = report.build(records, marker_files, set(cfg.get('allowed_programs') or common.ALLOWED_PROGRAMS))
        common.write_json(os.path.join(out, 'report.json'), rep)
        with open(os.path.join(out, 'report.txt'), 'w', encoding='utf-8') as f:
            f.write(report.text_report(rep))
        manifest['sample_interval_ns'] = rep['sample_interval_ns']
        manifest['samples'] = counts.get('sample', 0)
        manifest['lost_samples'] = None   # the recorder reports lost events in the raw recording only
        manifest['result'] = 'ok'
        code = 0
        say(f'report: {counts.get("sample", 0)} samples, interval {rep["sample_interval_ns"]} ns')
    except CaptureError as e:
        code = e.code
        manifest['result'] = 'cancelled' if e.code == 3 else 'error'
        manifest['error'] = str(e)
        say(f'error: {e}')
    except Exception as e:   # noqa: BLE001 - every failure must still switch off and be written down
        manifest['error'] = f'{type(e).__name__}: {e}'
        say('error: ' + traceback.format_exc())
    finally:
        profiling.switch_off()
        if recording or profiling.current().get('state') in ('recording', 'exporting') and \
                profiling.current().get('stamp') == manifest.get('stamp'):
            profiling.off()
        for c in children:
            if c.poll() is None:
                c.wait()
        if common.WINDOWS:
            manifest['switched_off_after'] = profiling.switch_state()[0] == 'off'
            # Recordings are deleted after use: the raw recording is already gone; the export
            # goes now that it has been converted (or the capture failed).
            profiling.clean()
            manifest['exports_left_after'] = profiling.exports_kept()
        manifest['finished'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        common.write_json(os.path.join(out, 'manifest.json'), manifest)
        log.close()
    return code



def linux_main(a):
    """Linux: one capture with perf (CONTRACT.md "Linux"). Same CLI, exit codes and outputs as on Windows; there is no
    switch, task or rate limit, because perf runs as the user and records only its own child (perf_launch.py) and
    the programs that one starts. Raw recording: perf.data in <out>, kept unless the recording was cancelled or
    failed before it finished (then discarded, like the Windows cancel)."""
    import perf_record, perf_samples
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    log = open(os.path.join(out, 'capture.log'), 'a', encoding='utf-8')

    def say(msg):
        line = f'[{datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]}] {msg}'
        log.write(line + '\n')
        log.flush()
        print(line, flush=True)

    target = os.path.abspath(a.target)
    perf = perf_record.perf_path()
    level = perf_record.paranoid()
    settings = perf_record.settings(level, perf_record.wakeups_possible(perf, level))
    manifest = {'schema': 1, 'os': 'linux', 'label': a.label,
                'recorder': {'name': 'perf', 'version': perf_record.version(perf), 'settings': settings},
                'target': {'path': target, 'sha256': None, 'symbols': None, 'args': a.target_args},
                'anchors': [], 'recording_limit_s': a.limit_s, 'recording_s': None, 'bytes_written': None,
                'raw_bytes': None, 'output_cap_bytes': perf_record.OUTPUT_CAP_BYTES, 'sample_interval_ns': None,
                'samples': None, 'lost_samples': None, 'switched_off_after': None, 'result': 'error', 'error': None,
                'started': datetime.datetime.now(datetime.timezone.utc).isoformat()}
    data = os.path.join(out, 'perf.data')
    go = os.path.join(out, 'perf-go.fifo')
    state = os.path.join(out, 'launch-state.json')
    code = 1
    ctl = proc = None
    launched = discard = False
    try:
        if not os.path.exists(target):
            raise CaptureError(2, f'target missing: {target}')
        manifest['target']['sha256'] = sha256(target)
        manifest['target']['symbols'] = symbols_of(target)
        if not perf or not manifest['recorder']['version']:
            raise CaptureError(2, 'perf is not installed')
        if level is None or level > 2:
            raise CaptureError(2, f'perf_event_paranoid is {level}; perf needs 2 or lower to record its own child')
        if perf_record.running():
            raise CaptureError(2, 'another recording is running')
        free_gb = shutil.disk_usage(out).free / 1024 ** 3
        if free_gb < common.MIN_FREE_GB:
            raise CaptureError(6, f'recording refused: low disk ({free_gb:.1f} GB free, minimum {common.MIN_FREE_GB})')
        for p in (data, state, go):
            if os.path.lexists(p):
                os.remove(p)
        os.mkfifo(go, 0o600)
        ctl = perf_record.Control(out)
        env = dict(kv.partition('=')[::2] for kv in a.env)
        programs = [{'argv': [target] + a.target_args, 'cwd': a.cwd or os.path.dirname(target), 'env': env,
                     'log': os.path.join(out, 'target.log')}]
        for i, exe in enumerate(a.also):
            args = shlex.split(a.also_args[i]) if i < len(a.also_args) else []
            exe = os.path.abspath(exe)
            programs.append({'argv': [exe] + args, 'cwd': os.path.dirname(exe), 'env': {}, 'log': None})
        spec = os.path.join(out, 'launch-spec.json')
        common.write_json(spec, {'go_fifo': go, 'state': state, 'programs': programs})
        cmd = perf_record.record_command(perf, data, ctl.ctl, ctl.ack,
                                         [sys.executable, os.path.join(common.HERE, 'perf_launch.py'), spec], settings)
        manifest['recorder']['command'] = cmd
        perf_log = open(os.path.join(out, 'perf-record.log'), 'w', encoding='utf-8')
        proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=perf_log, stderr=subprocess.STDOUT)
        discard = True
        t_on = ctl.send('enable', 30)
        if t_on is None:
            raise CaptureError(1, 'perf did not start recording (see perf-record.log)')
        say(f'recording (perf, limit {a.limit_s:.0f} s, wake-ups {"on" if settings["wakeups"] else "off"})')
        if a.fail_after_start:
            raise CaptureError(4, 'test hook: error right after the recording started')
        if not perf_record.tell_launcher(go, 'go'):
            raise CaptureError(1, 'the launcher is not listening (see perf-record.log)')
        launched = True
        say(f'started {len(programs)} program(s)')
        t_off, cancelled, st = None, False, {}
        while True:
            st = common.load_json(state, {}) or {}
            done = 'exit' in st or proc.poll() is not None
            elapsed = time.monotonic() - t_on
            if t_off is None:
                if a.cancel_after is not None and elapsed >= a.cancel_after:
                    t_off = ctl.send('disable') or time.monotonic()
                    cancelled = True
                    say(f'cancel after {elapsed:.1f} s; waiting for the programs')
                elif done or elapsed >= a.limit_s:
                    t_off = time.monotonic() if done else (ctl.send('disable') or time.monotonic())
                    say(f'stop after {t_off - t_on:.1f} s ({"programs ended" if done else "limit"})')
            if done:
                break
            time.sleep(0.1)
        for c in st.get('exit', []):
            say(f'program exit {c}')
        rc = proc.wait()
        say(f'perf exit {rc}')
        if cancelled:
            raise CaptureError(3, 'test hook: cancelled')
        if rc != 0 or not os.path.exists(data):
            raise CaptureError(1, f'perf record ended with exit {rc} (see perf-record.log)')
        discard = False
        manifest['recording_s'] = round(t_off - t_on, 3)
        manifest['raw_bytes'] = os.path.getsize(data)

        marker_files = sorted(glob.glob(os.path.join(out, '**', 'markers.bin'), recursive=True))
        anchors = []
        for m in marker_files:
            h, _ = common.read_markers(m)
            if h:
                anchors += h['anchors']
        manifest['anchors'] = anchors
        allowed = set(common.ALLOWED_PROGRAMS)
        samples = os.path.join(out, 'samples.jsonl')
        with open(os.path.join(out, 'perf-script.log'), 'w', encoding='utf-8') as err:
            script = subprocess.Popen(perf_record.script_command(perf, data, bool(settings['wakeups'])), stdin=subprocess.DEVNULL,
                                      stdout=subprocess.PIPE, stderr=err, text=True, errors='replace')
            counts = perf_samples.convert(script.stdout, samples, allowed, common.steady_offset_ns(anchors),
                                          perf_record.OUTPUT_CAP_BYTES)
            if script.wait() != 0:
                raise CaptureError(1, f'perf script ended with exit {script.returncode} (see perf-script.log)')
        manifest['bytes_written'] = os.path.getsize(samples)
        if counts.get('truncated'):
            say('WARNING: export cut short by the output cap')
        say(f'exported {sum(v for k, v in counts.items() if k not in ("lost", "truncated"))} records, '
            f'{manifest["bytes_written"]} bytes ({manifest["raw_bytes"]} bytes recorded); report')
        records = [json.loads(l) for l in open(samples, encoding='utf-8') if l.strip()]
        rep = report.build(records, marker_files, allowed)
        common.write_json(os.path.join(out, 'report.json'), rep)
        with open(os.path.join(out, 'report.txt'), 'w', encoding='utf-8') as f:
            f.write(report.text_report(rep))
        manifest['sample_interval_ns'] = rep['sample_interval_ns']
        manifest['samples'] = counts.get('sample', 0)
        manifest['lost_samples'] = counts.get('lost', 0)
        manifest['result'] = 'ok'
        code = 0
        say(f'report: {counts.get("sample", 0)} samples, interval {rep["sample_interval_ns"]} ns, '
            f'lost {counts.get("lost", 0)}')
    except CaptureError as e:
        code = e.code
        manifest['result'] = 'cancelled' if e.code == 3 else 'error'
        manifest['error'] = str(e)
        say(f'error: {e}')
    except Exception as e:   # noqa: BLE001 - every failure must still stop the recording and be written down
        manifest['error'] = f'{type(e).__name__}: {e}'
        say('error: ' + traceback.format_exc())
    finally:
        # never leave a recording behind: a launcher still waiting ends without starting anything (perf then ends
        # with it); if programs run, nothing more is recorded and perf ends when they end
        if not launched and proc is not None and os.path.lexists(go):
            perf_record.tell_launcher(go, 'abort', 2)
        if proc is not None and proc.poll() is None:
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                if ctl:
                    ctl.send('disable', 5)
                proc.wait()
        if ctl:
            ctl.close()
        if os.path.lexists(go):
            os.remove(go)
        if discard and os.path.exists(data):
            os.remove(data)
            say('recording discarded')
        manifest['finished'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        common.write_json(os.path.join(out, 'manifest.json'), manifest)
        log.close()
    return code

if __name__ == '__main__':
    sys.exit(main())
