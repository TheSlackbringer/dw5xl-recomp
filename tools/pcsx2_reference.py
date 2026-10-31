#!/usr/bin/env python3
"""Capture reference screenshots of the original game in PCSX2, headless.

When unsure how the original renders or behaves, compare the port against
PCSX2 instead of guessing. This runs the owner's PCSX2 AppImage on a private
Xvfb display, so nothing appears on the desktop and no input reaches other
windows (never send synthetic keys to the real desktop: KDE asks the owner to
allow "remote control"). PCSX2 runs from a throwaway copy of the owner's
config (BIOS path, memory-card copies), so their settings and saves are never
touched. It uses PCSX2's software renderer (accurate reference; Vulkan cannot
present on Xvfb) and its own F8 screenshot hotkey.

  tools/pcsx2_reference.py --out <dir> --seconds 540 [--interval 0.5]
                           [--keys "15:x,20:Return"] [--iso <file>] [--mc-folder <saves>]

Keys are X keysym names at seconds after launch (combine with +, e.g. Shift_L+F8
for a one-frame GS dump, Control_L+Shift_L+F8 to start/stop a multi-frame dump;
dumps land next to the screenshots); the copied config maps
Cross = X, Start = Return, Circle = C, Square = Z, Triangle = V, D-pad = arrows.
--mc-folder copies a folder of saves (the port's memory-card layout) into the throwaway config as PCSX2's
slot 1 folder card, with a formatted card header when it has none; the card stays in
<out>/pcsx2home/PCSX2/memcards/Mcd001.ps2 after the run. --mc-file does the same with a copy of a .ps2 card
file (folder cards show only the running disc's saves; a card file shows all of them). Screenshots land in <out>/snaps (PCSX2 names them by wall-clock time; sort by
name). Audio goes to the dw5xl_codex_tests null sink. The output is game-derived:
keep it out of git (e.g. notes/evidence or a scratch directory).

Requires: Xvfb, python3-xlib, the AppImage and a BIOS configured in
~/.config/PCSX2 (owner's install), the PAL disc image under discs/dw5xl/.
"""
import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_APP = os.path.expanduser('~/Games/psx2/pcsx2-v2.8.2-linux-appimage-x64-Qt.AppImage')
OWNER_CFG = os.path.expanduser('~/.config/PCSX2')
PAD_KEYS = {'Cross': 'Keyboard/X', 'Start': 'Keyboard/Return', 'Circle': 'Keyboard/C', 'Square': 'Keyboard/Z',
            'Triangle': 'Keyboard/V', 'Up': 'Keyboard/Up', 'Down': 'Keyboard/Down', 'Left': 'Keyboard/Left',
            'Right': 'Keyboard/Right', 'Select': 'Keyboard/BackSpace', 'L1': 'Keyboard/Q', 'R1': 'Keyboard/E'}


def set_key(ini, section, key, value):
    m = re.search(r'^\[' + re.escape(section) + r'\]\n', ini, re.M)
    if not m:
        return ini + f'\n[{section}]\n{key} = {value}\n'
    end = ini.find('\n[', m.end())
    end = len(ini) if end < 0 else end
    body = ini[m.end():end]
    if re.search(r'^' + re.escape(key) + r' = .*$', body, re.M):
        body = re.sub(r'^' + re.escape(key) + r' = .*$', f'{key} = {value}', body, flags=re.M)
    else:
        body = f'{key} = {value}\n' + body
    return ini[:m.end()] + body + ini[end:]


def folder_card_superblock():
    """Block 0 of a formatted 8 MB PS2 memory card, as PCSX2 keeps it in a folder card's _pcsx2_superblock (8 KiB;
    PCSX2 counts the card as formatted when byte 0x16 is the 'o' of the magic). The standard values of an 8 MB card:
    512-byte pages, 2 pages per cluster, 16 per erase block, 8192 clusters, data from cluster 41 to 8135, the root
    directory at cluster 0, backup blocks 1023/1022, the indirect FAT cluster 8, no bad blocks, PS2 card."""
    sb = bytearray(b'\xff' * 8192)
    struct.pack_into('<28s12sHHHHIIIIII8x', sb, 0, b'Sony PS2 Memory Card Format ', b'1.2.0.0', 512, 2, 16, 0xFF00,
                     8192, 41, 8135, 0, 1023, 1022)
    struct.pack_into('<32I', sb, 0x50, 8, *([0] * 31))
    struct.pack_into('<32I', sb, 0xD0, *([0xFFFFFFFF] * 32))
    struct.pack_into('<BB', sb, 0x150, 2, 0x52)
    return bytes(sb)


def make_config(out, mc_folder=None, mc_file=None):
    home = os.path.join(out, 'pcsx2home')
    cfg = os.path.join(home, 'PCSX2')
    os.makedirs(os.path.join(cfg, 'inis'), exist_ok=True)
    os.makedirs(os.path.join(cfg, 'memcards'), exist_ok=True)
    os.makedirs(os.path.join(out, 'snaps'), exist_ok=True)
    ini = open(os.path.join(OWNER_CFG, 'inis', 'PCSX2.ini')).read()
    bios = re.search(r'^Bios = (.*)$', ini, re.M)
    if bios:
        path = bios.group(1)
        if not os.path.isabs(path):
            path = os.path.normpath(os.path.join(OWNER_CFG, path))
        ini = set_key(ini, 'Folders', 'Bios', path)
    ini = set_key(ini, 'Folders', 'Snapshots', os.path.join(out, 'snaps'))
    ini = set_key(ini, 'EmuCore/GS', 'Renderer', '13')
    for key, value in (('PauseOnFocusLoss', 'false'), ('ConfirmShutdown', 'false'), ('StartFullscreen', 'false')):
        ini = set_key(ini, 'UI', key, value)
    for button, key in PAD_KEYS.items():
        ini = set_key(ini, 'Pad1', button, key)
    ini = set_key(ini, 'Hotkeys', 'Screenshot', 'Keyboard/F8')
    if mc_file:
        # Slot 1: a copy of a PCSX2 card file (.ps2); slot 2 off.
        shutil.copyfile(mc_file, os.path.join(cfg, 'memcards', 'Mcd001.ps2'))
        ini = set_key(ini, 'MemoryCards', 'Slot1_Enable', 'true')
        ini = set_key(ini, 'MemoryCards', 'Slot1_Filename', 'Mcd001.ps2')
        ini = set_key(ini, 'MemoryCards', 'Slot2_Enable', 'false')
    elif mc_folder:
        # Slot 1: a copy of a folder of PS2 saves as a PCSX2 folder card (a directory at the slot path); slot 2 off.
        card = os.path.join(cfg, 'memcards', 'Mcd001.ps2')
        shutil.copytree(mc_folder, card, copy_function=shutil.copyfile)
        for root, dirs, files in os.walk(card):
            for name in dirs + files:
                os.chmod(os.path.join(root, name), 0o755 if name in dirs else 0o644)
        os.chmod(card, 0o755)
        superblock = os.path.join(card, '_pcsx2_superblock')
        if not os.path.exists(superblock) or os.path.getsize(superblock) != 8192:
            open(superblock, 'wb').write(folder_card_superblock())
        ini = set_key(ini, 'MemoryCards', 'Slot1_Enable', 'true')
        ini = set_key(ini, 'MemoryCards', 'Slot1_Filename', 'Mcd001.ps2')
        ini = set_key(ini, 'MemoryCards', 'Slot2_Enable', 'false')
    open(os.path.join(cfg, 'inis', 'PCSX2.ini'), 'w').write(ini)
    if not mc_folder and not mc_file:
        for card in glob.glob(os.path.join(OWNER_CFG, 'memcards', '*.ps2')):
            shutil.copy2(card, os.path.join(cfg, 'memcards'))
    return home


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--seconds', type=float, required=True)
    ap.add_argument('--interval', type=float, default=0.5)
    ap.add_argument('--keys', default='', help='comma list of <seconds>:<keysym>[:<hold seconds>]')
    ap.add_argument('--iso', default=None)
    ap.add_argument('--app', default=DEFAULT_APP)
    ap.add_argument('--display', default=':77')
    ap.add_argument('--mc-folder', default=None,
                    help='a folder of PS2 saves (e.g. game/test-save/<card>) used as slot 1, as a PCSX2 folder card')
    ap.add_argument('--mc-file', default=None, help='a PCSX2 card file (.ps2) whose copy is slot 1')
    ap.add_argument('--xshots', action='store_true',
                    help='also capture the whole private display every interval (PCSX2\'s own menus are not in its screenshots)')
    args = ap.parse_args()

    iso = args.iso or next(iter(glob.glob(os.path.join(REPO, 'discs', 'dw5xl', '*.iso'))), None)
    if not iso or not os.path.exists(iso):
        sys.exit('no disc image (pass --iso)')
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    home = make_config(out, args.mc_folder, args.mc_file)
    keys = []
    for item in filter(None, args.keys.split(',')):
        parts = item.split(':')
        keys.append((float(parts[0]), parts[1], float(parts[2]) if len(parts) > 2 else 0.1))
    keys.sort()

    xvfb = subprocess.Popen(['Xvfb', args.display, '-screen', '0', '1280x1024x24', '-nolisten', 'tcp'],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)
    os.environ['DISPLAY'] = args.display
    from Xlib import X, XK, display
    from Xlib.ext import xtest

    env = dict(os.environ, DISPLAY=args.display, QT_QPA_PLATFORM='xcb', XDG_CONFIG_HOME=home,
               PULSE_SINK='dw5xl_codex_tests', VK_DRIVER_FILES='/nonexistent.json', VK_ICD_FILENAMES='/nonexistent.json')
    log = open(os.path.join(out, 'pcsx2.log'), 'w')
    proc = subprocess.Popen([args.app, '-batch', '-fastboot', '--', iso], env=env, stdout=log, stderr=subprocess.STDOUT)
    d = display.Display(args.display)
    d.set_error_handler(lambda *a: None)
    root = d.screen().root
    pid_atom = d.intern_atom('_NET_WM_PID')

    def descends(pid):
        while pid > 1:
            if pid == proc.pid:
                return True
            try:
                pid = int(open('/proc/%d/stat' % pid).read().rsplit(')', 1)[1].split()[1])
            except OSError:
                return False
        return False

    def game_window():
        stack = [root]
        while stack:
            w = stack.pop()
            try:
                name = w.get_wm_name() or ''
                pid = w.get_full_property(pid_atom, X.AnyPropertyType)
                stack.extend(w.query_tree().children)
            except Exception:
                continue
            if pid and descends(pid.value[0]) and name and 'PCSX2' not in name and 'Qt' not in name \
                    and w.get_attributes().map_state == X.IsViewable:
                return w
        return None

    def press(keysyms, hold=0.08):
        win = game_window()
        if win is None:
            return False
        win.set_input_focus(X.RevertToParent, X.CurrentTime)
        d.sync()
        codes = [d.keysym_to_keycode(XK.string_to_keysym(k)) for k in keysyms.split('+')]
        for code in codes:
            xtest.fake_input(d, X.KeyPress, code)
        d.sync()
        time.sleep(hold)
        for code in reversed(codes):
            xtest.fake_input(d, X.KeyRelease, code)
        d.sync()
        return True

    def xshot(index):
        from PIL import Image
        geometry = root.get_geometry()
        raw = root.get_image(0, 0, geometry.width, geometry.height, X.ZPixmap, 0xFFFFFFFF)
        Image.frombytes('RGB', (geometry.width, geometry.height), raw.data, 'raw', 'BGRX').save(
            os.path.join(out, 'xshots', '%04d.png' % index))

    if args.xshots:
        os.makedirs(os.path.join(out, 'xshots'), exist_ok=True)
    start = time.time()
    next_shot = start + args.interval
    shot_index = 0
    try:
        while time.time() - start < args.seconds and proc.poll() is None:
            now = time.time()
            while keys and keys[0][0] <= now - start:
                t, k, hold = keys.pop(0)
                print('%.1fs key %s: %s' % (now - start, k, 'sent' if press(k, hold) else 'no window yet'), flush=True)
            if now >= next_shot:
                press('F8')
                if args.xshots:
                    xshot(shot_index)
                shot_index += 1
                next_shot += args.interval
            time.sleep(0.02)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
        xvfb.terminate()
    shots = sorted(glob.glob(os.path.join(out, 'snaps', '*.png')))
    print('%d screenshots in %s' % (len(shots), os.path.join(out, 'snaps')))


if __name__ == '__main__':
    main()
