# Linux builder release

The AppImage contains the graphical setup launcher, native PS2 recompiler, app
and runtime sources, Clang, CMake, Ninja, a Linux SDK and minimal FFmpeg. It
contains no game files, translated code or game executable. On first start the
player selects their own PAL XL ISO, optionally adds DW5, and builds offline.
Player files go to `$XDG_DATA_HOME/dw5xl` (normally `~/.local/share/dw5xl`).
Build inputs are synchronized into its `builder/` folder before a first build
or an update. Identical files keep their timestamps, so a changing AppImage
mount path does not discard the player's compiled objects. Normal later starts
skip this synchronization and launch the game directly.

The target is x86-64 with AVX2, glibc 2.31 or newer, and a Vulkan graphics driver.
The compiler baseline is built in the pinned Steam Runtime 3 SDK; it does not
require a compiler, Git or Python on the player's system.

## Build the package

CI prepares the source dependencies and checked tool archives, then runs the
same offline builder used locally. The Linux AppImage and Windows ZIP are
uploaded to the same numbered draft only after all required jobs pass.
Publishing remains an owner action.

Local prerequisites are Podman, Python and the existing dependency sources.
Pull the builder image once: `podman pull "$(cat tools/release/linux-builder-image)"`.
The release build itself runs with networking disabled and never installs host
packages. Supply these already downloaded inputs:

```sh
export DW5XL_DEP_ROOT=/path/to/app/deps
export DW5XL_RECOMP_DEPS=/path/to/recompiler/build/_deps
export DW5XL_FFMPEG_SOURCE=/path/to/ffmpeg-7.1.tar.xz
export DW5XL_APPIMAGE_TOOLS=/path/to/appimage-tools
export DW5XL_LINUX_SDK_WORK=/path/to/linux-sdk-work
export DW5XL_BUILD_JOBS=2
bash tools/release/build_linux.sh
```

`DW5XL_DEP_ROOT` holds patched `parallel-gs`, `raylib-src`, `imgui-src` and
`rlimgui-src` source checkouts. `DW5XL_RECOMP_DEPS` holds the five dependency
checkouts populated by configuring PS2Recomp. No game data is used.

`linux-tools.json` pins the AppImage tools and FFmpeg source. CI checks size and
SHA-256 before use. Local builds never fetch missing inputs automatically.

## Builder image

The compiler and the libc++ that the package ships come from one image:
`tools/release/linux.Containerfile` (Steam Runtime 3 sniper SDK, pinned by
digest, plus clang/lld/llvm 23 and the six libc++ `.deb` files of the same
apt.llvm.org snapshot in `/approved-debs`, listed with name, version, size and
SHA-256 in `/approved-debs/manifest.json`). The workflow `builder-image`
builds it and publishes it to `ghcr.io/theslackbringer/dw5xl-release-linux`;
`tools/release/linux-builder-image` holds the exact image (by digest) that CI and
local release builds use. The release build checks the `.deb` files against the
manifest before using them, and makes no request to apt.llvm.org.

To update the compiler or libc++: run the `builder-image` workflow (Actions,
"Run workflow"), check the libc++ version in its summary, and commit the printed
image line into `tools/release/linux-builder-image`. `DW5XL_LINUX_IMAGE`
overrides the image for a local test, e.g. one built from the Containerfile.

The script builds the launcher and recompiler, runs the setup tests, scans the
AppDir for forbidden game content, and writes
`DW5XL-RECOMP-<version>-x86_64.AppImage` with SHA-256 and `.zsync` sidecars
under `dist/`. SDK export, compiler, test and packaging logs stay in
`DW5XL_LINUX_SDK_WORK`. A clean checkout is required; `--test` permits local
changes and labels the resulting package for testing.

For systems without FUSE, AppImage's standard `APPIMAGE_EXTRACT_AND_RUN=1`
environment setting can be used. The launcher stays alive while its game child
runs so the bundled runtime libraries remain available.

## Updates

The AppImage carries update information for AppImage updaters such as
AppImageUpdate: `gh-releases-zsync|TheSlackbringer|dw5xl-recomp|latest|DW5XL-RECOMP-*x86_64.AppImage.zsync`
(`UPDATE_INFORMATION` in `make_release.py`). An updater asks GitHub for the
latest full release, which skips pre-releases, and downloads the changed parts
of the AppImage that its `.zsync` names. appimagetool writes the `.zsync` with
its bundled `zsyncmake`; the build fails unless it exists and matches the
AppImage, because appimagetool only warns when it cannot make one.

The `.zsync` names the AppImage by file name. The draft job renames the
AppImage from the commit to the release tag, so it also renames the `.zsync`
and rewrites its `Filename:` and `URL:` lines (`zsync_file.py`); the checksums
inside describe the bytes and stay valid. Renaming a release or its tag is
harmless; renaming the AppImage or `.zsync` asset by hand breaks updates for
that release. Drafts are created as full releases so that a published release
reaches updaters; marking one as a pre-release hides it from them.

Updating the AppImage updates the builder only. The player's files stay in
`$XDG_DATA_HOME/dw5xl`; the first start of a newer AppImage rebuilds the game
once without asking for the disc again, and later starts open the game directly.
