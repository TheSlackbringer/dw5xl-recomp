<h1 align="center">DW5XL Recomp</h1>

<p align="center"><b>Dynasty Warriors 5: Xtreme Legends (PS2) as a native PC game, built on your PC from your own disc.</b></p>

<p align="center">
  <a href="https://github.com/TheSlackbringer/dw5xl-recomp/actions/workflows/build.yml"><img alt="build" src="https://img.shields.io/github/actions/workflow/status/TheSlackbringer/dw5xl-recomp/build.yml?branch=main&label=build&logo=github"></a>
  <img alt="platform: Windows | Linux" src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux-0a7bbb">
  <img alt="graphics: Vulkan" src="https://img.shields.io/badge/graphics-Vulkan-ac162c">
  <a href="LICENSE"><img alt="license: GPL-3.0" src="https://img.shields.io/badge/license-GPL--3.0-6a9a1f"></a>
  <img alt="status: in development" src="https://img.shields.io/badge/status-in%20development-e6a200">
  <img alt="disc: PAL SLES-53860" src="https://img.shields.io/badge/disc-PAL%20SLES--53860-555">
</p>

A native PC port of **Dynasty Warriors 5: Xtreme Legends** for the PlayStation 2, built with PS2Recomp. The game's logic is statically translated into C++; the runtime provides the PS2 hardware services it still needs. Graphics use **Vulkan through paraLLEl-GS**.

**In development.** Windows builds come as a self-building release (below); on Linux the game is built from source for now, a Linux release is planned. Full playability has not been established.

> [!IMPORTANT]
> You need your own legal copy of **Dynasty Warriors 5: Xtreme Legends** (English PAL, `SLES_538.60`). Nothing from the game is in this repository or in its releases: no disc images, game executables, IRX modules, game assets or translated game code. The game is built on your own PC from your own disc.

## Current progress

| Area | State | Details |
| --- | :---: | --- |
| Boot and menus | ✅ | Boot, title screen, Options and mode selection work. |
| Input | ✅ | Keyboard and gamepad input work. |
| Audio and movies | ✅ | Intro and event movies play with audio; menu music, battle BGM, voice lines, and sound effects play throughout menus and battles. |
| Saves | ✅ | Saving and loading work. |
| Windows | 🚧 | The release builds and runs the game on Windows 10/11; frame rate on Windows is being worked on. |
| Legend Mode | ✅ | Scenario selection, briefing, and preparation render; battles run at 50 FPS with full movement, combat, Musou, and crowd rendering at native resolution. |
| Other modes | 🚧 | Edit Mode works. Destiny, Challenge, and Xtreme Modes reach battle, with further mode testing ongoing. |
| Load Original | ✅ | Original disc verification works; Musou and Free Modes unlock when the original DW5 files are present. |
| Opening | ✅ | Real-time 3D intro plays with 3D character models, silhouettes, background scenes, and synchronized audio. |

✅ works in the tested scenes · 🚧 in development

Extended session stability and higher-resolution scaling performance remain in active development. These results describe tested scenes, not a complete playthrough or a guarantee that every mode works.

## Supported discs

| Game | Version | Executable | Required? |
| --- | --- | --- | --- |
| Dynasty Warriors 5: Xtreme Legends | English PAL | `SLES_538.60` | Yes |
| Dynasty Warriors 5 | English PAL | `SLES_533.39` | Optional, for Load Original |

Other regions and revisions are unsupported (the program checks every file against the known checksums). You can use XL alone. To unlock the original game's Musou and Free modes, supply the supported original DW5 disc and select **Load Original** in the game. Having both games present does not unlock those modes at startup; verification fails when the original DW5 copy is missing or invalid.

## Playing on Windows

The release is a **builder**, not a game: it contains our program, the source code, a compiler and the tools, and builds the game on your PC from your own disc. You install nothing else — no Visual Studio, no Python, no Git — and nothing is downloaded while it builds.

**You need:** Windows 10 (version 1903 or newer) or 11, 64-bit; a CPU with AVX2 (Intel since 2013, AMD Ryzen); a graphics card with a current Vulkan driver; about 10 GB of free space; your disc image(s) as `.iso`.

1. Download `DW5XL-<version>-windows-x64.zip` from the [Releases](../../releases) page and **unzip it close to the drive root**, e.g. `C:\Games\` (the compiler inside has deep folders, and Windows trips over paths longer than 260 characters).
2. Start **`DW5XL.exe`**. The program is not signed, so Windows may say *"Windows protected your PC"*: choose **More info → Run anyway**.
3. **First start:** choose your DW5XL disc image (Browse, or drag it onto the window), optionally the DW5 image for Load Original, and press **Start**. The disc is checked and copied, the game's code is translated and compiled for your PC — about **10 minutes** once (longer on older PCs; Windows Defender scanning the new files adds to it). Then the game starts.
4. **Every later start:** `DW5XL.exe` starts the game at once. A newer release rebuilds once, without asking for the disc again.
5. **Load Original:** when the game asks for the Dynasty Warriors 5 disc, an *Insert disc* window opens over the game: choose that disc image and press **Install** — the game continues by itself.

Your game files, the built game, saves, settings and logs are in `%LOCALAPPDATA%\DW5XL`. Replacing the release folder with a newer version keeps them. If something goes wrong, the logs are in `%LOCALAPPDATA%\DW5XL\logs` (`setup.log`, `build.log`, `game.log`); please attach them to an issue.

**Known issues:** the frame rate on Windows is lower than on Linux in busy battles and the picture can jump (issue 025, being measured); higher output resolutions (F4) cost more on Windows.

## Playing on Linux

A Linux release (an AppImage with the same self-building wizard) is planned. Until then, build from source as described below; `dw5xl_setup` (built with the game) offers the same wizard window for building from your own disc.

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

The verbose per-function logging option is disabled here because it substantially slows gameplay. This is a development build; the complete setup has not yet been validated as a clean-install release workflow.

## Running and controls

```sh
cd game/dw5xl
PS2X_GS_BACKEND=vulkan ../../app/build/dw5xl ./SLES_538.60
```

In this developer setup, saves are stored in `game/dw5xl/mc0/`. Keep that directory when updating your build. (The Windows release keeps everything in `%LOCALAPPDATA%\DW5XL` instead.)

### Controls

| Keyboard | PS2 input |
| --- | --- |
| Enter / Tab | Start / Select |
| X or Space / C or Esc | Cross / Circle |
| Z / V | Square / Triangle |
| Arrow keys or WASD | D-pad |
| Q / E | L1 / R1 |
| Left Shift / Right Shift | L2 / R2 |

Gamepads are also supported.

### Function keys and display

| Key | Description |
| --- | --- |
| **F2** | Toggle build ID and game tick HUD overlay (useful for bug reports and log alignment). |
| **F3** | Toggle real game FPS, VSync rate, resolution scale, and AA sample HUD counter. |
| **F4** | Cycle internal resolution scale live ($1\times \to 2\times \to 4\times$) and persist to `dw5xl.ini`. |
| **F5** | Cycle anti-aliasing sample multiplier ($1\times \to 2\times \to 4\times \to 8\times \to 16\times$) and persist to `dw5xl.ini`. |
| **F9** | Discs window (release build): which discs are installed, add or repair the DW5 disc, check the files. |
| **F1** | Runtime debugger (developer builds; off in the release unless `PS2X_DEBUG_UI=1`). |

You can also pass environment variables on startup: `PS2X_FPS=1` enables the F3 overlay by default, `PS2X_GS_SCALE=1|2|4` sets the startup resolution scale, and `PS2X_GS_SSAA=1|2|4|8|16` sets the anti-aliasing sample count (defaults can be kept at `1` when profiling performance).

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

Current priorities are Windows performance, long-session stability, broader gameplay testing and the Linux release.

### Making a release

Releases contain only our code and freely licensed tools — never the compiled game, disc files, an ISO or translated code (PS2Recomp is GPL-3.0; see `AGENTS.md`). On a clean commit, after the full test suite:

```sh
cmake --build app/build-release-win --target dw5xl_setup      # the launcher, cross-compiled with llvm-mingw
cmake --build build-recomp-win --target ps2_recomp           # the recompiler for Windows
tools/release/make_release.py windows --build app/build-release-win --recomp-build build-recomp-win
```

The script refuses a dirty tree, packs the launcher, sources, compiler (llvm-mingw for Windows), CMake, Ninja, the minimal FFmpeg (`tools/release/build_ffmpeg.sh`) and the licenses, scans the package for anything from the game, and writes the zip and its SHA-256 to `dist/`. Publishing is done by the maintainer.

## License and attribution

The port's own source and tools are licensed under GPL-3.0; see [LICENSE](LICENSE). This license grants no rights to the original games or their assets.

The vendored [PS2Recomp](https://github.com/ran-j/PS2Recomp) runtime and recompiler are modified versions of the upstream project by ran-j and contributors. Revision and maintenance details are in [DW5XL_VENDORED.md](external/PS2Recomp/DW5XL_VENDORED.md). Third-party code retains its applicable licenses; see [THIRD_PARTY.md](THIRD_PARTY.md) and the accompanying notices.

Dynasty Warriors is a trademark of KOEI TECMO. This project is not affiliated with or endorsed by KOEI TECMO.
