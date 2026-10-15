# AGENTS.md – DW5 Xtreme Legends native port (handoff for any coding agent)

This file contains general guidance for coding agents. Read `notes/PROJECT_SETUP.md` for current target and setup requirements, `notes/BRINGUP.md` for investigation history, and `notes/SHIPPING_RESEARCH.md` before changing player setup. `CLAUDE.md` holds the general rules in shorter form.

## Durable project memory

- At session start, after context loss/compaction, and before resuming another agent's
  work, read `notes/PROJECT.md`, the newest `notes/CLAUDE_HANDOFF.md` section and relevant
  evidence, then verify Git status/diff and build settings. The chat is not the project
  state. Current verified project memory supersedes older status summaries, not rules.
- Keep `notes/PROJECT.md` concise and current: architecture, agreed requirements and
  decisions, implemented behavior with validation limits, known bugs, current dirty
  files/build settings, pending approvals, next steps and evidence links. Keep detailed
  plans/specs/investigations in separate local notes and link them from PROJECT.md.
- Update it after meaningful discoveries, tests, fixes or user decisions, and before
  long work batches or handing off. Preserve unfinished agent reports/proposals and
  essential evidence in `notes/evidence/`; never rely solely on chat or `/tmp`.
- Check usage when available before expensive batches and periodically during long
  work. At 10% remaining or less, checkpoint promptly before more work; do not wait
  for zero. Regular checkpoints remain required even without usage information.
- Clearly distinguish approved/applied/tested from proposed/unverified work. Record
  exact reproduction commands, remaining decisions and a short restart prompt.
  Ignore rules remain in force: notes are local and are not transferred by Git push.

## 1. Goal
Native Linux port of **Dynasty Warriors 5 Xtreme Legends** via static recompilation with
**PS2Recomp** (MIPS R5900 → C++). Target: XL fully playable natively, including XL's **Load Original** feature that reads
the original DW5 disc, so both games run in one port. FMV movies must work in the final version.

## 2. RULES
- **NEVER ASSUME — ask the owner, with a recommendation.** If you need something opened, switched, installed,
  checked or done (e.g. a different Ghidra program loaded in the GUI, PCSX2 at a certain screen, a setting changed,
  a file provided), or you are unsure what the owner has or wants, ask: say what you need, why, and what you
  recommend. Do not assume what is open, installed or intended, and do not route around a missing piece silently.
- **Scope changes require explicit approval.** Present the proposed scope before
  acting; do not expand it or infer approval from a general request. After approval, carry out the work
  within that scope and bring any proposed changes back for confirmation.
- **Important decisions and requirements require confirmation.** Explain the options
  and recommendations, then wait for confirmation before acting. Do not assume approval,
  invent requirements, or change agreed requirements. A request to fix an issue is not agreement with
  a particular choice. Routine implementation can proceed within agreed requirements.
- **Library, function, and graphics behavior decisions require confirmation.** Explain the options,
  give a recommendation, and wait for a decision before implementing it, even within approved scope.
  Research, diagnosis, tests, and preparation of a proposal can continue while waiting; do not apply the proposed change.
- **Native over emulation.** Replace hardware use at the game/SDK level with native code where practical.
  Model hardware only where the runtime must.
- **Survey before reinventing.** Research existing approaches and present options. Keep research
  comparisons in local notes; dependency provenance belongs in `THIRD_PARTY.md`.
- **Ask before downloads** (state file, source, size) and before opening the game in a visible window. Never take
  screenshots of the user's screen; use the runtime's frame dumps. Hidden runs still play audio on the user's speakers –
  don't run tests while the user is testing something else.
- Hidden tests use a separate silent audio output while retaining game audio processing/capture. Live tests use default desktop output. Never change the default output or route other applications.
- Legitimate original discs only. **Never commit or distribute game data** (`game/`, `discs/`), Ghidra DBs or generated code.
- **GPU renderer only: use Vulkan/paraLLEl-GS for development, game runs, debugging and performance testing.** Do not run CPU renderer comparisons, develop CPU-renderer fixes, or propose switching back to CPU unless explicitly requested. Existing CPU backend code may remain; its presence does not authorize CPU work.
- **No automatic Git staging or commits.** Leave changes unstaged; create commits only upon explicit command.
- **Never commit or push anything derived from the game's code or data**: no generated or lifted C++ (the lifted
  VU1 code lives only in the ignored `recomp/dw5xl/vu1/`; never `dw5xl_vu1_lifted.cpp` anywhere in Git), frame dumps
  (`final.png`, `frame_*.png`), RAM/VRAM/VU captures or microcode `.bin` files. Repo hooks in `.githooks/` block the
  lifted VU1 code on commit and push (enable once per clone: `git config core.hooksPath .githooks`). Never bypass
  hooks (`--no-verify`, `-n`), and never push without the owner's explicit command.
- Prefer the game's own SDK library code over runtime HLE stubs when a function only builds packets/structs.
  HLE only what touches hardware the runtime cannot model.
- Parallelise independent tasks (separate worktrees/build dirs per task, see §4); verify every fix with a test run.
- Find root causes; don't paper over symptoms with workarounds.

## 3. Repository layout
- `external/PS2Recomp/` – third-party code, not ours: a vendored copy of the upstream runtime/recompiler plus our
  maintained runtime fixes, tracked as ordinary files (no submodule). Base revision and provenance are in
  `external/PS2Recomp/DW5XL_VENDORED.md` and
  `THIRD_PARTY.md`.
  `ps2xRuntime/` (EE runtime: memory, DMA, VIF, Vulkan paraLLEl-GS GPU compute renderer and CPU fallback renderer,
  kernel/syscalls, SDK HLE stubs in `src/lib/Kernel/Stubs/`),
  `ps2xIOP/` (IOP R3000 interpreter running the game's real IRX modules: KOEISND, LIBSD, CDVDSTM, MCMAN, DBCMAN...;
  SPU2 model `src/emulator/core/iop_spu2.cpp`), `ps2xRecomp/` (the recompiler `ps2_recomp`).
- `recomp/dw5xl/dw5xl.toml` – recompiler config: `stubs` (SDK functions bound to runtime HLE), `entry_points`, `[mmio]`
  map, jump tables, `[patches]` (keep `instructions` EMPTY, see §8). `ghidra_functions.code.csv` = function list.
  `output/` = generated C++ (~42k files, gitignored; regenerate).
- `app/` – our executable: `CMakeLists.txt` (unity build, PCH, `-march=x86-64-v3`), `src/`:
  `dw5xl_pad2.cpp` (libpad2 → host pad bridge), `dw5xl_images.cpp` (native replacement of the IPU still-image queue
  at 0x1F0A40), `ipu_idec.{h,cpp}` (our own MPEG-2 intra/IPU IDEC decoder, 61 dB vs FFmpeg),
  `dw5xl_overlays.cpp` (native active-overlay lifecycle and dispatch).
- `tools/` – `regen.sh`, `fix_ghidra_csv.py`, `ghidra_scripts/` (DecompileAt, CallersOf, DecompileAll,
  ExportPS2Functions), `ipu/` (decoder self-test + FFmpeg comparison), `debug/` (VRAM CT32 decoder, gdb sampler).
- `game/dw5xl/` (extracted XL disc, run from here; `mc0/`,`mc1/` = memory cards), `game/dw5/` (DW5 disc files),
  `discs/` (ISOs), `ghidra/` (project dw5xl, program SLES_538.60), `ghidra_iop/` (project koeisnd: KOEISND.IRX,
  module-relative addresses; runtime base 0x53b00). All gitignored.

## 3a. How our changes reach the game
Generated C++ registers every game function under its PS2 address; calls go through that table. Overrides in
`app/src` register per disc serial (`PS2_REGISTER_GAME_OVERRIDE(..., "SLES_538.60", ...)`), verify the expected
instruction at the address, then `runtime.replaceFunction(addr, fn)`. `runtime.lookupFunction(addr)` returns the
original so a replacement can wrap it (call the original, then adjust its result) – the intended route for later
camera changes. PS2Recomp has no mid-function hooks. Disc files are read only; never patch them or generated output.
Game files live in ignored local folders (`game/dw5xl`, `game/dw5`, `discs/`).

## 4. Build, run, version control
Current build and run commands are in `notes/PROJECT_SETUP.md`.
Headless test (no window, frame PNG dumps), from `game/dw5xl`: `PS2X_GS_BACKEND=vulkan PS2X_HIDDEN=1 PS2X_DUMP_DIR=<dir>
PS2X_DUMP_EVERY=100 PS2X_EXIT_TICK=1000 PS2X_HOST_INPUT="400:ENTER:20,..." ../../app/build/dw5xl ./SLES_538.60`
(tick = vsync, 50/s on PAL; pre-create the dump dir).
Host input names: ENTER(Start) X(Cross) C(Circle) Z(Square) V(Triangle) UP DOWN LEFT RIGHT Q E (L1 R1).
Leave ~100 ticks between menu presses (scroll animations swallow input). Known sequences:
- Intro skip → main menu: `400:ENTER:20` (menu up by ~750).
- Main menu → Options: `800:DOWN:8,900:DOWN:8,1000:X:10` (Options at ~1100).
- Options → Opening → Play: then `1300..1600: DOWN x6 every 60`, `1700:X:10`, `1850:X:10`.
Git: runtime changes are ordinary commits in this repo (`external/PS2Recomp/` is vendored; keep runtime and app changes in
separate commits where practical). Parallel work: `git worktree add ../dw5-ps2-<task> -b <task>`, symlink `game/`
and `app/deps/`, build the recompiler in `external/PS2Recomp/build`, own `app/build-<task>`.
Ghidra headless (GUI may lock the project → work on a copy):
`/opt/ghidra/support/analyzeHeadless <copydir> dw5xl -process SLES_538.60 -noanalysis -readOnly
-scriptPath tools/ghidra_scripts -postScript DecompileAt.java 0x1F0A40` (output lines prefixed `INFO DecompileAt.java>`).
Recompiled C++ for any function: `recomp/dw5xl/output/*_0x<addr>.cpp` (each instruction is commented with its asm).

## 5. Status (2026-10-13)
- **Working:**
  - **Boot & Menus:** Full boot to title screen and main menu (all modes), Options submenus, and Opening Edit thumbnails.
  - **Input:** Native keyboard and gamepad support via libpad2 bridge (`dw5xl_pad2.cpp`).
  - **Movies & Video:** PSS intro movies with audio demuxing, natural end (0xB7), and Start skipping; title screen sky animation decoded via native IDEC decoder (`ipu_idec.cpp`).
  - **Audio:** KOEISND running on IOP interpreter with real LIBSD + SPU2 model; menu BGM, streaming movie audio, and cursor SFX (Options menu silence authentic).
  - **3D Graphics:** Real-time 3D rendering (Opening scene 3D character models, castle gates, sky) using native libvu0 (unbound `sceVu0*` stubs).
  - **Vulkan GPU Renderer:** paraLLEl-GS GPU compute backend (`PS2X_GS_BACKEND=vulkan`) with authentic PAL scanout (640x512 full viewport), comb-free `ReusableFieldWeaver` deinterlacing, native, 2x and 4x output (`PS2X_GS_SCALE=1|2|4`, F4) with separate anti-aliasing samples (`PS2X_GS_SSAA=1|2|4|8|16`, F5; renderer patch in `vendor/parallel-gs-patches/`), and a 3-slot asynchronous readback ring buffer running at solid 50 fps (CPU backend fallback).
  - **Memory Card:** Saving and loading via HLE libmc (default prompt is No – press LEFT then Cross).
  - **Overlays:** Native module generation and active-module dispatch (`dw5xl_overlays.cpp`) for OVL00 (Opening) and OVL01 (Xtreme Mode), constructor execution, and indirect vtable dispatch.
- **Broken / Unverified:** See §6.

## 6. Next tasks (priority order)
1. **In-game 3D scenes & Legend Mode loading:** Legend Mode battle loading screen stalls after tick 853 (no disc reads); investigate loading state and 3D scene initialization.
2. **Overlay coverage & Indirect dispatch:** Continue expanding indirect call and vtable method mapping for OVL00/OVL01/OVL02 to ensure full scene and gameplay coverage.
3. **Legend Mode submenus & font rendering:** Diagnose any remaining UI/font rendering issues in submenus (e.g. black UI screens in Xtreme/Legend Mode submenus).
4. **Save/Load round-trip test:** Verify save creation (`mc0/`) and reboot auto-load / Options→Load.
5. **Load Original:** DW5 disc data/overlays from `game/dw5/` once core 3D gameplay is verified.
6. **Codebase cleanup:** Strip temporary debug instrumentation (`DW5XL-DEBUG`/`DW5XL-TEST`), retain useful traces behind env vars, implement authentic `sceSdSetEffectAttr` (currently bypassed), and prepare generic upstreamable fixes (VIF DIRECT, libgraph, MPEG 0xB7, MC getdir, Vulkan PAL scanout & async ring buffer).

## 7. Debug toolkit (env vars, all optional)
`PS2X_STATS` (vsync/flip rate) · `PS2X_GS_BACKEND=vulkan` (required backend for current development and testing) ·
`PS2X_GS_SCALE=1|2|4|8|16` (SSAA scaling factor; default 1x) · `PS2X_GS_SYNCREADBACK=1` (force synchronous readback for A/B testing) ·
`PS2X_NO_WEAVE=1` (bypass ReusableFieldWeaver) · `PS2X_GS_TRACE=<file>` + `PS2X_GS_TRACE_FROM/_TO=<tick>` (REG/DRAW/XFER lines;
pseudo-register `reg_0x7f` = GIF packet marker, value = path<<32|size; note: its TEST printer shows ATST off by one bit,
the renderer decodes correctly) · `PS2X_VRAM_DUMP=<file>` (4 MiB GS memory at exit; decode with
`tools/debug/gs_vram_ct32.py`) · `PS2X_IPU_DUMP=<dir>`, `PS2X_IPU_TRACE` · `PS2X_SPU2_TRACE` · `PS2X_LIBSD_TRACE` ·
`PS2X_KSNG_TRACE` / `PS2X_KSNH_TRACE` (sound RPCs) · `PS2X_SIFAUDIO_TRACE` · `PS2X_MPEGAUDIO_TRACE` · `PS2X_MPEG_TRACE` ·
`PS2X_AUDIO_DUMP=<pcm>` (s16 stereo 48 kHz) · `PS2X_MPG_TRACE` (VU1 uploads) · `PS2X_VIF1_CHAIN_TRACE` ·
`PS2X_PAD2_TRACE` · `PS2X_RAM_DUMP_TICKS=t1,t2` (32 MiB RDRAM to `ram_<tick>.bin` in the dump dir) ·
`PS2X_RAM_WATCH=addr:len` (logs each tick a RAM range changed) · `PS2X_ZWATCH` · `PS2X_DEBUG_ZALWAYS` (force depth pass; experiments only).
Heavy tracing slows the IOP so audio streams starve (sounds like a beep) – judge audio untraced.
No `perf`: use `tools/debug/gdb_sample_profile.sh`.

## 8. Hard-won lessons & gotchas
- **`[patches]` in `dw5xl.toml`**: `ps2_analyzer` falsely flags ordinary stores as "Potential self-modifying code" and generates patches that NOP them. Keep `instructions` empty in `[patches]` after any re-analysis.
- **SDK HLE stubs vs game code**: Runtime HLE stubs often have subtle bugs (e.g. `libgraph` skipped clears, `libvu0` inverted matrix multiplication order, `libvif` altered DMA tag qwc). Prefer unbinding stubs in `dw5xl.toml` so authentic game code runs, keeping only hardware-facing primitives.
- **VIF1 DIRECT & GIF transfers**: DIRECT carries only its own payload; GIF PATH2 tag state continues IMAGE across DIRECTs.
- **Audio & IOP timing**: IOP/SPU2 run on the host clock; EE vblank pacing repays lateness and follows PAL 50 Hz. Heavy tracing slows IOP execution enough to starve audio streams (sounds like a beep/stutter) – evaluate audio untraced.
- **Overlays (OVL00–OVL02)**: Overlays load at 0x619000; native lifecycle, constructor execution, and indirect dispatch are handled in `app/src/dw5xl_overlays.cpp`.
- Full investigation history and detailed root causes are recorded in `notes/BRINGUP.md`.

## 9. Collaboration & Communication
- **PS2 concepts in plain words:** Answer concisely and technically, explain PS2 concepts simply, verify claims before stating them, give clear recommendations without assuming agreement. Give short status updates during long work.
- **Hands-on debugging:** Proactive, hands-on digging with concrete experiments – never "wait for upstream".
- **Visual verification:** Offer visible window testing at milestones; translate playtest reports into scripted reproductions.
- **Decision making:** Direction (native vs emulation), priorities, external downloads, and publishing require explicit confirmation. Present 2-4 options with recommendations; survey existing projects first.
- **Multi-agent coordination:** Split independent coding/recomp tasks across parallel agents with self-contained briefs; agents do analysis/patch writing in their own worktree or scratch subfolder; one integrator applies, builds and tests; commits require explicit command.
- Keep `notes/BRINGUP.md` updated (dated sections: cause → fix → evidence → debug switches → lesson) after each verified fix. Commit only upon explicit command. Keep `AGENTS.md`/`CLAUDE.md` current when rules or workflow change.

## 10. Ghidra: GUI, MCP and headless
Ghidra setup:
- Ghidra **12.1.2** at `/opt/ghidra` (`/opt/ghidra/ghidraRun` = GUI, `/opt/ghidra/support/analyzeHeadless` = CLI).
- User extensions in `~/.config/ghidra/ghidra_12.1.2_DEV/Extensions/`:
  `ghidra-emotionengine-reloaded` (PS2 EE language **`r5900:LE:32:default`** – required for SLES_538.60; IOP IRX
  modules use plain `MIPS:LE:32:default`), `GhidraMCP` (GUI plugin exposing an HTTP API).
- Projects: `ghidra/dw5xl.gpr` (programs: SLES_538.60, plus IRX modules such as LIBSD.IRX) and `ghidra_iop/koeisnd.gpr`
  (KOEISND.IRX). IRX addresses are module-relative; runtime IOP bases from the boot log: SIO2MAN 0x10000, MCMAN 0x11000,
  MCSERV 0x38400, SIO2D 0x3bc00, DBCMAN 0x3ea00, DS2O_D 0x43c00, LIBSD 0x46400, SDHD 0x4c100, CDVDSTM 0x4ef00,
  **KOEISND 0x53b00**.
- Re-create the EE project (if lost): `analyzeHeadless ghidra dw5xl -import game/dw5xl/SLES_538.60 -overwrite
  -processor r5900:LE:32:default -scriptPath tools/ghidra_scripts -postScript ExportPS2Functions.java
  <abs>/recomp/dw5xl/ghidra.toml <abs>/recomp/dw5xl/ghidra_functions.csv`, then
  `tools/fix_ghidra_csv.py ghidra_functions.csv ghidra_functions.code.csv 310CE8` (drops data past code end 0x310CE8,
  clamps two oversized functions) → feeds `dw5xl.toml ghidra_output`.

**MCP (interactive, fastest for many small lookups):**
- Open the Ghidra GUI → File → Open Project (`ghidra/dw5xl.gpr`; don't use "Import" on a .gpr) →
  double-click **SLES_538.60** in the project tree → CodeBrowser with the GhidraMCP plugin enabled → it serves
  `http://127.0.0.1:8080/`.
- MCP server = Python bridge, stdio:
  `uvx --from <path to the GhidraMCP release's ghidra_mcp-2.0.0-py3-none-any.whl>
  ghidra-mcp --ghidra-server http://127.0.0.1:8080/` (the wheel's local path is in `notes/PROJECT_SETUP.md`).
- Tools: get_program_info (CALL FIRST – the GUI may have a different program open, e.g. LIBSD.IRX),
  decompile_function_by_address, disassemble_function, get_function_by_address, get_xrefs_to/from,
  get_function_calls, list_functions/strings/segments, rename_function_by_address, set_*_comment.
- Limits: hardware registers (0x10000000+, 0x12000000+) and scratchpad (0x70000000+) have no xrefs → use the
  `[mmio]` map in `dw5xl.toml` to find code touching a register. If the backend is unreachable (GUI closed/reboot),
  fall back to headless.

**Headless (batch, scripts, when the GUI holds the lock):**
- ONE source project: `ghidra/dw5xl.gpr` (GUI/MCP; all renames/comments go here). Never run headless on it and never
  treat a copy as a source. Every headless use starts from a fresh throwaway copy in the user's/agent's own scratch dir:
  `cp -r ghidra/dw5xl.gpr ghidra/dw5xl.rep <mydir>/` (KOEISND: `ghidra_iop/koeisnd.*`), run `-readOnly` against it, then
  delete it. Parallel agents each make their own copy.
- `analyzeHeadless <mydir> dw5xl -process SLES_538.60 -noanalysis -readOnly -scriptPath tools/ghidra_scripts
  -postScript <Script>.java <args>` – KOEISND: `analyzeHeadless <mydir> koeisnd -process -noanalysis -readOnly ...`.
  Scripts: `DecompileAt.java 0xADDR...` (decompile the functions containing the addresses), `CallersOf.java 0xADDR`
  (list callers + decompile them), `DecompileAll.java <outfile>` (whole program to one C file – great for grep, used for
  KOEISND), `ExportPS2Functions.java <toml> <csv>`.
- Output goes to stdout with every line prefixed `INFO  <Script>.java>`; extract with
  `sed -n '/=== /,/REPORT: Save/p'`. Each run costs 30–60 s JVM start, so batch many addresses per run.
- Cheap alternative for one function: the generated C++ `recomp/dw5xl/output/*_0x<addr>.cpp` contains every
  instruction as a comment (address, word, mnemonic).

## 11. How we test and open the game
**Visible run (ask before opening visible window):** `cd game/dw5xl && PS2X_GS_BACKEND=vulkan ../../app/build/dw5xl ./SLES_538.60 > <log> 2>&1` started in
the background; conduct manual playtesting and report; closing the window exits with code 0. Don't start test runs while a
visible playtest or other test is active (audio overlaps). Check with `pgrep -x dw5xl`; stop with `kill <pid>`
(never `pkill -f dw5xl` from a shell whose own command line contains that string – it kills itself).

**Hidden scripted runs (the main loop):**
1. Reproduce: `PS2X_GS_BACKEND=vulkan PS2X_HIDDEN=1 PS2X_HOST_INPUT="<tick>:<KEY>:<hold>,..." PS2X_DUMP_DIR=<dir> PS2X_DUMP_EVERY=<n>
   PS2X_EXIT_TICK=<n> timeout <s> ../../app/build/dw5xl ./SLES_538.60 > run.log 2>&1` (from `game/dw5xl`; explicitly select `vulkan`).
   Frame dumps are the presented frames (`frame_<tick>.png`, `final.png`); view them as images. Contact sheets:
   `magick montage frame_*.png -tile 4x4 -geometry 256x179+2+2 -label '%f' sheet.png`.
2. Timeline landmarks (ticks = vsyncs, 50/s; approximate): Koei logo 0–200, ω-Force ~300, Start at 400 skips to the main menu
   (up by ~750); without input the intro ends ~5600 → title "PRESS START BUTTON" → idle → demo battle ~6600 (hangs).
3. Instrument: env traces from §7, `[run:tick]` lines (pc/ra/DISPFB every 120 ticks), `[dw5xl:disp]` (display register
   changes), `[dw5xl:movie]`, `[dw5xl:rpc]`; GS trace + VRAM dump for rendering; `PS2X_AUDIO_DUMP` + numpy/matplotlib
   spectrogram for audio analysis.
4. Locate code in Ghidra (MCP or headless), find the root cause, patch, `tools/regen.sh` if the toml changed, rebuild,
   re-run the SAME script and compare frames/logs.
5. Milestone → offer visible run testing → update BRINGUP.md. Commit only upon explicit command.
**When unsure how the original looks or behaves, compare against PCSX2 before guessing or asking:**
- `tools/pcsx2_reference.py --out <scratch dir> --seconds <n> [--interval 0.5] [--keys "<sec>:<keysym>,..."]` runs the
  owner's PCSX2 (`~/Games/psx2/*.AppImage`, PAL disc in `discs/dw5xl/`) headless on a private Xvfb display with a
  throwaway copy of the owner's PCSX2 config, and takes F8 screenshots into `<out>/snaps`. It uses PCSX2's software
  renderer: the accurate GS reference (its Vulkan/GL renderers approximate). Keys: Cross=x, Start=Return, Circle=c,
  Square=z, Triangle=v, D-pad=arrows. Without input the disc plays intro → title → demo → title → real-time Opening.
- Capture the same moment from the port (`PS2X_DUMP_DIR`/`PS2X_DUMP_EVERY`) and compare contact sheets side by side.
- Never send synthetic input to the real desktop (KDE pops a "remote control" prompt for the owner); never edit the
  owner's `~/.config/PCSX2`. Screenshots are game-derived: scratch or `notes/evidence/` only, never git.
- Still ask the owner when PCSX2 itself may be wrong, or for things screenshots cannot show (feel, audio timing).

**Rendering bugs: record and replay one frame offline.** `PS2X_GS_RECORD=<file> PS2X_GS_RECORD_FROM=<tick>
PS2X_GS_RECORD_TO=<tick>` records every renderer call (window starts with a VRAM snapshot; ~2 MB per tick in battle).
`app/build/dw5xl_gsrender <rec> <cpu|vulkan> <outdir> <ticks> [--fb fbp,fbw,w,h] [--stop-at <record>]
[--probe "x,y;..." --z zbp,zpsm]` renders it with either renderer; bisect with `--stop-at` on the Vulkan-vs-CPU
difference to find the exact draw. `dw5xl_gsrec dump <rec> --from N --count M --fields` shows the draws.
Vulkan renderer fixes go to `app/deps/parallel-gs` as runtime patches in `vendor/parallel-gs-patches/` (shader SPIR-V
via `tools/parallel_gs_splice_shader.py`).

- Dev-harness command file: `PS2X_CTRL=<file>` (polled ~10x/s; `press BTN ticks`, `shot <png>`, `threads <txt>`, `quit`).
- `PS2X_HOST_INPUT` goes through the real host-input path (keyboard mapping + libpad2 bridge); `PS2X_INPUT` injects
  guest pad masks directly.

- Manual visible playtests must leave `PS2X_HOST_INPUT` and `PS2X_INPUT` unset. A nonempty host-input script disables
  physical keyboard polling for the entire run, including after its last event.
- Keep one game instance. The integrator closes manual playtests before builds or hidden runs; agents do scratch
  analysis and patch preparation without shared builds or launches during active playtests.
