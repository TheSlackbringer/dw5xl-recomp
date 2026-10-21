# Linux release build environment (issue 011 M8): Valve's Steam Runtime 3 "sniper" SDK (Debian 11, glibc 2.31),
# so the game runs on the Steam Deck and practically every current distribution, plus clang 23 from apt.llvm.org
# (the same compiler major version as development). Built and used on the owner's PC only:
#   podman build -t dw5xl-release-linux -f tools/release/linux.Containerfile tools/release
FROM registry.gitlab.steamos.cloud/steamrt/sniper/sdk@sha256:9ae8083ee9ae6298361a62f289e62f0a2b0779d23fc8c276304157f0c06010c5

RUN curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
 && echo "deb http://apt.llvm.org/bullseye/ llvm-toolchain-bullseye-23 main" > /etc/apt/sources.list.d/llvm.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends clang-23 lld-23 llvm-23 \
 && rm -rf /var/lib/apt/lists/*

ENV CC=clang-23 CXX=clang++-23
