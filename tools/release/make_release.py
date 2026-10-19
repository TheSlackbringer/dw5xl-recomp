#!/usr/bin/env python3
"""Release packages (issue 011 M8, the Unleashed model). Run on the owner's PC only: the game is compiled from
code generated from the owner's own disc, so CI can never build it. Publishing is done by the owner.

  tools/release/make_release.py windows --build app/build-release-win [--test]

Checklist enforced here:
  * the working tree is clean and the game's build ID is this commit (no "-dirty");
    --test skips this and names the package "...-test" (never publish a test package);
  * the package holds only: the game, the libraries it loads, README.txt, LICENSES/ — never disc files, an ISO,
    generated or lifted source, or any other game-derived file (scan below);
  * SHA256SUMS next to the package.
Not checked here (do it before): `run.py full` green on this commit, the owner's OK to publish.
"""
import argparse, hashlib, os, re, shutil, subprocess, sys, zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LLVM_MINGW = Path(os.environ.get('LLVM_MINGW_ROOT',
                                 Path.home() / '.local/opt/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64'))
FFMPEG_MIN_WINDOWS = Path(os.environ.get('DW5XL_FFMPEG_MIN', Path.home() / '.local/opt/ffmpeg-min-windows'))

README_WINDOWS = """Dynasty Warriors 5 Xtreme Legends - PC port
============================================

A native Windows port made by static recompilation: the game's PS2 code was translated to
C++ and compiled for the PC. It is not an emulator.

THIS DOWNLOAD CONTAINS NO GAME FILES. You need your own copy of the game:
  * Dynasty Warriors 5 Xtreme Legends, PAL (SLES-53860), as a disc image (.iso)
  * optional, for "Load Original": Dynasty Warriors 5, PAL (SLES-53339), as a disc image

Play
  1. Unzip this folder anywhere.
  2. Start dw5xl.exe. Windows may say "Windows protected your PC" because the program is not
     signed: choose "More info", then "Run anyway".
  3. On the first start, choose your disc image and press Install. The game files are checked
     and copied from your disc once (about 1-2 minutes). Then the game starts.
  4. Load Original: when the game asks for the Dynasty Warriors 5 disc, a window opens where you
     choose that disc image; then choose NO at the game's "Cancel data load?" question.

Your files
  Game files from your disc, memory card (saves), settings and logs are in
  %LOCALAPPDATA%\\DW5XL. To update, replace this folder with the new version; your saves stay.

Keys
  F3  frame rate overlay     F4  output resolution (1x / 2x / 4x)
  F5  anti-aliasing          F9  discs (add or check your disc images)

Source code: https://github.com/TheSlackbringer/dw5xl-recomp (GPL-3.0), build {build}.
FFmpeg (LGPL-2.1) source: https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz
Licenses of everything included are in the LICENSES folder.
This is a fan project, not affiliated with or endorsed by Koei Tecmo or Sony.
"""

# Disc file names (from app/setup/disc_manifest.h): none may ever be in a package.
def disc_file_names():
    text = (REPO / 'app/setup/disc_manifest.h').read_text()
    return {Path(m).name.upper() for m in re.findall(r'\{"([^"]+)",\s*\d+u', text)}


def fail(msg):
    sys.exit(f'release: {msg}')


def git(*args):
    return subprocess.run(['git', '-C', str(REPO), *args], capture_output=True, text=True).stdout.strip()


def check_tree_and_build_id(exe, test):
    head = git('describe', '--always')
    data = exe.read_bytes()
    if test:
        return head + '-test'
    if git('status', '--porcelain', '--untracked-files=no'):
        fail('the working tree has changes; commit or use --test')
    if head.encode() not in data or f'{head}-dirty'.encode() in data:
        fail(f'{exe} was not built from {head} without changes (rebuild after committing)')
    return git('describe', '--tags', '--always')


def dll_closure(exe, search):
    readobj = LLVM_MINGW / 'bin' / 'llvm-readobj'
    found, todo = {}, [exe]
    while todo:
        out = subprocess.run([str(readobj), '--coff-imports', str(todo.pop())], capture_output=True, text=True).stdout
        for line in out.splitlines():
            line = line.strip()
            if not (line.startswith('Name:') and line.lower().endswith('.dll')):
                continue
            name = line.split(':', 1)[1].strip()
            if name.lower() in found:
                continue
            for d in search:
                hit = next((p for p in d.glob('*') if p.name.lower() == name.lower()), None)
                if hit:
                    found[name.lower()] = hit
                    todo.append(hit)
                    break
    return sorted(found.values(), key=lambda p: p.name.lower())


def licenses(build):
    deps = build / '_deps'
    pgs = REPO / 'app/deps/parallel-gs'
    wanted = {
        'dw5xl (GPL-3.0).txt': REPO / 'LICENSE',
        'PS2Recomp (GPL-3.0).txt': REPO / 'external/PS2Recomp/LICENSE',
        'paraLLEl-GS (LGPL-3.0).txt': pgs / 'COPYING.LGPLv3',
        'Granite (MIT).txt': pgs / 'Granite/LICENSE',
        'volk (MIT).txt': pgs / 'Granite/third_party/volk/LICENSE.md',
        'raylib (zlib).txt': deps / 'raylib-src/LICENSE',
        'GLFW (zlib).txt': deps / 'raylib-src/src/external/glfw/LICENSE.md',
        'Dear ImGui (MIT).txt': deps / 'imgui-src/LICENSE.txt',
        'rlImGui (zlib).txt': deps / 'rlimgui-src/LICENSE',
        'llvm-mingw runtime - libc++, libunwind (Apache-2.0 WITH LLVM-exception).txt': LLVM_MINGW / 'LICENSE.TXT',
        'FFmpeg (LGPL-2.1).txt': FFMPEG_MIN_WINDOWS / 'licenses/ffmpeg/COPYING.LGPLv2.1',
    }
    missing = [str(p) for p in wanted.values() if not p.exists()]
    if missing:
        fail('license files missing: ' + ', '.join(missing))
    return wanted


def scan(stage, allowed_dlls):
    forbidden = disc_file_names()
    bad = []
    for p in stage.rglob('*'):
        if p.is_dir():
            continue
        rel = p.relative_to(stage)
        name = p.name
        ok = (rel == Path('dw5xl.exe') or (rel.parent == Path('.') and name in allowed_dlls) or
              rel == Path('README.txt') or (rel.parts[0] == 'LICENSES' and name.endswith(('.txt', '.md'))))
        if not ok or name.upper() in forbidden or name.lower().endswith(('.iso', '.bin', '.elf', '.cpp', '.h', '.irx')):
            bad.append(str(rel))
        elif any(c in name for c in '<>:"|?*\\'):
            bad.append(f'{rel} (a character Windows does not allow in file names)')
    if bad:
        fail('not allowed in a release package: ' + ', '.join(bad))


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def windows(build, test):
    exe = build / 'dw5xl.exe'
    if not exe.exists():
        fail(f'{exe} missing: build the dw5xl target first')
    version = check_tree_and_build_id(exe, test)
    name = f'DW5XL-{version}-windows-x64'
    dist = REPO / 'dist'
    stage = dist / name
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir(parents=True)
    shutil.copy2(exe, stage / 'dw5xl.exe')
    dlls = dll_closure(exe, [build, LLVM_MINGW / 'x86_64-w64-mingw32/bin'])
    for dll in dlls:
        shutil.copy2(dll, stage / dll.name)
    (stage / 'README.txt').write_text(README_WINDOWS.format(build=version).replace('\n', '\r\n'))
    (stage / 'LICENSES').mkdir()
    for target, source in licenses(build).items():
        shutil.copy2(source, stage / 'LICENSES' / target)
    scan(stage, {d.name for d in dlls})
    archive = dist / f'{name}.zip'
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(stage.rglob('*')):
            z.write(p, Path(name) / p.relative_to(stage))
    (dist / f'{name}.zip.sha256').write_text(f'{sha256(archive)}  {archive.name}\n')
    size = archive.stat().st_size / 2**20
    print(f'{archive} ({size:.0f} MB): dw5xl.exe + {len(dlls)} DLLs ({", ".join(d.name for d in dlls)})')
    return archive


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('target', choices=['windows'])
    ap.add_argument('--build', required=True, type=Path)
    ap.add_argument('--test', action='store_true', help='allow a dirty tree; the package is named "-test"')
    args = ap.parse_args()
    windows(args.build.resolve(), args.test)


if __name__ == '__main__':
    main()
