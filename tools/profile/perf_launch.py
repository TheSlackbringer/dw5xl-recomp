#!/usr/bin/env python3
"""The program perf records (profiling tools): started by capture.py as `perf record`'s child, it waits for
"go" on its fifo, then starts the target and every --also program as its own children (perf follows them), writes
their pids, waits for all of them and writes their exit codes. Any other word ends it without starting anything.

  perf_launch.py <spec.json>   spec: {"go_fifo": path, "state": path, "programs": [{"argv", "cwd", "env", "log"}]}
"env" holds only the extra variables for that program; everything else is inherited."""
import json, os, subprocess, sys


def write_state(path, data):
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(data, f)
    os.replace(tmp, path)


def main():
    spec = json.load(open(sys.argv[1], encoding='utf-8'))
    with open(spec['go_fifo'], encoding='ascii') as f:
        word = f.read().strip()
    if word != 'go':
        write_state(spec['state'], {'started': False, 'word': word})
        return 0
    children = []
    for p in spec['programs']:
        env = dict(os.environ, **(p.get('env') or {}))
        log = open(p['log'], 'w', encoding='utf-8') if p.get('log') else subprocess.DEVNULL
        children.append(subprocess.Popen(p['argv'], cwd=p['cwd'], env=env, stdout=log,
                                         stderr=subprocess.STDOUT if p.get('log') else subprocess.DEVNULL))
    pids = [c.pid for c in children]
    write_state(spec['state'], {'started': True, 'pids': pids})
    codes = [c.wait() for c in children]
    write_state(spec['state'], {'started': True, 'pids': pids, 'exit': codes})
    return 0


if __name__ == '__main__':
    sys.exit(main())
