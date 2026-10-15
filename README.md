# DW5XL Recomp

A native PC port of **Dynasty Warriors 5: Xtreme Legends** for the PlayStation 2, built with PS2Recomp. The game's logic is statically translated into C++; the runtime provides the PS2 hardware services it still needs. Graphics use **Vulkan through paraLLEl-GS**.

**In development.** Linux is the current development and testing platform. Windows is planned. There is no packaged player release yet, and full playability has not been established.

> [!IMPORTANT]
> You need your own legal copy of **Dynasty Warriors 5: Xtreme Legends** (English PAL, `SLES_538.60`). This repository does not provide disc images, game executables, IRX modules or game assets. The main executable and overlay C++ translations are generated locally from your disc. It does not provide or download either game.

## Current progress

| Area | Status |
| --- | --- |
| Boot and menus | Boot, title screen, Options and mode selection work. |
| Input | Keyboard and gamepad input work. |
| Audio and movies | Intro and selected event movies play with audio; menu music and sound effects work. |
| Saves | Saving and loading have been confirmed in manual testing. |
| Legend Mode | Scenario selection, briefing and preparation render; battles run with movement and attacks in current tests. |
| Other modes | Edit Mode works. Destiny, Challenge and Xtreme reach battle, but need further gameplay testing after the shared battle fixes. |
| Load Original | Local original-disc validation and the original prompts are implemented; broader mode testing remains. |
| Opening | Reaches real-time 3D scenes, with outstanding visual defects. |

Rendering defects and combat performance drops remain under investigation. These results describe tested scenes, not a complete playthrough or a guarantee that every mode works.

## Supported discs

| Game | Version | Executable | Required? |
| --- | --- | --- | --- |
| Dynasty Warriors 5: Xtreme Legends | English PAL | `SLES_538.60` | Yes |
| Dynasty Warriors 5 | English PAL | `SLES_533.39` | Optional, for Load Original |

Other regions and revisions are unsupported. You can use XL alone. To unlock the original game's Musou and Free modes, supply the supported original DW5 files and select **Load Original** in the game. Having both games present does not unlock those modes at startup; verification fails when the original DW5 copy is missing or invalid.

The planned player release will provide a prebuilt application and native disc importer. That installer is not implemented; the workflow below is for developers.

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
git clone https://github.com/Themaister/parallel-gs.git app/deps/parallel-gs
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

The verbose per-function logging option is disabled here because it substantially slows gameplay. This is a development build; the complete setup has not yet been validated as a clean-install release workflow.

## Running and controls

```sh
cd game/dw5xl
PS2X_GS_BACKEND=vulkan ../../app/build/dw5xl ./SLES_538.60
```

Saves are stored in `game/dw5xl/mc0/`. Keep that directory when updating your build.

| Keyboard | PS2 input |
| --- | --- |
| Enter / Tab | Start / Select |
| X or Space / C or Esc | Cross / Circle |
| Z / V | Square / Triangle |
| Arrow keys or WASD | D-pad |
| Q / E | L1 / R1 |
| Left Shift / Right Shift | L2 / R2 |

Gamepads are also supported. Press **F3** to toggle the FPS counter, or start with `PS2X_FPS=1`. Optional supersampling is controlled by `PS2X_GS_SCALE=1|2|4|8|16`; start at `1` when testing performance.

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

Current priorities are rendering correctness, combat performance, broader gameplay testing, and preparing the native importer and Linux/Windows release packaging.

## License and attribution

The port's own source and tools are licensed under GPL-3.0; see [LICENSE](LICENSE). This license grants no rights to the original games or their assets.

The vendored [PS2Recomp](https://github.com/ran-j/PS2Recomp) runtime and recompiler are modified versions of the upstream project by ran-j and contributors. Revision and maintenance details are in [DW5XL_VENDORED.md](external/PS2Recomp/DW5XL_VENDORED.md). Third-party code retains its applicable licenses; see [THIRD_PARTY.md](THIRD_PARTY.md) and the accompanying notices.

Dynasty Warriors is a trademark of KOEI TECMO. This project is not affiliated with or endorsed by KOEI TECMO.
