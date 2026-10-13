# dw5xl-recomp

A native PC port of **Dynasty Warriors 5: Xtreme Legends** (PS2), built by statically recompiling the game's own code into C++ with PS2Recomp. There is no emulator at runtime: the game logic runs as native code, and the parts that touched PS2 hardware are replaced with native implementations where possible.

> [!IMPORTANT]
> This repository contains no game files: no disc images, executables, IRX modules, assets, or C++ generated from the game. You need your own copy of **Dynasty Warriors 5: Xtreme Legends** (English PAL, `SLES_538.60`). The planned **Load Original** support will also need your own copy of **Dynasty Warriors 5** (English PAL, `SLES_533.39`). This project does not provide or download either game.

## Status

Early development. Linux only for now; Windows is planned.

Working: boot, intro movies with audio (skippable), title screen, main menu and Options, keyboard and gamepad input, menu music and sound effects, and animated still images, all at the PAL 50 Hz rate. Memory card saving and loading are still being verified.

OVL00 now compiles as a separate native module and activates before its constructors. Opening runs its scene loop, but graphics are still corrupted; further engine work is required.

Not working yet: 3D gameplay (the real-time engine), the **Load Original** feature, and a player installer. There is no release build; you currently build from source with your own disc.

## Supported game versions

| Game | Region | Executable | Use |
| --- | --- | --- | --- |
| Dynasty Warriors 5: Xtreme Legends | English PAL | `SLES_538.60` | Required |
| Dynasty Warriors 5 | English PAL | `SLES_533.39` | Future Load Original support |

Other regions and revisions are not supported. The recompiler profile is made for `SLES_538.60`, and the port's fixes only apply when the disc serial and the code they replace match.

## Building

You need Linux with clang, lld, CMake and Ninja, plus the usual development packages for X11/Wayland, OpenGL and audio. CMake downloads the remaining library dependencies the first time you configure.

1. Extract your own XL disc image into `game/dw5xl/`, keeping its folder structure. `SLES_538.60` and `SYSTEM.CNF` must be directly inside that folder. With `bsdtar`:

   ```sh
   mkdir -p game/dw5xl
   bsdtar -xf "/path/to/your/Dynasty Warriors 5 Xtreme Legends.iso" -C game/dw5xl
   ```

   If you have the original DW5 disc, extract it into `game/dw5/` the same way. It is not used yet.

2. Build the recompiler and generate the game code from your executable:

   ```sh
   CC=clang CXX=clang++ cmake -S external/PS2Recomp -B external/PS2Recomp/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
   cmake --build external/PS2Recomp/build --target ps2_recomp
   tools/regen.sh
   ```

3. Build the game:

   ```sh
   CC=clang CXX=clang++ cmake -S app -B app/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_LINKER_TYPE=LLD
   cmake --build app/build --target dw5xl
   ```

## Running

```sh
cd game/dw5xl
PS2X_GS_BACKEND=cpu ../../app/build/dw5xl ./SLES_538.60
```

The CPU renderer is the tested one for now. Saves go to `game/dw5xl/mc0/`.

Keyboard: Enter = Start, Tab = Select, X or Space = Cross, C or Esc = Circle, Z = Square, V = Triangle, arrow keys or WASD = D-pad, Q / E = L1 / R1, Left / Right Shift = L2 / R2. Gamepads work as well.

## How it works

`tools/regen.sh` runs the recompiler over your `SLES_538.60` using the profile in `recomp/dw5xl/` (function map, SDK stubs, hardware register map). It writes about 43,000 C++ files to `recomp/dw5xl/output/`, one per game function, which stay on your machine and are never committed.

The executable links that generated code with the runtime in `external/PS2Recomp/` and our own code in `app/src/`. At startup every game function is registered under its original PS2 address, and the game calls functions through that table. For a known disc, our code replaces specific entries with native functions:

- **Controller input:** the game's controller library calls go to a native input bridge (`dw5xl_pad2.cpp`).
- **Still images:** the game's image decoder is replaced by our own MPEG-2 intra decoder (`dw5xl_images.cpp`, `ipu_idec.cpp`).

Movie playback and timing fixes live in the runtime.

A replacement can take over a function completely, or call the original and adjust its result. That is how planned improvements such as camera controls will be added.

Your disc files are only read. Fixes live in handwritten C++, the recompiler profile and the runtime, never in the game files or the generated code.

## Repository layout

| Path | Contents |
| --- | --- |
| `app/` | The executable: build files and our native code (`src/`) |
| `recomp/dw5xl/` | Recompiler profile and function map for `SLES_538.60` |
| `tools/` | Code generation script, Ghidra scripts, decoder tests, debug helpers |
| `external/PS2Recomp/` | Third-party: modified copy of PS2Recomp (runtime and recompiler) |
| `vendor/runtime-patches/` | Third-party: the runtime patch series applied to that copy, kept for provenance |
| `licenses/` | Third-party license notices |
| `game/` | Your extracted disc files (ignored by Git) |

`AGENTS.md` and `CLAUDE.md` describe the development workflow for contributors and coding agents.

## Roadmap

1. 3D engine: fix the rendering corruption in the real-time Opening, then add recompilation and switching for the game's overlay modules.
2. Verify saving and loading, and sound in all menus.
3. **Load Original** with the original DW5 disc.
4. Player release: a prebuilt application with a first-run importer that checks your disc, for Linux and Windows.
5. Improvements such as camera controls.

## License and attribution

The port's own source and tools are licensed under GPL-3.0; see [LICENSE](LICENSE). This license grants no rights to the original games or their assets.

Everything under `external/` is third-party code that keeps its own license. `external/PS2Recomp/` is a modified copy of [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors (GPL-3.0), not an unmodified upstream release. Its base revision and changes are recorded in `external/PS2Recomp/DW5XL_VENDORED.md`. [THIRD_PARTY.md](THIRD_PARTY.md) lists all third-party sources and notices.

Dynasty Warriors is a trademark of KOEI TECMO. This project is not affiliated with or endorsed by KOEI TECMO.
