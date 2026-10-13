# DW5 Xtreme Legends native port (PS2Recomp)

Read `notes/CLAUDE_HANDOFF.md` for the latest local handoff and paused work before proceeding.

Static recompilation of **Dynasty Warriors 5 Xtreme Legends** to a native Linux
executable with PS2Recomp. Goal: XL fully playable natively, including XL's **Load Original** feature that reads the
original DW5 disc, so both games run in one port. Movies must work in the final version.

Read `notes/PROJECT_SETUP.md` for current target and setup requirements, and `notes/SHIPPING_RESEARCH.md` before changing player setup.
Read `notes/BRINGUP.md` first: it is the running log of every fix, the current blocker and the "NEXT" list.
Keep it up to date (append dated sections; record causes, fixes, debug switches and lessons).

## Ground rules (from the user)
- **The owner approves the project scope and any scope changes.** Present the proposed scope before
  acting; do not expand it or infer approval from a general request. After approval, carry out the work
  within that scope and bring any proposed change back to the owner.
- **Let the owner make important decisions and decisions about requirements.** Explain the options
  and your recommendation, then wait for the owner to choose before acting. Do not assume approval,
  invent requirements, or change agreed requirements. A request to fix an issue is not agreement with
  a particular choice. Routine implementation can proceed within the owner's agreed requirements.
- **Native over emulation.** Replace hardware use at the game/SDK level with native code (as done for libpad2 input,
  IPU still images, libmpeg movies). Only model hardware where the runtime has to.
- **Survey first.** Research existing approaches and present options. Keep research comparisons in
  local notes; dependency provenance belongs in `THIRD_PARTY.md`.
- **Downloads need explicit permission** (name, source, size). Ask before running the game in a visible window; never
  screenshot the user's screen (use frame dumps instead). Hidden runs still play audio on the user's speakers.
- Only the user's own discs; never commit or distribute game files (`game/`, `discs/`).
- Prefer the game's own SDK library code over runtime HLE stubs when a function only builds packets/structs (wrong
  stubs caused the black title screen). HLE only what touches hardware the runtime cannot model.
- **Work with multiple agents.** When there are several independent coding/recomp tasks (separate bugs, subsystems,
  RE questions, research), split them across background agents instead of doing everything serially. Give each agent
  a self-contained brief (addresses, files, what is known, expected output). Agents must not rebuild the shared build
  tree or run the game concurrently: give them analysis/patch-writing tasks (Ghidra on their own project copy, reading
  code, writing a diff to their scratch dir) and apply, build and test their patches yourself, one at a time.

## Layout
- `external/PS2Recomp/` – third-party runtime/recompiler (not ours), vendored with our maintained runtime fixes
  (plain tracked files, no submodule; base revision in `external/PS2Recomp/DW5XL_VENDORED.md`).
  Dependency provenance is documented in `THIRD_PARTY.md`. Runtime: `ps2xRuntime/`, IOP emulator (runs real IRX modules): `ps2xIOP/`, recompiler: `ps2xRecomp/`.
- `recomp/` – recompiler profiles (stubs, entry points, MMIO map, jump tables, patches);
  `ghidra_functions.code.csv` – function list (clean with `tools/fix_ghidra_csv.py`); `output/` – generated C++ (~42k files).
- `app/` – our executable: `CMakeLists.txt` (unity build, PCH, `-march=x86-64-v3`), `src/` game-specific native code:
  - `dw5xl_pad2.cpp` – libpad2 → host pad bridge (game override)
  - `dw5xl_images.cpp` – native replacement of the IPU still-image queue
  - `ipu_idec.{h,cpp}` – our own IPU IDEC decoder (MPEG-2 intra, spec tables, 61 dB vs FFmpeg)
- `game/` – local disc data; `ghidra/` and `ghidra_iop/` – local analysis projects. Active paths and program names are in `notes/PROJECT_SETUP.md`.
- `tools/` – `regen.sh` (regenerate + rsync only changed files), `ghidra_scripts/` (DecompileAt, CallersOf,
  DecompileAll, ExportPS2Functions), `ipu/` (decoder self-test, FFmpeg comparison).

## Version control
- Top-level repo (branch `main`) holds our code/config; local notes are ignored. GitHub destination: TheSlackbringer/dw5xl-recomp.
  `external/PS2Recomp/` is vendored: runtime changes are ordinary commits here; see `THIRD_PARTY.md` for provenance.
- `.gitignore` keeps out game data (`game/`, `discs/`), Ghidra databases, generated code (`recomp/*/output/`,
  regenerate with `tools/regen.sh`), builds, logs and `notes/`. Never force-add those.
- Parallel agents: give each its own worktree (`git worktree add ../dw5-ps2-<task> -b <task>`), its own `game/` and `app/deps/` symlinks, its own
  `tools/regen.sh` output and `app/build-<task>` dir. Full build ~10+ min. Merge results back via branches.
- Commits use the repo-local identity (the owner's GitHub handle with their GitHub noreply email, already configured in
  `.git/config`); never use the global git identity or add real names/emails. The owner is always the author; Claude may be added
  with a `Co-Authored-By` trailer.

## Build & run
Current build and run commands are in `notes/PROJECT_SETUP.md`.
Headless test pattern (no window, frame dumps you can view):
`PS2X_HIDDEN=1 PS2X_DUMP_DIR=<dir> PS2X_DUMP_EVERY=100 PS2X_EXIT_TICK=900 PS2X_HOST_INPUT="400:ENTER:20" ...`
(Start at tick 400 skips the intro; the main menu is up by ~tick 750). Keys: Enter=Start, X/Space=Cross, C=Circle,
Z=Square, V=Triangle, arrows/WASD.

Ghidra: ONE source project, `ghidra/dw5xl.gpr`. Use it through GUI/MCP for interactive lookups (check MCP first when
the owner has the GUI open). Headless never runs on the source: every headless use makes a fresh throwaway copy in its
own scratch dir, runs `-readOnly`, then deletes it. See `notes/PROJECT_SETUP.md` for commands.
Working split: the main session builds, runs and tests one thing at a time and uses MCP; bigger RE or code-review
questions go to parallel background agents (each with its own headless copy or code-reading brief).

## Debug switches (env)
`PS2X_STATS` vsync/flip rate · `PS2X_GS_TRACE=<file>` + `_FROM/_TO` ticks (REG/DRAW/XFER; pseudo-reg 0x7F marks GIF
packet path) · `PS2X_VRAM_DUMP=<file>` raw 4 MiB GS memory at exit (CT32 deswizzle: block/column tables) ·
`PS2X_IPU_DUMP=<dir>` / `PS2X_IPU_TRACE` · `PS2X_SPU2_TRACE` · `PS2X_LIBSD_TRACE` · `PS2X_KSNG_TRACE` / `PS2X_KSNH_TRACE`
(sound RPCs) · `PS2X_SIFAUDIO_TRACE` · `PS2X_MPEGAUDIO_TRACE` · `PS2X_AUDIO_DUMP=<pcm>` (s16 stereo 48 kHz) ·
`PS2X_MPG_TRACE` (VU1 uploads) · `PS2X_VIF1_CHAIN_TRACE` · `PS2X_PAD2_TRACE` · `PS2X_RAM_DUMP_TICKS=t1,t2` / `PS2X_RAM_WATCH=addr:len` (RAM dumps/watch) · `PS2X_DEBUG_ZALWAYS` (force depth pass,
experiments only) · `PS2X_ZWATCH`. Heavy tracing slows the IOP enough to starve audio streams (sounds like a beep).
Debug code is tagged `DW5XL-DEBUG` / `DW5XL-TEST` for later cleanup.

## Hard-won lessons (details in BRINGUP.md)
- The analyzer's `[patches]` "Potential self-modifying code" entries NOP real stores; keep that list empty.
- Runtime libgraph HLE stubs were wrong (no clears, fake DISPLAY values) → run DW5's own libgraph instead.
- The libvif `sceVif1PkOpenDirectCode` HLE stub refreshed the open DMA tag's qwc; the game's close adds to it → CNT
  qwc 4 instead of 3 (Opening Edit corruption). Unbound; the game's own libvif runs. Check stub side effects first.
- VIF1 DIRECT carries only its own payload; GIF PATH2 state continues IMAGE transfers (upstream shortcut desynced).
- PSS movies carry several private-stream-1 sub-streams; only 0xA0–0xAF are audio.
- DW5 uses libpad2 (DBCMAN), not libpad; KOEISND streams via SPU2 IRQ + PIO (`sceSdVoiceTrans` BY_IO).
- IOP SPU2/IOP clock follows the host clock; EE vblank pacing must repay lateness and follow the selected CRT mode (using the active display cadence). MPEG frame intervals and PTS conversions must use the same cadence.

Manual visible tests: unset `PS2X_HOST_INPUT` and `PS2X_INPUT`; a host-input script disables real keyboard polling
for the entire run. Use one game instance. Close the manual test before shared builds/hidden runs; background agents
prepare analysis/patches in scratch without launching the game while the owner plays.
