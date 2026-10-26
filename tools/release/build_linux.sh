#!/usr/bin/env bash
# Build the Linux *builder* AppImage offline with the existing Steam Runtime SDK.
# No game files or generated game code are needed. Nothing is installed on the host.
# Required local inputs: the builder image (issue 041; pinned in tools/release/linux-builder-image, it holds the
# compiler and the approved libc++ packages), dependency source checkouts, the FFmpeg source tarball, and
# linuxdeploy/appimagetool (see issue 011 notes).
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
test_args=()
if [ "${1:-}" = --test ]; then test_args=(--test); shift; fi
if [ "$#" -ne 0 ]; then echo 'usage: build_linux.sh [--test]' >&2; exit 2; fi
sdk_work=${DW5XL_LINUX_SDK_WORK:-"$repo/app/build-release-linux"}
image=${DW5XL_LINUX_IMAGE:-$(cat "$repo/tools/release/linux-builder-image")}
deps=$(realpath "${DW5XL_DEP_ROOT:-$repo/app/deps}")
recomp_deps=$(realpath "${DW5XL_RECOMP_DEPS:-$repo/external/PS2Recomp/build/_deps}")
ffmpeg=${DW5XL_FFMPEG_SOURCE:-"$HOME/.local/opt/ffmpeg-src-download/ffmpeg-7.1.tar.xz"}
image_tools=${DW5XL_APPIMAGE_TOOLS:-"$HOME/.local/opt/appimage-tools"}
git_common=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir)
jobs=${DW5XL_BUILD_JOBS:-2}
mkdir -p "$sdk_work"
sdk_work=$(realpath "$sdk_work")
podman run --rm --pull=never --network=none --cpus="$jobs" \
    -v "$repo:/src" -v "$sdk_work:/sdk" \
    -v "$deps:/deps:ro" -v "$recomp_deps:/recomp-deps:ro" -v "$ffmpeg:/ffmpeg.tar.xz:ro" \
    -v "$image_tools:/image-tools:ro" -e CMAKE_BUILD_PARALLEL_LEVEL="$jobs" \
    -v "$git_common:$git_common:ro" \
    -e TEST_PACKAGE="${test_args[*]}" "$image" bash -euo pipefail -c '
python3 /usr/local/bin/write_libcxx_manifest.py --check /approved-debs
rm -rf /sdk/cxx
mkdir -p /sdk/cxx
for package in /approved-debs/*.deb; do dpkg-deb -x "$package" /sdk/cxx; done
python3 /src/tools/release/export_linux_sdk.py --output /sdk/clang --cxx /sdk/cxx
export PATH=/sdk/clang/bin:$PATH CC=/sdk/clang/bin/clang CXX=/sdk/clang/bin/clang++
export LD_LIBRARY_PATH=/sdk/clang/lib:/sdk/ffmpeg/lib
if [ ! -f /sdk/ffmpeg/include/libavcodec/avcodec.h ]; then
    /src/tools/release/build_ffmpeg.sh linux /ffmpeg.tar.xz /sdk/ffmpeg > /sdk/ffmpeg-build.log 2>&1
fi
common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/src/tools/linux/clang.cmake
        -DDW5XL_LINUX_TOOLCHAIN_ROOT=/sdk/clang -DPS2X_ENABLE_SCCACHE=OFF -DFETCHCONTENT_FULLY_DISCONNECTED=ON)
cmake -S /src/app -B /src/app/build-linux-builder "${common[@]}" \
    -DDW5XL_BUILD_GAME=OFF -DDW5XL_BUILD_TESTS=ON -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF \
    -DPS2X_ENABLE_VULKAN_GS=ON -DPS2X_PARALLEL_GS_SOURCE_DIR=/deps/parallel-gs \
    -DPS2X_FFMPEG_ROOT=/sdk/ffmpeg -DFETCHCONTENT_SOURCE_DIR_RAYLIB=/deps/raylib-src \
    -DFETCHCONTENT_SOURCE_DIR_IMGUI=/deps/imgui-src -DFETCHCONTENT_SOURCE_DIR_RLIMGUI=/deps/rlimgui-src \
    > /sdk/configure.log 2>&1
cmake --build /src/app/build-linux-builder --target dw5xl_setup dw5xl_setup_disc_test \
    dw5xl_setup_build_test dw5xl_setup_codegen_test dw5xl_setup_pipeline_test dw5xl_setup_install_test \
    > /sdk/build.log 2>&1
ctest --test-dir /src/app/build-linux-builder -R "^setup_" --no-tests=error --output-on-failure > /sdk/setup-tests.log 2>&1
cmake -S /src/external/PS2Recomp -B /src/build-recomp-linux-builder "${common[@]}" \
    -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF \
    -DFETCHCONTENT_SOURCE_DIR_ELFIO=/recomp-deps/elfio-src -DFETCHCONTENT_SOURCE_DIR_TOML11=/recomp-deps/toml11-src \
    -DFETCHCONTENT_SOURCE_DIR_FMT=/recomp-deps/fmt-src -DFETCHCONTENT_SOURCE_DIR_LIBDWARF=/recomp-deps/libdwarf-src \
    -DFETCHCONTENT_SOURCE_DIR_RABBITIZER=/recomp-deps/rabbitizer-src > /sdk/recompiler-configure.log 2>&1
cmake --build /src/build-recomp-linux-builder --target ps2_recomp > /sdk/recompiler-build.log 2>&1
export DW5XL_PACKAGE_DEP_ROOT=/deps
python3 /src/tools/release/make_release.py linux --build /src/app/build-linux-builder \
    --recomp-build /src/build-recomp-linux-builder --linux-sdk /sdk/clang --ffmpeg-root /sdk/ffmpeg \
    --appimage-tools /image-tools ${TEST_PACKAGE:+--test} > /sdk/package.log 2>&1
tail -5 /sdk/setup-tests.log
tail -3 /sdk/package.log
'
