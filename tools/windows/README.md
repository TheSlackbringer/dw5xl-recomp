# Building on Windows

Status: prepared and checked on Linux only. The Windows build has not yet been configured, compiled, linked or run on Windows. Linux remains the development platform. When something fails, keep the complete `build.ps1` output.

The Windows build uses the same compiler family and flags as Linux: clang (as `clang-cl`), Ninja and `lld-link`, Release, x86-64-v3, the Vulkan paraLLEl-GS renderer, and aggressive logs off. MSVC `cl.exe` is an untested fallback and does not produce bit-identical floating-point results.

## Prerequisites

- Windows 10 1809 or later, or Windows 11, x64. The CPU must support AVX2/FMA/BMI2 (x86-64-v3, the same requirement as Linux). Install a current GPU driver with Vulkan support.
- Visual Studio 2022 or Build Tools for Visual Studio 2022, with:
  - **Desktop development with C++** (MSVC v143 and a Windows 10/11 SDK). This is required even with clang-cl, because clang-cl uses the MSVC headers, libraries and the Windows SDK.
  - **C++ Clang Compiler for Windows** (`clang-cl`, `lld-link`, `llvm-lib`). Alternatively, install LLVM for Windows to `C:\Program Files\LLVM`. Linux currently builds with clang 23. If the Clang bundled with Visual Studio rejects the code, install a current LLVM release; the script prefers `clang-cl` on `PATH`.
  - **C++ CMake tools for Windows** (CMake and Ninja). Alternatively, install CMake 3.29 or newer and Ninja yourself.
- The Vulkan SDK is not required. paraLLEl-GS loads `vulkan-1.dll` from the GPU driver at run time; the headers come from the pinned Granite checkout.
- Python, bash and Git are not needed to build. Git is used only if `app\deps` lacks the raylib, imgui or rlImGui sources and CMake must clone them.
- Internet access is needed once per build directory for FFmpeg (see below), unless you supply the archive yourself.

## 1. Bring over the inputs that are not in Git

The repository contains no game files and no generated C++. The supported path is to **generate on Linux and copy the result to Windows**:

- The generated C++ is platform-independent: it contains no host paths, and sampled files and every project-added runtime file pass a `_WIN32`/LLP64 syntax check.
- `tools/regen.sh` (bash, awk, sha256sum, rsync) and the `ps2_recomp` build are set up and verified only on Linux.
- The planned release model already builds generated code on the maintainer's machine. Regenerating on Windows would require a PowerShell version of `regen.sh` and a Windows build of the recompiler. That is future work.

On Linux, after `tools/regen.sh all`:

```sh
tools/windows/pack-inputs.sh                 # dw5xl-windows-inputs.tar.gz, about 100 MB
tools/windows/pack-inputs.sh --full          # also the source tree, including uncommitted changes
```

The default archive contains:

- the dependency sources: `app/deps/{imgui-src,raylib-src,rlimgui-src,parallel-gs}`, including the pinned paraLLEl-GS and Granite;
- the generated code: `recomp/dw5xl/output` and `recomp/dw5xl/overlays/ovl00..02/output`;
- the stamp file `app/deps/WINDOWS_INPUTS.txt`, which records the commit, local changes and paraLLEl-GS revision.

With `--full`, it also contains every tracked file and every untracked file that is not ignored. Use `--full` when the Windows folder should match uncommitted Linux work or has no Git checkout. `game/`, `discs/`, `notes/`, Ghidra projects and build directories are never packed.

**The archive contains code generated from your disc. Keep it private.** It is a local transfer file, not something to publish.

On Windows, use a short ASCII path such as `C:\src\dw5xl`:

```powershell
git clone <your repository URL> C:\src\dw5xl     # same commit as the Linux tree, or extract a --full archive instead
cd C:\src\dw5xl
tar -xzf D:\transfer\dw5xl-windows-inputs.tar.gz   # tar.exe ships with Windows 10 1803+
```

Disc files: copy your extracted `game\dw5xl\` folder from Linux and keep its structure. `SLES_538.60` and `SYSTEM.CNF` must be directly inside it. Optionally copy `game\dw5\` (`SLES_533.39`) for Load Original. To extract your own disc or ISO on Windows instead, mount the ISO in Explorer by double-clicking it, or insert the disc, and copy everything with `robocopy E:\ game\dw5xl /E /A-:R`. `/A-:R` clears the read-only attribute that disc files carry. `tar -xf "D:\path\to\XL.iso" -C game\dw5xl` may also work through the bsdtar included with Windows, but that has not been verified. The runtime matches file names case-insensitively, so Windows' case-insensitive file system needs nothing extra.

## 2. Build

From PowerShell in the checkout root:

```powershell
powershell -ExecutionPolicy Bypass -File tools\windows\build.ps1            # dw5xl.exe only
powershell -ExecutionPolicy Bypass -File tools\windows\build.ps1 -Tests     # everything + ctest
```

The script imports the Visual Studio x64 environment, finds `clang-cl`, `lld-link` and `llvm-lib`, and checks the inputs. It then runs commands equivalent to these:

```powershell
cmake -S app -B app\build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=<clang-cl.exe> -DCMAKE_CXX_COMPILER=<clang-cl.exe> `
  -DCMAKE_LINKER_TYPE=LLD -DCMAKE_AR=<llvm-lib.exe> `
  "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`$<`$<CONFIG:Debug>:Debug>DLL" `
  -DPS2X_ENABLE_VULKAN_GS=ON -DPS2X_PARALLEL_GS_SOURCE_DIR=C:/src/dw5xl/app/deps/parallel-gs `
  -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DDW5XL_BUILD_TESTS=OFF -DFETCHCONTENT_FULLY_DISCONNECTED=ON `
  -DFETCHCONTENT_SOURCE_DIR_IMGUI=C:/src/dw5xl/app/deps/imgui-src `
  -DFETCHCONTENT_SOURCE_DIR_RAYLIB=C:/src/dw5xl/app/deps/raylib-src `
  -DFETCHCONTENT_SOURCE_DIR_RLIMGUI=C:/src/dw5xl/app/deps/rlimgui-src
cmake --build app\build-windows --target dw5xl
```

Options:

- `-BuildDir <dir>` and `-Config Release|RelWithDebInfo|Debug`
- `-Compiler clang-cl|msvc` and `-DepsDir <dir>`
- `-Jobs N`: lower it if memory runs out. Ninja's default uses every core, and each compiler process handles a unity batch of 64 generated files.
- `-ConfigureOnly`
- `-FfmpegArchive <zip>`

**FFmpeg:** Linux uses the system FFmpeg. On Windows, the upstream runtime downloads `ffmpeg-n7.1-241205-lgpl-amd64-shared.zip` from the GitHub releases of `System233/ffmpeg-msvc-prebuilt` during the first build in each build directory. To build offline, download that file yourself and pass `-FfmpegArchive <path>`, which sets `PS2X_FFMPEG_PREBUILT_URL`.

A full build compiles about 950 unity batches. Excluding the checkout and build directory from real-time antivirus scanning speeds it up noticeably.

## 3. Artifacts

`app\build-windows\` contains:

- `dw5xl.exe`, a console application whose game window opens beside the console;
- `avcodec-61.dll`, `avformat-61.dll`, `avutil-59.dll`, `swresample-5.dll` and `swscale-8.dll` (or the versions in the archive), copied next to each executable after linking;
- with `-Tests`, `dw5xl_*_test.exe`, `dw5xl_gsrec.exe` and `dw5xl_vif1_replay.exe`.

It also needs the Visual C++ runtime (`vcruntime140.dll`, `msvcp140.dll`; Visual Studio installs it, and other PCs need the VC++ 2015–2022 x64 redistributable) and the GPU driver's `vulkan-1.dll`.

## 4. Run

```powershell
cd game\dw5xl
$env:PS2X_GS_BACKEND = "vulkan"
..\..\app\build-windows\dw5xl.exe .\SLES_538.60
```

Saves go to `game\dw5xl\mc0\`, and `dw5xl.ini` (F4 resolution scale) is written to the working directory. The debug environment variables are the same as on Linux: set them with `$env:NAME = "value"` and clear them with `Remove-Item Env:NAME`. `PULSE_SINK` does not apply. Audio uses the default WASAPI device, so hidden runs (`PS2X_HIDDEN=1`) are **not** silent on Windows.

## Tests on Windows

`build.ps1 -Tests` runs `ctest` with these differences from Linux:

- `gsreplay_synthetic` is not built: it drives the tools through a POSIX shell (`env -u`, wait status). `tools/gsreplay/compare_modes.sh` is bash-only too.
- `vu1_async_queue_tsan` is not built: ThreadSanitizer is not available for Windows.
- `vu1_lift` reads the VU1 census dumps from `notes\evidence\` relative to the working directory, so the script runs it from the checkout root only when those local notes exist. On Linux, ctest also fails it for the same reason unless run from a directory with those files.

## Troubleshooting

- **Visual Studio or SDK files are missing:** errors include `vswhere.exe not found`, `No Visual Studio installation`, and a missing `windows.h`, `vcruntime.h` or `libcmt.lib`. Install the Visual Studio components above, or run the script from an "x64 Native Tools" prompt.
- **The compiler or its tools are missing:** `clang-cl not found`, or `lld-link.exe`/`llvm-lib.exe` missing: install "C++ Clang Compiler for Windows" or LLVM for Windows.
- **CMake errors about "too old" or `CMAKE_LINKER_TYPE`:** use CMake 3.29 or newer.
- **The linker cannot read bitcode** ("invalid or corrupt file", "file contains LLVM bitcode"): `link.exe`/`lib.exe` were used instead of `lld-link`/`llvm-lib`. Use a fresh build directory and keep `-DCMAKE_LINKER_TYPE=LLD -DCMAKE_AR=<llvm-lib>`.
- **The FFmpeg download or extraction fails** (`ffmpeg_external`): you are offline or GitHub is blocked; use `-FfmpegArchive`. If linking then cannot find `avcodec.lib`, the archive layout differs from what `ps2xRuntime/CMakeLists.txt` expects (`include\` and `bin\*.lib`).
- **`dw5xl.exe` exits immediately without a message:** a DLL is missing beside it. Rebuild the `dw5xl` target, which copies the FFmpeg DLLs, and install the VC++ redistributable.
- **Path or "file name too long" errors:** move the checkout to a short path. `git config --system core.longpaths true` and the `LongPathsEnabled` policy also help.
- **The Vulkan renderer is unavailable, or `PS2X_GS_BACKEND=vulkan` fails:** this includes `[gs] Vulkan unavailable ... using CPU renderer`. Update the GPU driver, and check `vulkaninfo` if you have it.
- **The game stutters or runs at the wrong speed:** run with `PS2X_STATS=1`; it should report about 50 vblanks/s, as on Linux.
- **Precompiled headers fail:** if `sccache` is installed, CMake uses it automatically. Configure with `-DPS2X_ENABLE_SCCACHE=OFF`.
- **Memory runs out during the build, or the compiler crashes on large unity batches:** use `-Jobs 4`, or fewer.
