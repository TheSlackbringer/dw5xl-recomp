# DW5 Xtreme Legends native port (PS2Recomp)

Read `notes/CLAUDE_HANDOFF.md` for the latest local handoff and paused work before proceeding.

Static recompilation of **Dynasty Warriors 5 Xtreme Legends** to a native Linux
executable with PS2Recomp. Goal: XL fully playable natively, including XL's **Load Original** feature that reads the
original DW5 disc, so both games run in one port. Movies must work in the final version.

Read `notes/PROJECT_SETUP.md` for current target and setup requirements, and `notes/SHIPPING_RESEARCH.md` before changing player setup.
Read `notes/BRINGUP.md` first: it is the running log of every fix, the current blocker and the "NEXT" list.
Keep it up to date (append dated sections; record causes, fixes, debug switches and lessons).

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

## RULES
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
- **Survey first.** Research existing approaches and present options. Keep research comparisons in
  local notes; dependency provenance belongs in `THIRD_PARTY.md`.
- **Downloads need explicit permission** (name, source, size). Ask before running the game in a visible window; never
  screenshot the user's screen (use frame dumps instead). Hidden runs still play audio on the user's speakers.
- Hidden tests use a separate silent audio output while retaining game audio processing/capture. Live tests use default desktop output. Never change the default output or route other applications.
- Legitimate original discs only; never commit or distribute game files (`game/`, `discs/`).
- **GPU renderer only: use Vulkan/paraLLEl-GS for development, game runs, debugging and performance testing.** Do not run CPU renderer comparisons, develop CPU-renderer fixes, or propose switching back to CPU unless explicitly requested. Existing CPU backend code may remain; its presence does not authorize CPU work.
- **No automatic Git staging or commits.** Leave changes unstaged; create commits only upon explicit command.
- **Never commit or push anything derived from the game's code or data**: no generated or lifted C++ (the lifted
  VU1 code lives only in the ignored `recomp/dw5xl/vu1/`; never `dw5xl_vu1_lifted.cpp` anywhere in Git), frame dumps
  (`final.png`, `frame_*.png`), RAM/VRAM/VU captures or microcode `.bin` files. Repo hooks in `.githooks/` block the
  lifted VU1 code on commit and push (enable once per clone: `git config core.hooksPath .githooks`). Never bypass
  hooks (`--no-verify`, `-n`), and never push without the owner's explicit command.
- Prefer the game's own SDK library code over runtime HLE stubs when a function only builds packets/structs.
  HLE only what touches hardware the runtime cannot model.
- **Work with multiple agents.** When there are several independent coding/recomp tasks (separate bugs, subsystems,
  RE questions, research), split them across background agents instead of doing everything serially. Give each agent
  a self-contained brief (addresses, files, what is known, expected output). Agents must not rebuild the shared build
  tree or run the game concurrently: give them analysis/patch-writing tasks (Ghidra on their own project copy, reading
  code, writing a diff to their scratch dir) and apply, build and test their patches yourself, one at a time.

## Layout
- `external/PS2Recomp/` – third-party runtime/recompiler (not ours), vendored with our maintained runtime fixes
  (plain tracked files, no submodule; base revision in `external/PS2Recomp/DW5XL_VENDORED.md`).
  Dependency provenance is documented in `THIRD_PARTY.md`. Runtime: `ps2xRuntime/` (EE runtime, Vulkan paraLLEl-GS
  GPU compute renderer and CPU fallback renderer), IOP emulator (runs real IRX modules): `ps2xIOP/`, recompiler: `ps2xRecomp/`.
- `recomp/` – recompiler profiles (stubs, entry points, MMIO map, jump tables, patches);
  `ghidra_functions.code.csv` – function list (clean with `tools/fix_ghidra_csv.py`); `output/` – generated C++ (~42k files).
- `app/` – our executable: `CMakeLists.txt` (unity build, PCH, `-march=x86-64-v3`), `src/` game-specific native code:
  - `dw5xl_pad2.cpp` – libpad2 → host pad bridge (game override)
  - `dw5xl_images.cpp` – native replacement of the IPU still-image queue
  - `ipu_idec.{h,cpp}` – our own IPU IDEC decoder (MPEG-2 intra, spec tables, 61 dB vs FFmpeg)
  - `dw5xl_overlays.cpp` – native active-overlay lifecycle and dispatch (OVL00, OVL01)
- `game/` – local disc data; `ghidra/` and `ghidra_iop/` – local analysis projects. Active paths and program names are in `notes/PROJECT_SETUP.md`.
- `tools/` – `regen.sh` (regenerate + rsync only changed files), `ghidra_scripts/` (DecompileAt, CallersOf,
  DecompileAll, ExportPS2Functions), `ipu/` (decoder self-test, FFmpeg comparison).

## Current status & next tasks
- **Working:** Full boot & menu navigation (all modes); libpad2 host input; PSS intro movies with audio demuxing, natural 0xB7 end, and skip; native IDEC still-image decoding; KOEISND sound/BGM + cursor SFX (Options silence authentic); real-time 3D rendering (Opening scene models/castles/sky) via native libvu0; Vulkan paraLLEl-GS GPU compute backend with authentic PAL scanout (640x512 full viewport), comb-free `ReusableFieldWeaver` deinterlacing, native/2x/4x output with separate anti-aliasing (renderer patch in `vendor/parallel-gs-patches/`), and 3-slot asynchronous readback ring buffer at solid 50 fps (CPU backend fallback); HLE libmc memory card; native overlay lifecycle and active-module dispatch (`dw5xl_overlays.cpp`) for OVL00 and OVL01.
- **Next tasks:** (1) In-game 3D scenes & Legend Mode battle loading stall (tick 853); (2) Overlay coverage & indirect vtable dispatch expansion; (3) Legend Mode submenus and font rendering; (4) Save/Load round-trip test; (5) Load Original DW5 disc integration; (6) Strip temporary debug instrumentation and prepare upstreamable fixes.

## Version control
- Top-level repo (branch `main`) holds our code/config; local notes are ignored. GitHub destination: TheSlackbringer/dw5xl-recomp.
  `external/PS2Recomp/` is vendored: runtime changes are ordinary commits here; see `THIRD_PARTY.md` for provenance.
- `.gitignore` keeps out game data (`game/`, `discs/`), Ghidra databases, generated code (`recomp/*/output/`,
  regenerate with `tools/regen.sh`), builds, logs and `notes/`. Never force-add those.
- Parallel agents: give each its own worktree (`git worktree add ../dw5-ps2-<task> -b <task>`), its own `game/` and `app/deps/` symlinks, its own
  `tools/regen.sh` output and `app/build-<task>` dir. Full build ~10+ min. Merge results back via branches.
- **No automatic Git staging or commits.** Leave changes unstaged; create commits only upon explicit command.

## Build & run
Current build and run commands are in `notes/PROJECT_SETUP.md`.
Headless test pattern (no window, frame dumps you can view):
`PS2X_GS_BACKEND=vulkan PS2X_HIDDEN=1 PS2X_DUMP_DIR=<dir> PS2X_DUMP_EVERY=100 PS2X_EXIT_TICK=900 PS2X_HOST_INPUT="400:ENTER:20" ...`
(Start at tick 400 skips the intro; the main menu is up by ~tick 750). Keys: Enter=Start, X/Space=Cross, C=Circle,
Z=Square, V=Triangle, arrows/WASD.

Ghidra: ONE source project, `ghidra/dw5xl.gpr`. Use it through GUI/MCP for interactive lookups (check MCP first when
the GUI is open). Headless never runs on the source: every headless use makes a fresh throwaway copy in its
own scratch dir, runs `-readOnly`, then deletes it. See `notes/PROJECT_SETUP.md` for commands.
Working split: the main session builds, runs and tests one thing at a time and uses MCP; bigger RE or code-review
questions go to parallel background agents (each with its own headless copy or code-reading brief).

Unsure how the original looks? Compare with PCSX2 first: `tools/pcsx2_reference.py` (headless, private Xvfb, copy of
the owner's config, accurate software renderer; details in AGENTS.md §11). Rendering bug in one frame? Record with
`PS2X_GS_RECORD` (+`_FROM/_TO`) and replay/bisect it with `dw5xl_gsrender` (Vulkan vs CPU). Never send synthetic input
to the real desktop or edit `~/.config/PCSX2`.

## Debug switches (env)
`PS2X_STATS` vsync/flip rate · `PS2X_GS_BACKEND=vulkan` · `PS2X_GS_SCALE=1|2|4|8|16` (SSAA) · `PS2X_GS_SYNCREADBACK=1` · `PS2X_NO_WEAVE=1` ·
`PS2X_GS_TRACE=<file>` + `_FROM/_TO` ticks (REG/DRAW/XFER; pseudo-reg 0x7F marks GIF packet path) · `PS2X_VRAM_DUMP=<file>` raw 4 MiB GS memory
at exit (CT32 deswizzle: block/column tables) · `PS2X_IPU_DUMP=<dir>` / `PS2X_IPU_TRACE` · `PS2X_SPU2_TRACE` · `PS2X_LIBSD_TRACE` ·
`PS2X_KSNG_TRACE` / `PS2X_KSNH_TRACE` (sound RPCs) · `PS2X_SIFAUDIO_TRACE` · `PS2X_MPEGAUDIO_TRACE` · `PS2X_AUDIO_DUMP=<pcm>` (s16 stereo 48 kHz) ·
`PS2X_MPG_TRACE` (VU1 uploads) · `PS2X_VIF1_CHAIN_TRACE` · `PS2X_PAD2_TRACE` · `PS2X_RAM_DUMP_TICKS=t1,t2` / `PS2X_RAM_WATCH=addr:len` (RAM dumps/watch) ·
`PS2X_DEBUG_ZALWAYS` (force depth pass, experiments only) · `PS2X_ZWATCH`. Heavy tracing slows the IOP enough to starve audio streams (sounds like a beep).
Debug code is tagged `DW5XL-DEBUG` / `DW5XL-TEST` for later cleanup.

## Hard-won lessons & gotchas (details in BRINGUP.md)
- Keep `dw5xl.toml` `[patches]` empty: `ps2_analyzer` falsely flags ordinary stores as self-modifying code.
- Prefer game SDK code over runtime HLE stubs when stubs exhibit incorrect behavior or side effects (unbind in `dw5xl.toml`).
- VIF1 DIRECT carries only its own payload; GIF PATH2 state continues IMAGE transfers across DIRECTs.
- IOP/SPU2 clock follows the host clock; EE vblank pacing must repay lateness and follow PAL 50 Hz.
- Heavy IOP tracing starves audio streams; test audio untraced.
- Details of previous investigations and root causes are in `notes/BRINGUP.md`.

Manual visible tests: unset `PS2X_HOST_INPUT` and `PS2X_INPUT`; a host-input script disables real keyboard polling
for the entire run. Use one game instance. Close the manual test before shared builds/hidden runs; background agents
prepare analysis/patches in scratch without launching the game during active manual testing.
