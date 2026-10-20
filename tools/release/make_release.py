#!/usr/bin/env python3
"""Release packages (issue 011 v5): the *builder* — the launcher, the recompiler, the source code, the compiler and
the tools — never the compiled game (PS2Recomp is GPL-3.0; the player's PC builds the game from their own disc).
The package holds nothing from the game, so any PC (and later CI) can make it; publishing is the owner's.

  tools/release/make_release.py windows --build app/build-release-win --recomp-build build-recomp-win [--test]

Checklist enforced here:
  * a clean working tree (--test skips this and names the package "...-test"; never publish a test package);
  * the package holds only our files and freely licensed tools; the scan refuses a game binary, generated or lifted
    code, disc files, an ISO, and file names Windows does not allow;
  * BUILD_ID in the package (the player's build shows it); SHA-256 next to the zip.
Not checked here (do it before publishing): `run.py full` green on this commit, the owner's OK.
"""
import argparse, hashlib, os, re, shutil, subprocess, sys, zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LLVM_MINGW_HOST = Path(os.environ.get('LLVM_MINGW_ROOT',
                                      Path.home() / '.local/opt/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64'))
WIN_TOOLS = Path(os.environ.get('DW5XL_WIN_TOOLS', Path.home() / '.local/opt/windows-builder-tools'))
LLVM_MINGW_WIN_ZIP = WIN_TOOLS / 'llvm-mingw-20261006-ucrt-x86_64.zip'
CMAKE_WIN_ZIP = WIN_TOOLS / 'cmake-4.4.4-windows-x86_64.zip'
NINJA_WIN_ZIP = WIN_TOOLS / 'ninja-win.zip'
FFMPEG_MIN_WINDOWS = Path(os.environ.get('DW5XL_FFMPEG_MIN', Path.home() / '.local/opt/ffmpeg-min-windows'))

README_WINDOWS = """Dynasty Warriors 5 Xtreme Legends - PC port
============================================

A native Windows port made by static recompilation: the game's PS2 code is translated to C++
and compiled for your PC. It is not an emulator.

THIS DOWNLOAD CONTAINS NOTHING FROM THE GAME - no game files and no game code. It is a builder:
on the first start it makes the game on your PC from your own disc. You need:
  * Dynasty Warriors 5 Xtreme Legends, PAL (SLES-53860), as a disc image (.iso)
  * optional, for "Load Original": Dynasty Warriors 5, PAL (SLES-53339), as a disc image
You do not need to install anything: the compiler and all tools are in this folder.

Play
  1. Unzip this folder anywhere (keep the folder together).
  2. Start DW5XL.exe. Windows may say "Windows protected your PC" because the program is not
     signed: choose "More info", then "Run anyway".
  3. First start: choose your disc image and press Start. Your disc is checked and copied, the
     game's code is translated and compiled for your PC. This happens once and takes a few
     minutes (longer on older PCs). Then the game starts.
  4. Every later start of DW5XL.exe starts the game at once.
  5. Load Original: when the game asks for the Dynasty Warriors 5 disc, a window opens where you
     choose that disc image; the game continues by itself once it is installed (only the first time).

Your files
  Game files, the built game, saves, settings and logs are in %LOCALAPPDATA%\\DW5XL.
  A newer version of this folder rebuilds the game once and keeps your saves.

Keys
  F3  frame rate overlay     F4  output resolution (1x / 2x / 4x)
  F5  anti-aliasing          F9  discs (add or check your disc images)

Source code: https://github.com/TheSlackbringer/dw5xl-recomp (GPL-3.0), version {build}.
Licenses of everything included are in the LICENSES folder; the compiler and CMake also keep
theirs in their own folders under tools.
This is a fan project, not affiliated with or endorsed by Koei Tecmo or Sony.
"""


def fail(msg):
    sys.exit(f'release: {msg}')


def git(*args, cwd=REPO):
    return subprocess.run(['git', '-C', str(cwd), *args], capture_output=True, text=True, check=True).stdout


def disc_file_names():
    text = (REPO / 'app/setup/disc_manifest.h').read_text()
    return {Path(m).name.upper() for m in re.findall(r'\{"([^"]+)",\s*\d+u', text)}


def version_for(test):
    if test:
        return git('describe', '--always').strip() + '-test'
    if git('status', '--porcelain', '--untracked-files=no').strip():
        fail('the working tree has changes; commit or use --test')
    return git('describe', '--tags', '--always').strip()


def copy_tracked(src_repo, rel_paths, dest, skip=()):
    """Copies the files git tracks under rel_paths of src_repo (working-tree contents) to dest."""
    files = git('ls-files', '-z', '--recurse-submodules', '--', *rel_paths, cwd=src_repo).split('\0')
    n = 0
    for f in filter(None, files):
        if any(f.startswith(s) for s in skip) or (src_repo / f).is_dir():  # a submodule that is not checked out
            continue
        target = dest / f
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src_repo / f, target)
        n += 1
    return n


def dll_closure(exe, search):
    readobj = LLVM_MINGW_HOST / 'bin' / 'llvm-readobj'
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


def unzip_into(archive, dest, strip_top=True):
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            parts = Path(info.filename).parts
            rel = Path(*parts[1:]) if strip_top and len(parts) > 1 else Path(*parts)
            if not rel.parts:
                continue
            target = dest / rel
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, open(target, 'wb') as out:
                shutil.copyfileobj(src, out)


def apache_text():
    text = (LLVM_MINGW_HOST / 'LICENSE.TXT').read_text()
    start = text.index('Apache License')
    end = text.index('---- LLVM Exceptions')
    return text[start:end].rstrip() + '\n'


# Text only the compiled game contains (its disc check, app/src/dw5xl_overlays.cpp): finds the game under any name.
GAME_MARKER = b'menu verification disc='


def scan(stage):
    """Nothing from the game may be in a release package."""
    forbidden_names = disc_file_names() | {'DW5XL.ELF'}
    bad = []
    for p in stage.rglob('*'):
        rel = p.relative_to(stage).as_posix()
        name = p.name
        if any(c in name for c in '<>:"|?*\\'):
            bad.append(f'{rel} (a character Windows does not allow in file names)')
        if p.is_dir():
            continue
        low = rel.lower()
        if (name.upper() in forbidden_names or low.endswith(('.iso', '.elf', '.irx')) or
                re.search(r'(^|/)recomp/.*/output/', low) or re.search(r'(^|/)recomp/dw5xl/vu1/', low) or
                re.match(r'fun_[0-9a-f]+_0x[0-9a-f]+\.cpp$', name.lower()) or 'vu1_lifted' in low):
            bad.append(rel)
        elif low.endswith(('.exe', '.dll')) and GAME_MARKER in p.read_bytes():
            bad.append(f'{rel} (the compiled game)')
    if bad:
        fail('not allowed in a release package: ' + ', '.join(sorted(bad)[:20]))


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def windows(build, recomp_build, test):
    launcher = build / 'dw5xl_setup.exe'
    recompiler = recomp_build / 'ps2xRecomp' / 'ps2_recomp.exe'
    for path in (launcher, recompiler, LLVM_MINGW_WIN_ZIP, CMAKE_WIN_ZIP, NINJA_WIN_ZIP,
                 Path(f'{FFMPEG_MIN_WINDOWS}.zip')):
        if not path.exists():
            fail(f'{path} missing')
    version = version_for(test)
    name = f'DW5XL-{version}-windows-x64'
    dist = REPO / 'dist'
    stage = dist / name
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir(parents=True)

    # The launcher, the program the player starts.
    shutil.copy2(launcher, stage / 'DW5XL.exe')
    host_dlls = [build, LLVM_MINGW_HOST / 'x86_64-w64-mingw32/bin']
    for dll in dll_closure(launcher, host_dlls):
        shutil.copy2(dll, stage / dll.name)
    (stage / 'BUILD_ID').write_text(version + '\n')

    # Our source (only what git tracks: never anything game-derived) and the libraries' sources.
    n = copy_tracked(REPO, ['app', 'external/PS2Recomp', 'recomp', 'tools/windows', 'LICENSE'], stage,
                     skip=('external/PS2Recomp/android/', 'external/PS2Recomp/vita/', 'external/PS2Recomp/ps2xStudio/'))
    n += copy_tracked(REPO / 'app/deps/parallel-gs', ['.'], stage / 'app/deps/parallel-gs')
    deps = build / '_deps'
    n += copy_tracked(deps / 'raylib-src', ['.'], stage / 'app/deps/raylib-src', skip=('examples/', 'projects/'))
    n += copy_tracked(deps / 'imgui-src', ['.'], stage / 'app/deps/imgui-src', skip=('examples/', 'docs/'))
    n += copy_tracked(deps / 'rlimgui-src', ['.'], stage / 'app/deps/rlimgui-src', skip=('examples/',))

    # Tools: recompiler, compiler, CMake, Ninja, FFmpeg.
    tools = stage / 'tools'
    shutil.copy2(recompiler, tools / 'ps2_recomp.exe')
    for dll in dll_closure(recompiler, host_dlls):
        shutil.copy2(dll, tools / dll.name)
    unzip_into(LLVM_MINGW_WIN_ZIP, tools / 'llvm-mingw')
    unzip_into(CMAKE_WIN_ZIP, tools / 'cmake')
    unzip_into(NINJA_WIN_ZIP, tools, strip_top=False)
    shutil.copy2(f'{FFMPEG_MIN_WINDOWS}.zip', tools / 'ffmpeg.zip')

    # Player README and licenses.
    (stage / 'README.txt').write_text(README_WINDOWS.format(build=version).replace('\n', '\r\n'))
    lic = stage / 'LICENSES'
    lic.mkdir()
    pgs = REPO / 'app/deps/parallel-gs'
    for target, source in {
        'dw5xl (GPL-3.0).txt': REPO / 'LICENSE',
        'PS2Recomp (GPL-3.0).txt': REPO / 'external/PS2Recomp/LICENSE',
        'paraLLEl-GS (LGPL-3.0).txt': pgs / 'COPYING.LGPLv3',
        'Granite (MIT).txt': pgs / 'Granite/LICENSE',
        'volk (MIT).txt': pgs / 'Granite/third_party/volk/LICENSE.md',
        'raylib (zlib).txt': deps / 'raylib-src/LICENSE',
        'GLFW (zlib).txt': deps / 'raylib-src/src/external/glfw/LICENSE.md',
        'Dear ImGui (MIT).txt': deps / 'imgui-src/LICENSE.txt',
        'rlImGui (zlib).txt': deps / 'rlimgui-src/LICENSE',
        'llvm-mingw (Apache-2.0 WITH LLVM-exception and others).txt': LLVM_MINGW_HOST / 'LICENSE.TXT',
        'FFmpeg (LGPL-2.1).txt': FFMPEG_MIN_WINDOWS / 'licenses/ffmpeg/COPYING.LGPLv2.1',
    }.items():
        if not source.exists():
            fail(f'license file missing: {source}')
        shutil.copy2(source, lic / target)
    (lic / 'Ninja (Apache-2.0).txt').write_text('Ninja - https://ninja-build.org - Copyright Google Inc. and contributors.\n'
                                                'Licensed under the Apache License, Version 2.0:\n\n' + apache_text())
    (lic / 'CMake (BSD-3-Clause).txt').write_text('CMake - https://cmake.org - BSD-3-Clause. Its license and the '
                                                  'licenses of its parts are in tools/cmake/doc/cmake.\n')

    scan(stage)
    archive = dist / f'{name}.zip'
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for p in sorted(stage.rglob('*')):
            if p.is_file():
                z.write(p, Path(name) / p.relative_to(stage))
    (dist / f'{name}.zip.sha256').write_text(f'{sha256(archive)}  {archive.name}\n')
    files = sum(1 for p in stage.rglob('*') if p.is_file())
    print(f'{archive} ({archive.stat().st_size / 2**20:.0f} MB, {files} files, {n} source files)')
    return archive


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('target', choices=['windows'])
    ap.add_argument('--build', required=True, type=Path, help='Windows build folder with dw5xl_setup.exe')
    ap.add_argument('--recomp-build', required=True, type=Path, help='Windows build folder of the recompiler')
    ap.add_argument('--test', action='store_true', help='allow a dirty tree; the package is named "-test"')
    args = ap.parse_args()
    windows(args.build.resolve(), args.recomp_build.resolve(), args.test)


if __name__ == '__main__':
    main()
