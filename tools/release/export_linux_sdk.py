#!/usr/bin/env python3
"""Export the existing Steam Runtime SDK into a relocatable, offline player toolchain.

Run inside tools/release/linux.Containerfile's image. --cxx is an extraction of
the approved LLVM libc++ packages; no package is installed on the player's PC.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess


GLIBC = re.compile(r'^(ld-linux|lib(c|m|dl|pthread|rt|resolv|util|anl|nss_[^.]+))\.so')
SDK_LIBRARY = re.compile(
    r'^(ld-linux|crt|[SMr]?crt|lib(c[.-]|c_nonshared|m[.-]|mvec|dl|pthread|rt[.-]|resolv|util|anl|'
    r'gcc|X|xcb|GL|glapi|EGL|vulkan|asound|pulse|wayland|xkbcommon|drm|ffi|bsd|md[.-]|'
    r'z[.-]|expat|fontconfig|freetype|dbus|udev|cap[.-]|pcre|systemd))')


def copy_file(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)  # Dereference SDK symlinks; never leave host-absolute links.


def export_sdk(output, cxx):
    output.mkdir(parents=True, exist_ok=True)
    programs = {'clang': '/usr/lib/llvm-23/bin/clang', 'ld.lld': '/usr/lib/llvm-23/bin/lld',
                'llvm-ar': '/usr/lib/llvm-23/bin/llvm-ar', 'cmake': '/usr/bin/cmake',
                'ninja': '/usr/bin/ninja', 'pkg-config': '/usr/bin/pkg-config',
                'wayland-scanner': '/usr/bin/wayland-scanner'}
    libraries = set()
    for name, path in programs.items():
        target = output / 'cmake/bin/cmake' if name == 'cmake' else output / 'libexec' / name
        copy_file(Path(path), target)
        linked = subprocess.check_output(['ldd', path], text=True)
        for path in re.findall(r'=> (/\S+)', linked):
            library = Path(path)
            if not GLIBC.match(library.name):
                libraries.add(library)
    for library in libraries:
        copy_file(library, output / 'host-libs' / library.name)
    for alias, target in {'clang++': 'clang', 'llvm-ranlib': 'llvm-ar'}.items():
        path = output / 'libexec' / alias
        path.unlink(missing_ok=True)
        path.symlink_to(target)

    shutil.copytree('/usr/lib/llvm-23/lib/clang', output / 'lib/clang', dirs_exist_ok=True)
    shutil.copytree(cxx / 'usr/lib/llvm-23/include/c++', output / 'include/c++', dirs_exist_ok=True)
    for source in (cxx / 'usr/lib/llvm-23/lib').glob('*'):
        if source.is_file():
            copy_file(source, output / 'lib' / source.name)
    shutil.copytree('/usr/share/cmake-3.25', output / 'cmake/share/cmake-3.25', dirs_exist_ok=True)

    sysroot = output / 'sysroot'
    shutil.copytree('/usr/include', sysroot / 'usr/include', dirs_exist_ok=True)
    for directory in ('usr/lib/x86_64-linux-gnu', 'lib/x86_64-linux-gnu'):
        for source in Path('/' + directory).iterdir():
            if source.is_file() and SDK_LIBRARY.match(source.name):
                copy_file(source, sysroot / directory / source.name)
    for source in Path('/usr/lib/gcc/x86_64-linux-gnu/10').glob('*'):
        if source.is_file() and (source.name.startswith(('crt', 'libgcc', 'libgcov'))):
            copy_file(source, sysroot / 'usr/lib/gcc/x86_64-linux-gnu/10' / source.name)
    copy_file(Path('/lib64/ld-linux-x86-64.so.2'), sysroot / 'lib64/ld-linux-x86-64.so.2')
    for directory in ('usr/lib/x86_64-linux-gnu/pkgconfig', 'usr/share/pkgconfig'):
        shutil.copytree('/' + directory, sysroot / directory, dirs_exist_ok=True)
    for directory in ('/usr/share/doc', str(cxx / 'usr/share/doc')):
        for source in Path(directory).rglob('*'):
            if source.is_file():  # SDK images strip changelogs, leaving some dangling documentation links.
                copy_file(source, output / 'licenses' / source.relative_to(directory))

    bin_dir = output / 'bin'
    bin_dir.mkdir(exist_ok=True)
    wrapper = bin_dir / 'run-tool'
    wrapper.write_text('''#!/bin/sh
set -eu
tool=$(basename "$0")
sdk=$(dirname "$(dirname "$(readlink -f "$0")")")
export LD_LIBRARY_PATH="$sdk/host-libs:$sdk/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$sdk/bin:$PATH"
case "$tool" in
  cmake)
    exec "$sdk/cmake/bin/cmake" "$@" ;;
  clang|clang++)
    exec "$sdk/libexec/$tool" --sysroot="$sdk/sysroot" --gcc-toolchain="$sdk/sysroot/usr" \\
      -resource-dir="$sdk/lib/clang/23" -L"$sdk/lib" "$@" ;;
  pkg-config)
    export PKG_CONFIG_SYSROOT_DIR="$sdk/sysroot"
    export PKG_CONFIG_LIBDIR="$sdk/sysroot/usr/lib/x86_64-linux-gnu/pkgconfig:$sdk/sysroot/usr/share/pkgconfig" ;;
esac
exec "$sdk/libexec/$tool" "$@"
''')
    wrapper.chmod(0o755)
    for name in (*programs, 'clang++', 'llvm-ranlib'):
        path = bin_dir / name
        path.unlink(missing_ok=True)
        path.symlink_to('run-tool')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cxx', type=Path, required=True)
    args = parser.parse_args()
    export_sdk(args.output, args.cxx)
