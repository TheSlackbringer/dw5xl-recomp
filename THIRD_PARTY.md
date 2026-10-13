# Third-party code and notices

The maintained DW5XL port source and tools are distributed under the GNU General Public License version 3; see [LICENSE](LICENSE). Third-party material retains its applicable license and copyright notices. This license grants no rights to Dynasty Warriors game code, disc files, trademarks or assets. Generated translations and player-owned disc files are excluded from this source repository.

## PS2Recomp and runtime patches

- [PS2Recomp](https://github.com/ran-j/PS2Recomp), by ran-j and contributors: GPL-3.0. `external/PS2Recomp/` is a vendored, modified copy of the upstream runtime/recompiler with a runtime patch series and DW5XL modifications. The upstream base revision and a summary of the changes are recorded in `external/PS2Recomp/DW5XL_VENDORED.md`; later changes are recorded in this repository's Git history.
- Runtime patch series from [Voicedrew11/holyland](https://github.com/Voicedrew11/holyland), by Voicedrew11 and contributors. That repository's own tooling is MIT; its README identifies the PS2Recomp-derived `patches/` as GPL-3.0. Copies are retained in `vendor/runtime-patches/`; original patch headers and the source URLs in `list.txt` preserve provenance. Specific MIT portions are listed below.
- The vendored runtime is a modified version, not the unmodified upstream release.
- `tools/ghidra_scripts/ExportPS2Functions.java` is adapted from PS2Recomp's exporter. The other Ghidra helper scripts use Ghidra's scripting API; this repository does not vendor Ghidra itself.

## Notices for incorporated MIT portions

These notices are reproduced unchanged from the modified runtime and must accompany redistribution of the relevant portions:

- [RecompOne SPU notice](licenses/RecompOne-SPU.txt): copied Gaussian coefficients and adapted sound algorithms, including the original source path and revision. Incorporated through the runtime patch series' sound patch and the IOP runtime.
- [Verdite2 mouse indicator notice](licenses/Verdite2-mouse-indicator-LICENSE.txt): runtime mouse-capture indicator. Incorporated through the runtime patch series' indicator patch.

These notices also remain at their original locations inside the runtime and in the corresponding vendored patches.

## Build dependencies

The build uses additional projects rather than vendoring their complete source into this top-level repository:

| Dependency | Source | License information |
| --- | --- | --- |
| raylib | [raysan5/raylib](https://github.com/raysan5/raylib) | zlib/libpng-style license; see the dependency's LICENSE. |
| Dear ImGui | [ocornut/imgui](https://github.com/ocornut/imgui) | MIT; see LICENSE.txt. |
| rlImGui | [raylib-extras/rlImGui](https://github.com/raylib-extras/rlImGui) | zlib-style license; see LICENSE. |
| sse2neon, where enabled | [DLTcollab/sse2neon](https://github.com/DLTcollab/sse2neon) | Preserve the license notices in the exact dependency revision used. |
| FFmpeg | [FFmpeg](https://ffmpeg.org/legal.html) | Licensing depends on the configuration and components of the linked build; verify the actual libraries before redistributing them. |

The recompiler and dependencies can have additional transitive components; retain their upstream notices. This document records current source provenance, not a complete binary-release license manifest. Before distributing executables or dependency libraries, assemble the notices for the exact build and provide the corresponding source required by its licenses.
