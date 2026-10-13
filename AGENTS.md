# AGENTS.md – DW5 Xtreme Legends native port (handoff for any coding agent)

This file contains general guidance for coding agents. Read `notes/PROJECT_SETUP.md` for current target and setup requirements, `notes/BRINGUP.md` for investigation history, and `notes/SHIPPING_RESEARCH.md` before changing player setup. `CLAUDE.md` holds the general rules in shorter form.

## 1. Goal
Native Linux port of **Dynasty Warriors 5 Xtreme Legends** via static recompilation with
**PS2Recomp** (MIPS R5900 → C++). Target: XL fully playable natively, including XL's **Load Original** feature that reads
the original DW5 disc, so both games run in one port. FMV movies must work in the final version.

## 2. Rules (from the project owner)
- **The owner approves the project scope and any scope changes.** Present the proposed scope before
  acting; do not expand it or infer approval from a general request. After approval, carry out the work
  within that scope and bring any proposed change back to the owner.
- **Let the owner make important decisions and decisions about requirements.** Explain the options
  and your recommendation, then wait for the owner to choose before acting. Do not assume approval,
  invent requirements, or change agreed requirements. A request to fix an issue is not agreement with
  a particular choice. Routine implementation can proceed within the owner's agreed requirements.
- **Native over emulation.** Replace hardware use at the game/SDK level with native code where practical (done for
  libpad2 input, IPU still images, libmpeg movies via FFmpeg). Model hardware only where the runtime must.
- **Survey before reinventing.** Research existing approaches and present options. Keep research
  comparisons in local notes; dependency provenance belongs in `THIRD_PARTY.md`.
- **Ask before downloads** (state file, source, size) and before opening the game in a visible window. Never take
  screenshots of the user's screen; use the runtime's frame dumps. Hidden runs still play audio on the user's speakers –
  don't run tests while the user is testing something else.
- Only the owner's discs. **Never commit or distribute game data** (`game/`, `discs/`), Ghidra DBs or generated code.
- **No personal identity in the project.** Commits use the repo-local identity already configured in `.git/config`
  (the owner's GitHub handle + GitHub noreply email). Never use the global git identity and don't add real names/emails
  anywhere. The owner is always the author; an AI assistant may only appear as a `Co-Authored-By` trailer.
- Prefer the game's own SDK library code over runtime HLE stubs when a function only builds packets/structs
  (wrong libgraph stubs caused the black title). HLE only what touches hardware the runtime cannot model.
- Parallelise independent tasks (separate worktrees/build dirs per task, see §4); verify every fix with a test run.
- Find root causes; don't paper over symptoms (a display "zoom" workaround hid fake libgraph register values).

## 3. Repository layout
- `external/PS2Recomp/` – third-party code, not ours: a vendored copy of the upstream runtime/recompiler plus our
  maintained runtime fixes, tracked as ordinary files (no submodule). Base revision and provenance are in
  `external/PS2Recomp/DW5XL_VENDORED.md` and
  `THIRD_PARTY.md`.
  `ps2xRuntime/` (EE runtime: memory, DMA, VIF, GS CPU renderer, kernel/syscalls, SDK HLE stubs in `src/lib/Kernel/Stubs/`),
  `ps2xIOP/` (IOP R3000 interpreter running the game's real IRX modules: KOEISND, LIBSD, CDVDSTM, MCMAN, DBCMAN...;
  SPU2 model `src/emulator/core/iop_spu2.cpp`), `ps2xRecomp/` (the recompiler `ps2_recomp`).
- `recomp/dw5xl/dw5xl.toml` – recompiler config: `stubs` (SDK functions bound to runtime HLE), `entry_points`, `[mmio]`
  map, jump tables, `[patches]` (keep `instructions` EMPTY, see §8). `ghidra_functions.code.csv` = function list.
  `output/` = generated C++ (~42k files, gitignored; regenerate).
- `app/` – our executable: `CMakeLists.txt` (unity build, PCH, `-march=x86-64-v3`), `src/`:
  `dw5xl_pad2.cpp` (libpad2 → host pad bridge), `dw5xl_images.cpp` (native replacement of the IPU still-image queue
  at 0x1F0A40), `ipu_idec.{h,cpp}` (our own MPEG-2 intra/IPU IDEC decoder, 61 dB vs FFmpeg).
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
Headless test (no window, frame PNG dumps), from `game/dw5xl`: `PS2X_GS_BACKEND=cpu PS2X_HIDDEN=1 PS2X_DUMP_DIR=<dir>
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
Working: boot; input (keyboard/gamepad via libpad2 bridge); intro FMVs with audio, natural end and skip; title screen;
main menu (Xtreme/Destiny/Legend Mode, Load Original, Options, Encyclopedia, Camp, Edit, Challenge); Options menu;
sound effects + menu music (KOEISND on real LIBSD + SPU2 model); IPU still-image animation (title sky); memory card
saves (prompt defaults to No – press LEFT then Cross).
Broken / unverified – see §6.

## 6. Next tasks (priority order)
1. **Verify the SREG sound fix (built, NOT yet tested).** KOEISND publishes BGM stream status to the EE with
   sifcmd 0x80000001 (SET_SREG: SREG 0x18 = stream event flags bit0 running/0x40 paused/0x80 error, SREG 0x19 = ring
   fill); the runtime dropped it because `sceSifInitCmd` is HLE'd. Fix: `SIF.cpp dispatchSifCommand` stores SREGs;
   `sceSifGetSreg@0x122378` / `sceSifSetSreg@0x122390` bound to runtime stubs; IOP `sceCdBreak` (cdvdman 39) and
   `GetSystemTimeLow` (thbase 43) implemented. Test: `PS2X_KSNG_TRACE=1`, main menu → Options → back; expect KSNG 0x96
   (play), 0x9b fades, 0x97 stop, 0x9d/0x9e around loads. **Options silence is correct:** the owner confirmed on an
   emulator (2026-10-13) that the real game plays no music in Options, only the cursor sound when moving up/down. So
   the SREG fix still needs verifying in another scene with BGM (title/main menu loops, later battles).
2. **Overlays (biggest blocker).** The Opening Edit GS corruption is FIXED (HLE stub for sceVif1PkOpenDirectCode
   inflated a CNT qwc; the game's own function now runs). Options -> Opening -> PLAY now reaches overlay code and stops:
   `[guest-branch:missing-target]` JALR to 0x692D00, the first OVL00 constructor (table 0x699FD0..0x69A008), called from
   FUN_00100260 after the loader. Overlays OVL_00/01/02.BIN have an `MWo3` header (0x80 bytes) and load at 0x61A800
   (code at 0x61A880). Wrappers 0x2D4DC0..0x2D6010 load them through FUN_00145A50; FUN_00100260 clears BSS and runs
   constructors. PS2Recomp has no overlay support: implement per-overlay recompilation with module identity +
   runtime switching. Expect VU1 microcode / PATH1 work once 3D code runs.
3. **Save/Load round-trip test** after the sceMcGetDir count fix: save (LEFT, Cross), check the new save folder
   under `game/dw5xl/mc0/`, then Options→Load and reboot auto-load.
4. **Load Original** (DW5 disc data/overlays in `game/dw5/`), after 3D works.
5. Cleanup: strip `DW5XL-DEBUG`/`DW5XL-TEST` code, keep useful traces behind env vars; real `sceSdSetEffectAttr`
   (currently bypassed, reverb off); consider upstreaming generic fixes (VIF DIRECT, libgraph, MPEG 0xB7, MC getdir).

## 7. Debug toolkit (env vars, all optional)
`PS2X_STATS` (vsync/flip rate) · `PS2X_GS_TRACE=<file>` + `PS2X_GS_TRACE_FROM/_TO=<tick>` (REG/DRAW/XFER lines;
pseudo-register `reg_0x7f` = GIF packet marker, value = path<<32|size; note: its TEST printer shows ATST off by one bit,
the renderer decodes correctly) · `PS2X_VRAM_DUMP=<file>` (4 MiB GS memory at exit; decode with
`tools/debug/gs_vram_ct32.py`) · `PS2X_IPU_DUMP=<dir>`, `PS2X_IPU_TRACE` · `PS2X_SPU2_TRACE` · `PS2X_LIBSD_TRACE` ·
`PS2X_KSNG_TRACE` / `PS2X_KSNH_TRACE` (sound RPCs) · `PS2X_SIFAUDIO_TRACE` · `PS2X_MPEGAUDIO_TRACE` · `PS2X_MPEG_TRACE` ·
`PS2X_AUDIO_DUMP=<pcm>` (s16 stereo 48 kHz) · `PS2X_MPG_TRACE` (VU1 uploads) · `PS2X_VIF1_CHAIN_TRACE` ·
`PS2X_PAD2_TRACE` · `PS2X_RAM_DUMP_TICKS=t1,t2` (32 MiB RDRAM to `ram_<tick>.bin` in the dump dir) ·
`PS2X_RAM_WATCH=addr:len` (logs each tick a RAM range changed) · `PS2X_ZWATCH` · `PS2X_DEBUG_ZALWAYS` (force depth pass; experiments only) · `PS2X_NO_WEAVE`.
Heavy tracing slows the IOP so audio streams starve (sounds like a beep) – judge audio untraced.
No `perf`: use `tools/debug/gdb_sample_profile.sh`.

## 8. Hard-won lessons (root causes found so far)
- `ps2_analyzer` writes `[patches]` "Potential self-modifying code" entries that NOP ordinary stores. All 34 were bogus
  (broke sound: batch count never stored → master volume never set). Keep the list empty after any re-analysis.
- Runtime libgraph HLE stubs were wrong (SetDefClear did nothing, SetDefDBuff/SwapDBuff skipped clears, SetDefDispEnv
  produced fake DISPLAY values) → removed from `stubs`; DW5's own libgraph runs. Kept hardware-facing ones
  (ResetGraph, ResetPath, PutDispEnv, SyncV, SyncPath, SyncVCallback, zbufaddr, Load/StoreImage).
- Upstream VIF1 interpreter swallowed raw bytes after a DIRECT ending in an IMAGE GIFtag → desync, fake MPG/MSCAL,
  garbage GS writes. DIRECT carries only its payload; GIF PATH2 tag state continues IMAGE across DIRECTs.
- PSS movies carry several private-stream-1 sub-streams; only 0xA0–0xAF are audio (0x90 = data track).
- Movie end: retail libmpeg sets isEnd on the video sequence_end_code 0xB7; the game stops demuxing before PSS 0xB9.
- Input: DW5 uses libpad2 (DBCMAN.IRX) – bridged natively in `app/src/dw5xl_pad2.cpp`.
- Sound: KOEISND streams movie audio via SPU2 IRQ + PIO (`sceSdVoiceTrans` SD_TRANS_BY_IO); IOP/SPU2 run on the host
  clock; EE vblank pacing repays lateness and follows the game's CRT mode (PAL 50 Hz).
- Display: with real libgraph values the movie uses FIELD mode, menus FRAME mode (DH=447 → 224 rows);
  FRAME-mode scanout reads half-height fields.
- IPU still images: the game's only raw-IPU user outside libmpeg is the image queue at 0x1EFF00–0x1F0240
  (MMIO map); ipum archives (+8 w, +10 h, +0x10 {offset,size}[]), frame byte 0 = IPU_CTRL flags (DW5XL: 0x60 =
  B-15 + non-linear quant). Replaced natively.
- Memory card: libmc is HLE'd to host folders; sceMcGetDir maxent<0 = count only.

## 9. Working with the owner
- Technical owner (software developer, not a PS2 specialist): answer concisely and technically, explain PS2 concepts in
  plain words, verify claims before stating them, give a clear recommendation without assuming agreement, keep a casual tone (Danish/English mix
  is fine). Give short status updates during long work.
- Wants proactive, hands-on digging with concrete experiments – never "wait for upstream".
- Wants to SEE progress: offer a visible window at milestones; the owner playtests and reports exact steps
  (menu path, buttons, what was seen/heard). Use those steps verbatim to build a scripted reproduction.
- Decision rights stay with the owner: direction (native vs emulation), priorities, downloads, publishing.
  Present 2-4 options with a recommendation; survey other projects first.
- Multi-agent: split independent coding/recomp tasks across parallel agents with self-contained briefs; agents do
  analysis/patch writing in their own worktree or scratch subfolder; one integrator applies, builds, tests, commits.
- Keep `notes/BRINGUP.md` updated (dated sections: cause → fix → evidence → debug switches → lesson) and commit
  after each verified fix. Keep `AGENTS.md`/`CLAUDE.md` current when rules or workflow change.

## 10. Ghidra: GUI, MCP and headless
Setup on this machine:
- Ghidra **12.1.2** at `/opt/ghidra` (`/opt/ghidra/ghidraRun` = GUI, `/opt/ghidra/support/analyzeHeadless` = CLI).
- User extensions in `~/.config/ghidra/ghidra_12.1.2_DEV/Extensions/`:
  `ghidra-emotionengine-reloaded` (PS2 EE language **`r5900:LE:32:default`** – required for SLES_538.60; IOP IRX
  modules use plain `MIPS:LE:32:default`), `GhidraMCP` (GUI plugin exposing an HTTP API), `XEXLoaderWV` (unrelated).
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
- The owner opens the Ghidra GUI → File → Open Project (`ghidra/dw5xl.gpr`; don't use "Import" on a .gpr) →
  double-click **SLES_538.60** in the project tree → CodeBrowser with the GhidraMCP plugin enabled → it serves
  `http://127.0.0.1:8080/`.
- MCP server = Python bridge, stdio:
  `uvx --from <path to the GhidraMCP release's ghidra_mcp-2.0.0-py3-none-any.whl>
  ghidra-mcp --ghidra-server http://127.0.0.1:8080/` (the wheel's local path is in `notes/PROJECT_SETUP.md`). For Codex add it to `~/.codex/config.toml`, e.g.
  `[mcp_servers.ghidra]` with `command = "uvx"` and the args above (check Codex's MCP docs for the exact syntax).
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
**Visible run (ask the owner first):** `cd game/dw5xl && PS2X_GS_BACKEND=cpu ../../app/build/dw5xl ./SLES_538.60 > <log> 2>&1` started in
the background; the owner plays and reports; closing the window exits with code 0. Don't start test runs while the
owner plays or tests something else (audio overlaps). Check with `pgrep -x dw5xl`; stop with `kill <pid>`
(never `pkill -f dw5xl` from a shell whose own command line contains that string – it kills itself).

**Hidden scripted runs (the main loop):**
1. Reproduce: `PS2X_GS_BACKEND=cpu PS2X_HIDDEN=1 PS2X_HOST_INPUT="<tick>:<KEY>:<hold>,..." PS2X_DUMP_DIR=<dir> PS2X_DUMP_EVERY=<n>
   PS2X_EXIT_TICK=<n> timeout <s> ../../app/build/dw5xl ./SLES_538.60 > run.log 2>&1` (from `game/dw5xl`).
   Frame dumps are the presented frames (`frame_<tick>.png`, `final.png`); view them as images. Contact sheets:
   `magick montage frame_*.png -tile 4x4 -geometry 256x179+2+2 -label '%f' sheet.png`.
2. Timeline landmarks (ticks = vsyncs, 50/s; approximate): Koei logo 0–200, ω-Force ~300, Start at 400 skips to the main menu
   (up by ~750); without input the intro ends ~5600 → title "PRESS START BUTTON" → idle → demo battle ~6600 (hangs).
3. Instrument: env traces from §7, `[run:tick]` lines (pc/ra/DISPFB every 120 ticks), `[dw5xl:disp]` (display register
   changes), `[dw5xl:movie]`, `[dw5xl:rpc]`; GS trace + VRAM dump for rendering; `PS2X_AUDIO_DUMP` + numpy/matplotlib
   spectrogram for audio (an agent cannot listen – the owner is the ears; a 1-block ADPCM loop showed up as a constant
   ~0.6 ms period waveform).
4. Locate code in Ghidra (MCP or headless), find the root cause, patch, `tools/regen.sh` if the toml changed, rebuild,
   re-run the SAME script and compare frames/logs.
5. Milestone → offer a visible run to the owner → update BRINGUP.md → commit.
- Dev-harness command file: `PS2X_CTRL=<file>` (polled ~10x/s; `press BTN ticks`, `shot <png>`, `threads <txt>`, `quit`).
- `PS2X_HOST_INPUT` goes through the real host-input path (keyboard mapping + libpad2 bridge); `PS2X_INPUT` injects
  guest pad masks directly.
