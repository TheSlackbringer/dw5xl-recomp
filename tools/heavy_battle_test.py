#!/usr/bin/env python3
"""Heavy-combat VSync regression test (pass/fail), at native 1x.

Replays a scripted heavy fight from the owner's Legend Mode / Zhang Jiao save (the save resumes into
the battle pause menu): resume, run into the enemy and push on into the biggest crowds, then attacks,
moves and a Musou every 12 s for ~3 minutes. Crowd size varies per run (wall-clock pacing); on the
build before issue 001 v3 some runs reached ~97 % device load and dropped to ~41 FPS. Fails when the guest field clock slips, i.e. when the fights would visibly slow down.

  tools/heavy_battle_test.py [--mode gamescope|hidden] [--runs 2] [--audio null|real]

gamescope mode (default) runs the game in an off-screen gamescope compositor at 2560x1440@240 (the
owner's main monitor): a real presented window that never touches the desktop. Input is the game's
own PS2X_HOST_INPUT script, so no synthetic desktop input is sent.

Pass criteria per run (battle part, tick >= 4000):
  * the run reaches the exit tick (no hang/crash)
  * fields more than 3 ms late (PS2X_LATE_VBLANK=1 [late-vblank] lines) <= --max-late (default 2), not counting
    IOP catch-up stalls (thread=0 and the fields right after one: issue 003, reported separately)
  * [stats] VSync mean >= 49.8
  * game frame rate: the render thread syncs with the GS twice per game frame (explicit device drains in
    the PS2X_VU1_ASYNC_TRACE window lines), so syncs/s = 2 x game FPS. Worst second >= 90 (45 FPS) and
    mean >= 98 (49 FPS). Before the GS worker thread the worst second was ~58 (29 FPS): "slow and choppy".

Needs: app/build/dw5xl built, game/dw5xl/SLES_538.60 (your disc), the save in game/dw5xl/mc0, the
null sink for --audio null (pactl load-module module-null-sink sink_name=dw5xl_codex_tests).
Never run while another dw5xl is open or while the owner is testing. Frame dumps are disabled on
purpose: PNG dumps cause 40 ms host hitches. The final frame is saved as <out>/final.png (game
output: keep it out of git).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_DIR = os.path.join(REPO, 'game', 'dw5xl')
EXIT_TICK = 14000
BATTLE_FROM = 4000


def route(exit_tick=EXIT_TICK):
    events = ['400:X:10', '1000:ENTER:20', '1400:X:10', '1900:X:10',
              '2600:ENTER:8',   # pause menu: Start moves the cursor to RESUME
              '3000:X:10',      # confirm RESUME
              '3900:UP:180',    # run into the enemy
              '4100:UP:400']    # and push on into the biggest crowds (issue 001: ~95 % VU1 load)
    dirs = ['UP', 'UP', 'LEFT', 'UP', 'RIGHT']
    turn = 0
    for tick in range(4600, exit_tick, 50):
        if tick % 600 == 0:
            events.append(f'{tick}:C:40')           # Musou
        elif tick % 400 == 0:
            events.append(f'{tick}:{dirs[turn % len(dirs)]}:60')
            turn += 1
        else:
            events.append(f'{tick}:Z:12')           # attack
    return ','.join(events)


def run_once(name, args, out):
    log_path = os.path.join(out, name + '.log')
    dump_dir = os.path.join(out, name)
    os.makedirs(dump_dir, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PS2X_', 'DW5XL_')) and k != 'PULSE_SINK'}
    env.update(PS2X_GS_BACKEND='vulkan', PS2X_GS_SCALE='1', PS2X_GS_SSAA='1', PS2X_STATS='1',
               PS2X_LATE_VBLANK='1', PS2X_VU1_ASYNC_TRACE='1', PS2X_EXIT_TICK=str(args.exit_tick), PS2X_DUMP_DIR=dump_dir,
               PS2X_DUMP_EVERY='100000000', PS2X_HOST_INPUT=route(args.exit_tick))
    if args.audio == 'null':
        env['PULSE_SINK'] = 'dw5xl_codex_tests'
    game = [os.path.join(REPO, 'app', 'build', 'dw5xl'), './SLES_538.60']
    if args.mode == 'hidden':
        env['PS2X_HIDDEN'] = '1'
        cmd = game
    else:
        cmd = ['gamescope', '--backend', 'headless', '-W', '2560', '-H', '1440', '-w', '2560', '-h', '1440',
               '-r', '240', '--'] + game
    with open(log_path, 'w') as log:
        try:
            subprocess.run(cmd, cwd=GAME_DIR, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=420)
        except subprocess.TimeoutExpired:
            pass
    text = open(log_path, errors='replace').read()

    problems = []
    if not re.search(r'\[output\] scale=1x aa=1x', text):
        problems.append('not running at 1x')
    if f'exit tick {args.exit_tick} reached' not in text:
        problems.append('did not reach the exit tick (hang, crash or timeout)')
    # IOP catch-up stalls (thread=0, and the fields right after one) are issue 003; counted separately.
    late, iop, last_iop = [], [], None
    for m in re.finditer(r'\[late-vblank\] tick=(\d+) late=(\d+)us .*?thread=(-?\d+)', text):
        tick = int(m.group(1))
        if tick < BATTLE_FROM:
            continue
        if m.group(3) == '0' or (last_iop is not None and tick == last_iop + 1):
            iop.append(m)
            last_iop = tick
        else:
            late.append(m)
    vsync = [float(m.group(2)) for m in re.finditer(r'\[stats\] t=\S+ tick=(\d+) vsync/s=([\d.]+)', text)
             if int(m.group(1)) >= BATTLE_FROM]
    mean = sum(vsync) / len(vsync) if vsync else 0.0
    syncs = []
    tick = 0
    for line in text.splitlines():
        m = re.search(r'\[stats\] t=\S+ tick=(\d+)', line)
        if m:
            tick = int(m.group(1))
        m = re.search(r'\[vu1-async\] windowMs=\d+ .* explicit=(\d+)/', line)
        if m and tick >= BATTLE_FROM:
            syncs.append(int(m.group(1)))
    sync_mean = sum(syncs) / len(syncs) if syncs else 0.0
    sync_min = min(syncs, default=0)
    worst = max((int(m.group(2)) for m in late), default=0) / 1000.0
    if len(late) > args.max_late:
        problems.append(f'{len(late)} late fields (max {args.max_late})')
    if not vsync or mean < 49.8:
        problems.append(f'VSync mean {mean:.2f} < 49.8')
    if not syncs or sync_min < 90 or sync_mean < 98:
        problems.append(f'game frame rate too low (syncs/s mean {sync_mean:.1f}, worst {sync_min}; need >= 98 / >= 90)')
    final = os.path.join(dump_dir, 'final.png')
    print(f'{name}: late fields={len(late)} worst={worst:.1f} ms (IOP stalls, issue 003: {len(iop)})  VSync mean={mean:.2f} '
          f'min={min(vsync, default=0):.1f}  game FPS mean={sync_mean / 2:.1f} worst={sync_min / 2:.1f}'
          f'  -> {"PASS" if not problems else "FAIL: " + "; ".join(problems)}')
    print(f'  log {log_path}' + (f'  final frame {final}' if os.path.exists(final) else ''))
    return not problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mode', choices=('gamescope', 'hidden'), default='gamescope')
    ap.add_argument('--runs', type=int, default=3, help='crowd load varies per run; 3+ runs recommended')
    ap.add_argument('--audio', choices=('null', 'real'), default='null')
    ap.add_argument('--max-late', type=int, default=2)
    ap.add_argument('--exit-tick', type=int, default=EXIT_TICK, help='end of the fight (default %(default)s)')
    ap.add_argument('--out', default=None, help='log directory (default: a new temp dir)')
    args = ap.parse_args()
    if subprocess.run(['pgrep', '-x', 'dw5xl'], capture_output=True).returncode == 0:
        sys.exit('another dw5xl is running; close it first')
    if args.mode == 'gamescope' and not shutil.which('gamescope'):
        sys.exit('gamescope not found (use --mode hidden)')
    out = os.path.abspath(args.out or tempfile.mkdtemp(prefix='dw5xl-heavy-battle-'))
    os.makedirs(out, exist_ok=True)
    results = [run_once(f'run{i + 1}', args, out) for i in range(args.runs)]
    print('PASS' if all(results) else 'FAIL')
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
