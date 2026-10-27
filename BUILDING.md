# Building and developing DW5XL Recomp

Players do not need this: the [releases](https://github.com/TheSlackbringer/dw5xl-recomp/releases) build the game for you (see the [README](README.md)). This page is for building from source on Linux, the developer tools, and making a release.

## Building on Linux

Run the following from the repository root. You need an x86-64 CPU supporting x86-64-v3, Clang, lld, CMake 3.21 or newer, Ninja, Git, rsync, pkg-config, Vulkan drivers, and development packages for FFmpeg, the window system and audio. FFmpeg must provide `libavcodec`, `libavformat`, `libavutil`, `libswresample` and `libswscale`. CMake may download other build dependencies during configuration.

### 1. Prepare your disc files

Extract your XL disc into `game/dw5xl/`, preserving its directory structure. For example, using `bsdtar`:

```sh
mkdir -p game/dw5xl
bsdtar -xf "/path/to/your/XL-disc.iso" -C game/dw5xl
```

`SLES_538.60`, `SYSTEM.CNF` and the overlay files must be in that directory. Optionally extract your original DW5 disc into `game/dw5/`, with `SLES_533.39` directly inside it. Keep all extracted assets; the executable alone is insufficient.

### 2. Generate the native game code

```sh
CC=clang CXX=clang++ cmake -S external/PS2Recomp -B external/PS2Recomp/build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_ANALYZER=OFF \
  -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF
cmake --build external/PS2Recomp/build --target ps2_recomp
tools/regen.sh all
```

This generates the main executable and OVL00–OVL02 translations locally. Ghidra is used for reverse engineering; the existing function maps support this generation step without creating a new Ghidra project.

### 3. Prepare the Vulkan renderer

The wrapper currently expects the pinned paraLLEl-GS revision below and its required submodules:

```sh
git clone https://github.com/Arntzen-Software/parallel-gs.git app/deps/parallel-gs
git -C app/deps/parallel-gs checkout cc6184af7e0c03da603045ca371ffa5dae9b0655
git -C app/deps/parallel-gs submodule update --init Granite
git -C app/deps/parallel-gs/Granite submodule update --init \
  third_party/volk third_party/khronos/vulkan-headers
```

Then apply this project's renderer patches (2x/4x output and anti-aliasing at native resolution):

```sh
git -C app/deps/parallel-gs apply "$PWD"/vendor/parallel-gs-patches/*.patch
```

If that checkout already exists, reuse it after verifying the revision.

### 4. Build the application

```sh
CC=clang CXX=clang++ cmake -S app -B app/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_LINKER_TYPE=LLD \
  -DPS2X_ENABLE_VULKAN_GS=ON \
  -DPS2X_PARALLEL_GS_SOURCE_DIR="$PWD/app/deps/parallel-gs" \
  -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF
cmake --build app/build --target dw5xl
```

The verbose per-function logging option is disabled here because it substantially slows gameplay. `dw5xl_setup` (built with the game) offers the release's wizard window for building from your own disc.

### 5. Run

```sh
cd game/dw5xl
PS2X_GS_BACKEND=vulkan ../../app/build/dw5xl ./SLES_538.60
```

In this developer setup, saves are stored in `game/dw5xl/mc0/` and settings in `game/dw5xl/dw5xl.ini`. Keep them when updating your build.

## Developer keys and switches

| Key | Description |
| --- | --- |
| **F1** | Runtime debugger (developer builds; off in the release unless `PS2X_DEBUG_UI=1`). |
| **F5** / **F9** | Save / load state (🚧 in development). |

Environment variables on startup: `PS2X_FPS=1` turns the F2 overlay on, `PS2X_GS_SCALE=1|2|4` sets the startup resolution scale, `PS2X_GS_SSAA=1|2|4|8|16` the anti-aliasing sample count and `PS2X_VSYNC=on|off` the vsync setting (keep scale and anti-aliasing at `1` when profiling performance).

## Development

Fixes live in handwritten app/runtime code, recompiler profiles and generation tools. Original disc files remain unchanged; generated translations are regenerated rather than edited by hand.

| Path | Contents |
| --- | --- |
| `app/` | Application, native bridges and regression tests |
| `recomp/dw5xl/` | PAL profiles, function maps and overlay configuration |
| `tools/` | Code generation, Ghidra scripts and debugging tools |
| `external/PS2Recomp/` | Vendored runtime and recompiler with maintained modifications |
| `vendor/runtime-patches/` | Runtime patch provenance |
| `licenses/` | Third-party notices |

Game data, analysis databases, generated main/overlay output, builds and local research notes are excluded from Git. Contributor and coding-agent workflows are described in `AGENTS.md` and `CLAUDE.md`.

Debugging tools:
- [Compare saved frames exactly](tools/debug/compare_frames.md): changed-pixel counts, locations and an optional difference image.
- [Validate function maps and generated output](tools/debug/validate_function_map.md): boundary, path and emission evidence for map/export and missing-instruction investigations, without changing maps or running the game.

## Making a release

Releases contain only our code and freely licensed tools — never the compiled game, disc files, an ISO or translated code (PS2Recomp is GPL-3.0; see `AGENTS.md`). On a clean commit, after the full test suite:

```sh
cmake --build app/build-release-win --target dw5xl_setup      # the launcher, cross-compiled with llvm-mingw
cmake --build build-recomp-win --target ps2_recomp           # the recompiler for Windows
tools/release/make_release.py windows --build app/build-release-win --recomp-build build-recomp-win
```

The script refuses a dirty tree, packs the launcher, sources, compiler (llvm-mingw for Windows), CMake, Ninja, the minimal FFmpeg (`tools/release/build_ffmpeg.sh`) and the licenses, scans the package for anything from the game, and writes the zip and its SHA-256 to `dist/`. The Linux AppImage is built in a pinned container; see [tools/release/LINUX.md](tools/release/LINUX.md). Publishing is done by the maintainer.
