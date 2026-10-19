#!/usr/bin/env bash
# Minimal FFmpeg for release builds (issue 011 M8): only what the game uses — SHA-256 (libavutil) and the MPEG-2
# video decoder for the movies — as shared libraries (LGPL), with no external libraries and no hand-written assembly
# (no nasm needed; PS2-resolution MPEG-2 decodes easily in C). The same source tarball
# (ffmpeg.org, signature checked) is built for Linux (inside the Steam Runtime container) and for Windows
# (cross-compiled with llvm-mingw).
#   tools/release/build_ffmpeg.sh linux   <ffmpeg-7.1.tar.xz> <prefix>
#   tools/release/build_ffmpeg.sh windows <ffmpeg-7.1.tar.xz> <prefix> [llvm-mingw root]
# Windows also writes <prefix>.zip in the layout the runtime's CMake expects for PS2X_FFMPEG_PREBUILT_URL
# (bin/*.dll, bin/*.lib import libraries, include/, licenses/).
set -euo pipefail
target=${1:?linux|windows}
tarball=$(realpath "${2:?ffmpeg source tarball}")
prefix=$(realpath -m "${3:?install prefix}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

tar -xJf "$tarball" -C "$work"
src=$(echo "$work"/ffmpeg-*)

common=(--prefix="$prefix" --enable-shared --disable-static --disable-programs --disable-doc --disable-debug
        --disable-autodetect --disable-everything --disable-network --disable-x86asm
        --disable-avdevice --disable-avfilter
        --enable-avcodec --enable-avformat --enable-avutil --enable-swresample --enable-swscale
        --enable-decoder=mpeg2video --enable-parser=mpegvideo)

case "$target" in
linux)
    (cd "$src" && ./configure "${common[@]}" --enable-pic)
    ;;
windows)
    root=${4:-$HOME/.local/opt/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64}
    export PATH="$root/bin:$PATH"
    (cd "$src" && ./configure "${common[@]}" --target-os=mingw32 --arch=x86_64 \
        --cross-prefix=x86_64-w64-mingw32- --cc=x86_64-w64-mingw32-clang --cxx=x86_64-w64-mingw32-clang++)
    ;;
*)
    echo "unknown target $target" >&2
    exit 2
    ;;
esac

make -C "$src" -j"$(nproc)"
rm -rf "$prefix"
make -C "$src" install
mkdir -p "$prefix/licenses/ffmpeg"
cp "$src"/COPYING.LGPLv2.1 "$src"/LICENSE.md "$prefix/licenses/ffmpeg/"

if [ "$target" = windows ]; then
    # The runtime links bin/<name>.lib (MSVC naming); MinGW import libraries work the same with lld.
    for lib in avcodec avformat avutil swresample swscale; do
        cp "$prefix/lib/lib$lib.dll.a" "$prefix/bin/$lib.lib"
    done
    rm -f "$prefix.zip"
    (cd "$prefix" && python3 -c "
import os, zipfile
with zipfile.ZipFile('$prefix.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk('.'):
        for f in files:
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, '.'))
")
    echo "wrote $prefix.zip"
fi
echo "installed minimal FFmpeg ($target) in $prefix"
