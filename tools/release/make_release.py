#!/usr/bin/env python3
"""Release packages (issue 011 v5): the *builder* — the launcher, the recompiler, the source code, the compiler and
the tools — never the compiled game (PS2Recomp is GPL-3.0; the player's PC builds the game from their own disc).
The package holds nothing from the game, so any PC (and later CI) can make it; publishing is the owner's.

  tools/release/make_release.py windows --build app/build-release-win --recomp-build build-recomp-win [--test]
  tools/release/build_linux.sh [--test]  # offline inputs: see tools/release/LINUX.md

Checklist enforced here:
  * a clean working tree (--test skips this and names the package "...-test"; never publish a test package);
  * the package holds only our files and freely licensed tools; the scan refuses a game binary, generated or lifted
    code, disc files, an ISO, and file names Windows does not allow;
  * BUILD_ID in the package (the player's build shows it); SHA-256 next to the zip.
Not checked here (do it before publishing): `run.py full` green on this commit, the owner's OK.
"""
import argparse, hashlib, os, re, shutil, subprocess, sys, zipfile
from pathlib import Path

import zsync_file

REPO = Path(__file__).resolve().parents[2]
# Issue 044: AppImage updaters look for this file pattern in the latest full GitHub release.
UPDATE_INFORMATION = 'gh-releases-zsync|TheSlackbringer|dw5xl-recomp|latest|DW5XL-RECOMP-*x86_64.AppImage.zsync'
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
  F2  frame rate overlay     F3  anti-aliasing
  F4  output resolution (1x / 2x / 4x)
  F8  build ID (for bug reports)
  F10 discs (add or check your disc images)

Source code: https://github.com/TheSlackbringer/dw5xl-recomp (GPL-3.0), version {build}.
Licenses of everything included are in the LICENSES folder; the compiler and CMake also keep
theirs in their own folders under tools.
This is a fan project, not affiliated with or endorsed by Koei Tecmo or Sony.
"""


def fail(msg):
    sys.exit(f'release: {msg}')


def git(*args, cwd=REPO):
    # Packaging reads host-owned checkouts in a container. Trust only this checkout
    # for this invocation; never change the user's global Git trust settings.
    return subprocess.run(['git', '-c', f'safe.directory={cwd.resolve()}', '-C', str(cwd), *args],
                          env={**os.environ, 'GIT_OPTIONAL_LOCKS': '0'},
                          capture_output=True, text=True, check=True).stdout


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


def scan(stage, windows=True):
    """Nothing from the game may be in a release package."""
    forbidden_names = disc_file_names() | {'DW5XL.ELF'}
    bad = []
    for p in stage.rglob('*'):
        rel = p.relative_to(stage).as_posix()
        name = p.name
        if windows and any(c in name for c in '<>:"|?*\\'):
            bad.append(f'{rel} (a character Windows does not allow in file names)')
        if p.is_dir():
            continue
        low = rel.lower()
        if (name.upper() in forbidden_names or low.endswith(('.iso', '.elf', '.irx')) or
                re.search(r'(^|/)recomp/.*/output/', low) or
                re.search(r'(^|/)recomp/[^/]+/(vu0|vu1|iop)/', low) or
                re.match(r'fun_[0-9a-f]+_0x[0-9a-f]+\.cpp$', name.lower()) or
                re.search(r'vu[01]_lifted', low)):
            bad.append(rel)
        else:
            with p.open('rb') as stream:
                header = stream.read(4)
                if header[:2] == b'MZ' or header == b'\x7fELF':
                    previous = header
                    while block := stream.read(1024 * 1024):
                        if GAME_MARKER in previous + block:
                            bad.append(f'{rel} (the compiled game)')
                            break
                        previous = block[-len(GAME_MARKER):]
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


def linux_package_name(version):
    # Issue 044: named after the project; no "linux", since every AppImage is for Linux.
    return f'DW5XL-RECOMP-{version}-x86_64'


def linux(build, recomp_build, sdk, ffmpeg, appimage_tools, test):
    """Build an AppDir containing the setup launcher, sources and an offline SDK."""
    launcher = build / 'dw5xl_setup'
    recompiler = recomp_build / 'ps2xRecomp/ps2_recomp'
    for path in (launcher, recompiler, sdk / 'bin/clang++', ffmpeg / 'include/libavcodec/avcodec.h',
                 appimage_tools / 'linuxdeploy-x86_64.AppImage', appimage_tools / 'appimagetool-x86_64.AppImage'):
        if not path.exists():
            fail(f'{path} missing')
    version = version_for(test)
    name = linux_package_name(version)
    dist = REPO / 'dist'
    stage = dist / f'{name}.AppDir'
    if stage.exists():
        shutil.rmtree(stage)
    (stage / 'usr/bin').mkdir(parents=True)
    (stage / 'usr/lib').mkdir()
    copy_tracked(REPO, ['app', 'external/PS2Recomp', 'recomp', 'tools/linux', 'LICENSE'], stage,
                 skip=('external/PS2Recomp/android/', 'external/PS2Recomp/vita/', 'external/PS2Recomp/ps2xStudio/'))
    dep_root = Path(os.environ.get('DW5XL_PACKAGE_DEP_ROOT', REPO / 'app/deps'))
    pgs = dep_root / 'parallel-gs'
    copy_tracked(pgs, ['.'], stage / 'app/deps/parallel-gs')
    for dep in ('raylib', 'imgui', 'rlimgui'):
        source = build / '_deps' / f'{dep}-src'
        if not source.exists():
            source = dep_root / f'{dep}-src'
        copy_tracked(source, ['.'], stage / 'app/deps' / f'{dep}-src')
    shutil.copy2(launcher, stage / 'usr/bin/dw5xl_setup')
    shutil.copy2(recompiler, stage / 'tools/ps2_recomp')
    shutil.copytree(sdk, stage / 'tools/clang', symlinks=True)
    shutil.copytree(ffmpeg, stage / 'tools/ffmpeg', symlinks=True)
    (stage / 'tools/cmake/bin').mkdir(parents=True)
    (stage / 'tools/cmake/bin/cmake').symlink_to('../../clang/bin/cmake')
    (stage / 'tools/ninja').symlink_to('clang/bin/ninja')
    for prefix in (sdk / 'lib', ffmpeg / 'lib'):
        for library in prefix.glob('*.so*'):
            shutil.copy2(library, stage / 'usr/lib' / library.name)
    (stage / 'BUILD_ID').write_text(version + '\n')
    (stage / 'README.txt').write_text(
        'Dynasty Warriors 5 Xtreme Legends - Linux builder\n\n'
        'Make the AppImage executable in your file manager and open it. Choose your PAL XL disc image\n'
        '(SLES-53860); optional DW5 (SLES-53339) enables Load Original. The first start builds the game\n'
        'from your own disc. The compiler and all build tools are included; no downloads are needed.\n'
        'Later starts open the game directly. Saves, settings, game data and logs stay in\n'
        '$XDG_DATA_HOME/dw5xl (normally ~/.local/share/dw5xl), separate from this AppImage.\n\n'
        'Requires x86-64 with AVX2, Linux glibc 2.31 or newer and a Vulkan graphics driver.\n'
        f'Source: https://github.com/TheSlackbringer/dw5xl-recomp, build {version}, GPL-3.0.\n'
        'The download contains our sources and freely licensed tools, with no game data or game binary.\n')
    licenses = stage / 'LICENSES'
    licenses.mkdir()
    for label, path in {
        'DW5XL-GPL-3.0.txt': REPO / 'LICENSE',
        'PS2Recomp-GPL-3.0.txt': REPO / 'external/PS2Recomp/LICENSE',
        'parallel-gs-LGPL-3.0.txt': pgs / 'COPYING.LGPLv3',
        'Granite-MIT.txt': pgs / 'Granite/LICENSE',
        'volk-MIT.txt': pgs / 'Granite/third_party/volk/LICENSE.md',
        'FFmpeg-LGPL-2.1.txt': ffmpeg / 'licenses/ffmpeg/COPYING.LGPLv2.1',
    }.items():
        shutil.copy2(path, licenses / label)
    (licenses / 'TOOLCHAIN.txt').write_text('Compiler, SDK, CMake and Ninja copyright notices are in tools/clang/licenses.\n')
    icon = REPO / 'tools/release/dw5xl.svg'
    desktop = REPO / 'tools/release/dw5xl.desktop'
    env = {**os.environ, 'APPIMAGE_EXTRACT_AND_RUN': '1',
           'LD_LIBRARY_PATH': str(stage / 'usr/lib'), 'ARCH': 'x86_64'}
    subprocess.run([str(appimage_tools / 'linuxdeploy-x86_64.AppImage'), '--appdir', str(stage),
                    '-e', str(stage / 'usr/bin/dw5xl_setup'), '-d', str(desktop), '-i', str(icon)], env=env, check=True)
    # linuxdeploy handles runtime dependencies; use our launcher to keep the release root explicit.
    app_run = stage / 'AppRun'
    app_run.unlink(missing_ok=True)
    app_run.write_text('''#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$root/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$root/usr/bin/dw5xl_setup" --root "$root" "$@"
''')
    app_run.chmod(0o755)
    scan(stage, windows=False)
    archive = dist / f'{name}.AppImage'
    image_tool = appimage_tools / 'appimagetool-x86_64.AppImage'
    # Reuse the runtime already present in the approved tool download. The tool's
    # default is to fetch another runtime, which would break an offline build.
    offset = int(subprocess.check_output([str(image_tool), '--appimage-offset'], text=True).strip())
    runtime = build / 'appimage-runtime-x86_64'
    with image_tool.open('rb') as stream:
        runtime.write_bytes(stream.read(offset))
    # -u embeds the update information and has appimagetool's bundled zsyncmake write the .zsync, into the current
    # folder. appimagetool only warns when zsyncmake is missing, so the .zsync is checked here.
    zsync = dist / f'{archive.name}.zsync'
    zsync.unlink(missing_ok=True)
    subprocess.run([str(image_tool), '--no-appstream', '--runtime-file', str(runtime), '-u', UPDATE_INFORMATION,
                    str(stage), str(archive)], env=env, check=True, cwd=dist)
    try:
        zsync_file.check(zsync, archive)
    except (OSError, ValueError) as error:
        fail(f'no matching update file next to the AppImage: {error}')
    Path(str(archive) + '.sha256').write_text(f'{sha256(archive)}  {archive.name}\n')
    print(f'{archive} ({archive.stat().st_size / 2**20:.0f} MB)')
    return archive


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('target', choices=['windows', 'linux'])
    ap.add_argument('--build', required=True, type=Path, help='build folder with the setup launcher')
    ap.add_argument('--recomp-build', required=True, type=Path, help='build folder of the native recompiler')
    ap.add_argument('--test', action='store_true', help='allow a dirty tree; the package is named "-test"')
    ap.add_argument('--linux-sdk', type=Path)
    ap.add_argument('--ffmpeg-root', type=Path)
    ap.add_argument('--appimage-tools', type=Path, default=Path.home() / '.local/opt/appimage-tools')
    args = ap.parse_args()
    if args.target == 'windows':
        windows(args.build.resolve(), args.recomp_build.resolve(), args.test)
    else:
        if not args.linux_sdk or not args.ffmpeg_root:
            ap.error('Linux requires --linux-sdk and --ffmpeg-root')
        linux(args.build.resolve(), args.recomp_build.resolve(), args.linux_sdk.resolve(),
              args.ffmpeg_root.resolve(), args.appimage_tools.resolve(), args.test)


if __name__ == '__main__':
    main()
