<h1 align="center">DW5XL Recomp</h1>

<p align="center"><b>Dynasty Warriors 5: Xtreme Legends (PS2) as a native PC game, built on your PC from your own disc.</b></p>

<p align="center">
  <a href="https://github.com/TheSlackbringer/dw5xl-recomp/actions/workflows/build.yml"><img alt="build" src="https://img.shields.io/github/actions/workflow/status/TheSlackbringer/dw5xl-recomp/build.yml?branch=main&label=build&logo=github"></a>
  <img alt="platform: Windows | Linux" src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux-0a7bbb">
  <img alt="graphics: Vulkan" src="https://img.shields.io/badge/graphics-Vulkan-ac162c">
  <a href="LICENSE"><img alt="license: GPL-3.0" src="https://img.shields.io/badge/license-GPL--3.0-6a9a1f"></a>
  <img alt="status: playable (beta)" src="https://img.shields.io/badge/status-playable%20(beta)-e6a200">
  <img alt="disc: PAL SLES-53860" src="https://img.shields.io/badge/disc-PAL%20SLES--53860-555">
</p>

A native PC port of **Dynasty Warriors 5: Xtreme Legends** for the PlayStation 2, built with PS2Recomp: the game's code is translated into C++ and runs natively, with graphics on **Vulkan through paraLLEl-GS**. Windows and Linux releases build the game on your PC from your own disc.

> [!IMPORTANT]
> You need your own legal copy of **Dynasty Warriors 5: Xtreme Legends** (English PAL, `SLES_538.60`). Nothing from the game is in this repository or in its releases: no disc images, game executables, IRX modules, game assets or translated game code.

## Status

**Beta: all modes work** — boot, menus, Legend/Musou/Free modes, the opening and event movies, music, voices and sound effects, saving and loading, and Load Original. We are still testing for bugs and performance, so expect rough edges: report them in [Issues](https://github.com/TheSlackbringer/dw5xl-recomp/issues).

**Known issues:** in busy battles the frame rate on Windows is lower than on Linux and the picture can jump; higher resolutions cost more on Windows.

## What you need

- **Dynasty Warriors 5: Xtreme Legends**, English PAL (`SLES_538.60`), as an `.iso`. Required.
- Optional: **Dynasty Warriors 5**, English PAL (`SLES_533.39`), to unlock the original game's Musou and Free modes through **Load Original** in the game.
- A 64-bit CPU with AVX2 (Intel since 2013, AMD Ryzen) and a graphics card with a current Vulkan driver; about 10 GB of free space.
- Windows 10 (1903 or newer) / 11, or Linux with glibc 2.31 or newer.

Other regions and revisions are not supported (every file is checked against known checksums).

## Playing

Download the latest release from the [Releases](https://github.com/TheSlackbringer/dw5xl-recomp/releases) page. It contains our program, the source code, a compiler and the tools; nothing else needs to be installed and nothing is downloaded while it builds.

- **Windows:** unzip `DW5XL-<version>-windows-x64.zip` close to the drive root, e.g. `C:\Games\` (deep paths trip up Windows), and start **`DW5XL.exe`**. It is not signed: if Windows says *"Windows protected your PC"*, choose **More info → Run anyway**.
- **Linux:** make `DW5XL-RECOMP-<version>-x86_64.AppImage` executable and open it. AppImage update tools such as AppImageUpdate can update it to the newest release.

**First start:** choose your XL disc image (and optionally the DW5 one) and press **Start**. The disc is checked, and the game is translated and compiled for your PC — about 10 minutes, once. Every later start opens the game directly; a new release rebuilds once without asking for the disc again.

**Load Original:** when the game asks for the Dynasty Warriors 5 disc, a window opens over the game: choose that disc image and press **Install**. You can also add or repair discs any time with **F10**.

Saves, settings and logs are kept in `%LOCALAPPDATA%\DW5XL` (Windows) or `~/.local/share/dw5xl` (Linux), so updating the release keeps them. If something goes wrong, please attach the files from its `logs` folder to an issue.

## Controls

| Keyboard | PS2 input |
| --- | --- |
| Arrow keys or WASD | D-pad |
| X or Space / C | Cross / Circle |
| Z / V | Square / Triangle |
| Q / E | L1 / R1 |
| Left Shift / Right Shift | L2 / R2 |
| Enter / Tab | Start / Select |
| Esc | PC SETTINGS |

Gamepads work too. Holding the gamepad's **Back** (Select) button for ¾ of a second opens PC SETTINGS; a short press is still Select in the game.

### PC SETTINGS

**PC SETTINGS** is in the game's Options menu (where the PS2's SCREEN ADJUST was), in the game's own style. You can also open it at any time with **Esc** or by holding **Back**: the game pauses until you close it with Esc, Triangle or Back.

| Setting | Values |
| --- | --- |
| VSYNC | On / Off |
| RESOLUTION | 640×512 (1×), 1280×1024 (2×), 2560×2048 (4×) |
| ANTI-ALIASING | Off, 2×, 4×, 8×, 16× |
| DISPLAY | Window / Fullscreen |
| SHOW FPS | On / Off |
| DEFAULT | Restores the default settings |

Up/Down picks a setting, Left/Right changes it, and Cross on DEFAULT resets them all. Changes apply right away and are remembered.

### Function keys

| Key | Description |
| --- | --- |
| **F1** | Runtime debugger (developer builds only). |
| **F2** | Show or hide the frame rate (same as SHOW FPS). |
| **F3** | Cycle anti-aliasing (1× → 2× → 4× → 8× → 16×). |
| **F4** | Cycle the resolution (1× → 2× → 4×). |
| **F5** | Save state (🚧 in development). |
| **F8** | Show the build ID and game tick (for bug reports). |
| **F9** | Load state (🚧 in development). |
| **F10** | Discs window: see the installed discs, add or repair the DW5 disc, check the files. |

## Building from source

See [BUILDING.md](BUILDING.md) for building on Linux from source, the developer tools and making a release.

## License and attribution

The port's own source and tools are licensed under GPL-3.0; see [LICENSE](LICENSE). This license grants no rights to the original games or their assets.

The vendored [PS2Recomp](https://github.com/ran-j/PS2Recomp) runtime and recompiler are modified versions of the upstream project by ran-j and contributors. Revision and maintenance details are in [DW5XL_VENDORED.md](external/PS2Recomp/DW5XL_VENDORED.md). Third-party code retains its applicable licenses; see [THIRD_PARTY.md](THIRD_PARTY.md) and the accompanying notices.

Dynasty Warriors is a trademark of KOEI TECMO. This project is not affiliated with or endorsed by KOEI TECMO.
