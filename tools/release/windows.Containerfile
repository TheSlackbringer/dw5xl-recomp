# Windows release builder (issue 027 M11): the environment the windows-build job used to install on every run
# (archlinux, now pinned by digest, with the same packages), and every pinned input of the Windows build, fetched
# once here and checked (size, SHA-256; windows-tools.json): llvm-mingw for cross-compiling (unpacked in /opt), the
# FFmpeg Windows build, the llvm-mingw / CMake / Ninja that the release package bundles for the player's PC, and the
# FFmpeg source for the package's minimal FFmpeg. CI runs the windows-build job in this image, pinned by digest in
# .github/workflows/build.yml and tools/release/windows-builder-image; a release build downloads nothing.
# Built rarely by .github/workflows/builder-image.yml and published to ghcr.io. Building it locally:
#   podman build -t dw5xl-release-windows -f tools/release/windows.Containerfile tools/release
FROM docker.io/library/archlinux@sha256:e82623936fa1a69700b48e98b058df6eaab18b737c27473a7234a40fdecb9413

LABEL org.opencontainers.image.source=https://github.com/TheSlackbringer/dw5xl-recomp
LABEL org.opencontainers.image.description="DW5XL Windows release builder: archlinux, llvm-mingw, pinned build inputs"
LABEL org.opencontainers.image.licenses="Arch Linux packages under their own licenses; llvm-mingw: Apache-2.0 WITH LLVM-exception; FFmpeg: LGPL-2.1+; CMake: BSD-3-Clause; Ninja: Apache-2.0"

# gcc: the host compiler for FFmpeg's configure
RUN pacman -Syu --noconfirm --needed git cmake ninja python curl unzip xz make diffutils gcc \
 && pacman -Scc --noconfirm

COPY fetch_inputs.py windows-tools.json /usr/local/share/dw5xl/
RUN python3 /usr/local/share/dw5xl/fetch_inputs.py /usr/local/share/dw5xl/windows-tools.json /inputs \
 && tar -xf /inputs/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64.tar.xz -C /opt
