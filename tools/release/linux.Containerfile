# Linux release build environment (issue 011 M8, issue 041): Valve's Steam Runtime 3 "sniper" SDK (Debian 11,
# glibc 2.31), so the game runs on the Steam Deck and practically every current distribution, plus clang 23 from
# apt.llvm.org (the same compiler major version as development) and, in /approved-debs, the libc++ packages of the
# same apt.llvm.org snapshot that the package ships (with manifest.json: name, version, size, SHA-256).
# Built rarely by .github/workflows/builder-image.yml and published to ghcr.io; CI and local release builds use
# the image pinned by digest in tools/release/linux-builder-image (see LINUX.md). Building it locally:
#   podman build -t dw5xl-release-linux -f tools/release/linux.Containerfile tools/release
FROM registry.gitlab.steamos.cloud/steamrt/sniper/sdk@sha256:9ae8083ee9ae6298361a62f289e62f0a2b0779d23fc8c276304157f0c06010c5

LABEL org.opencontainers.image.source=https://github.com/TheSlackbringer/dw5xl-recomp
LABEL org.opencontainers.image.description="DW5XL Linux release builder: Steam Runtime 3 sniper SDK, clang 23, approved libc++ packages"
LABEL org.opencontainers.image.licenses="Steam Runtime: Debian packages under their own licenses; LLVM: Apache-2.0 WITH LLVM-exception"

COPY write_libcxx_manifest.py /usr/local/bin/write_libcxx_manifest.py
RUN curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
 && echo "deb http://apt.llvm.org/bullseye/ llvm-toolchain-bullseye-23 main" > /etc/apt/sources.list.d/llvm.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends clang-23 lld-23 llvm-23 \
 && version=$(dpkg-query -W -f='${Version}' clang-23) \
 && mkdir -p /approved-debs && cd /approved-debs \
 && apt-get download libc++-23-dev="$version" libc++1="$version" libc++abi-23-dev="$version" libc++abi1="$version" \
        libunwind-23-dev="$version" llvm-libunwind1="$version" \
 && python3 /usr/local/bin/write_libcxx_manifest.py /approved-debs \
 && rm -rf /var/lib/apt/lists/*

ENV CC=clang-23 CXX=clang++-23
