# AGENTS.md — Dynasty Warriors 5 Xtreme Legends native port

Static recompilation of **Dynasty Warriors 5 Xtreme Legends** (PAL SLES_538.60) to a native Linux executable with
**PS2Recomp** (MIPS R5900 → C++), Vulkan paraLLEl-GS renderer. Goal: XL fully playable natively, including XL's
**Load Original** (DW5 disc), movies included. GitHub: TheSlackbringer/dw5xl-recomp.

Read first: this file → `notes/issues/README.md` (local: index, "Waiting for you", working agreement) → the current
issue's `spec.md` and `work.md`. Setup/build commands: `notes/PROJECT_SETUP.md`. Look-ups (repo map, debug switches,
Ghidra, PCSX2, record/replay, pitfalls): "Reference" at the end of this file. History only: `notes/BRINGUP.md`,
`notes/CLAUDE_HANDOFF.md`, `notes/OWNER_TEST_CHECKLIST.md`.

## Hard rules (owner, 2026-10-19 — after an agent weakened tests to make them pass)
- **Never change a test's pass criteria** (thresholds, tick windows, run mode) to make it pass. Changing criteria is
  a spec change: the owner approves it first. A failing test is reported as failing.
- **No timing workarounds:** no delays, sleeps or knobs tuned to one game routine or one machine. Fix the root cause.
- **Every claimed result names its log** (`notes/issues/tests/out/...`). "Passes" means the run has finished.
- **Only the owner approves a spec.** Write "approved" only after the owner said so, with the date.
- **Pushing is the owner's:** push only after the owner agreed to that push.
- Changes to `notes/` are tracked in its own local git repo (`git -C notes log`); commit your notes changes there.
- Before relying on the game files, `notes/tools/check_game_files.sh` must pass (they must match the player's disc).

## How we work with the owner (agreed 2026-10-16)
1. **One folder per issue**, `notes/issues/NNN-short-name/` (local, never committed; templates in `notes/issues/`):
   - `spec.md` — what the owner and you agreed. Copy `notes/issues/TEMPLATE-spec.md` and fill EVERY section:
     what the owner sees; background (facts with numbers, what is ruled out); how to reproduce; **recommended fix**
     (step by step, why it works, expected result); **native vs emulation** (a header line plus a section: which
     parts are native code, which stay emulated, anything moving between them); **main risks** (what could go wrong
     or need an owner decision, and what then); alternatives considered (and why not); measurable pass criteria;
     the automated test; the 1–3 "Owner test" items only the owner can judge; out of scope. Plain words, no
     unexplained jargon. Examples: issues 001 and 002.
   - `work.md` — stop/go milestones (not in the spec), all your findings, progress, measurements, conclusion, fix
     (commit) and history, kept current. Findings never live only in chat.
2. **Spec approval before any work.** Draft the spec (fact-finding needed to write it is fine); the owner approves.
   Push back with reasons when you think a spec is wrong; the owner decides and can overrule. No fixes or experimental
   code changes without an approved spec. While away, the owner can allow work on an unapproved spec — explicitly.
3. **A test per issue, written first:** `notes/issues/tests/issue_NNN_*.py` prints PASS/FAIL and must FAIL on the bug
   before the fix (where measurable). Runner: `notes/issues/tests/run.py NNN|quick|standard|full|list`; helpers and routes in
   `tests/dw5test.py`. Closed issues keep their tests as regression guards.
4. **Testing by change (owner 2026-10-21, replaces mandatory suite gates):** run all fast automated checks
   after every change: unit tests, Python unit tests and static checks. Also run checks relevant to the changed
   code and behaviour. Use CI to validate builds. Slow gameplay runs are separate from the fast checks.
   **After fixing at least three specs, recommend a full-suite run and ask the owner directly before running it.**
   Never start the full suite without the owner's explicit approval or request. There is no automatic broad-suite
   requirement before commits, pushes or owner test sessions. Test thresholds, tick windows, repeats and pass
   criteria stay unchanged; report failures and incomplete runs honestly, with logs. Owner tests cover the spec's
   short "Owner test" list. Test window: `run.py window NNN ...` (full logging filed under the issues).
   Normal play: `run.py play`.
5. **Commits and pushes:** one local commit per fixed issue ("Fix #00N: …") when it reaches "fixed — owner test".
   Push only after the owner confirmed the issues in the batch and agreed to the push. Other commits only on command.
6. **Stop/go:** split big work into milestones in `work.md`. Owner present: a two-line status per milestone, and an
   immediate report (with the alternative) when an approach doesn't pay off. Owner away: keep going, log milestones.
7. **While the owner is away** you may: work on approved specs in the agreed order; run automated tests off-screen;
   update `work.md` and the index; make local per-issue commits; capture PCSX2 references (headless); create NEW
   issues (draft spec + findings) — but not fix them. Wait for the owner for: fixes without an approved spec, spec
   changes, pushing, any visible window or desktop action, deleting outside your scratch space, downloads/installs/
   system settings, turning the PC off (only when asked that time). Stuck: write it under "Waiting for you" and
   continue with the next approved issue.

## Rules
- **NEVER ASSUME — ask the owner, with a recommendation.** Need something opened, switched, installed or done
  (a Ghidra tab, a PCSX2 screen, a file), or unsure what the owner has or wants: ask what, why, and what you recommend.
- **Decisions are the owner's:** scope, requirements, priorities, library/function/graphics behaviour choices,
  native-vs-emulation direction. Present 2–4 options with a recommendation; research and diagnosis may continue while
  waiting, applying the choice may not. A request to fix something is not agreement with a particular approach.
- **Native over emulation;** model hardware only where the runtime must. Prefer the game's own SDK code over runtime
  HLE stubs. Find root causes; no workarounds that paper over symptoms. Survey existing approaches before inventing.
- **Performance changes must give identical output** (same frames, same sound). Test at **1x**: every run passes
  `PS2X_GS_SCALE=1 PS2X_GS_SSAA=1` and you check the `[output] scale=1x` log line.
- **GPU renderer only** (Vulkan/paraLLEl-GS) for development and tests; no CPU-renderer work unless asked.
- **The port is not specific to the owner's hardware.** Keep fixes portable across supported hardware and the
  planned Linux/Windows targets. The owner's CPU/GPU are test equipment, not product requirements. Label
  performance measurements with their test hardware; do not generalize one machine's timings or introduce
  machine-specific fixes. Correctness is defined by PS2 behaviour. (Owner reminder, 2026-10-18.)
- **The owner's desktop is off limits:** never send synthetic input to it, never screenshot it (use frame dumps),
  never unlock it, never edit `~/.config/PCSX2`. Visible windows only for an owner test session or when asked.
- **One game at a time;** no builds or test runs while the owner has a window open. Tests use the silent sink
  `dw5xl_codex_tests`; live play uses the default output. Never change audio defaults or route other apps.
- **Downloads need explicit permission** (name, source, size).
- **Nothing derived from the game in Git:** no game files (`game/`, `discs/`), Ghidra DBs, generated or lifted C++
  (`recomp/*/output/`, `recomp/dw5xl/vu1/`), frame dumps, captures, microcode `.bin`, screenshots. Scan before every
  commit/push. Hooks in `.githooks/` (`git config core.hooksPath .githooks`); never bypass them (`--no-verify`).
- **Releases (owner decision 2026-10-19, issue 011 v5):** a release contains only our code and freely licensed
  tools (the builder: wizard, recompiler, sources, compiler) — never the compiled game, disc files, an ISO,
  generated or lifted C++ or any other game-derived file. The player's PC builds the game (PS2Recomp is GPL-3.0; a
  shipped game binary would break it). Shipping the compiled game only with written permission from PS2Recomp's
  authors. Releases are published only by the owner.
- **Commit identity:** author/committer `TheSlackbringer <15327900+TheSlackbringer@users.noreply.github.com>`;
  end messages with `Co-Authored-By: Claude <model> <noreply@anthropic.com>`. Call our renderer fixes "runtime
  patches" (`vendor/`). `external/PS2Recomp/` is vendored: runtime changes are ordinary commits here.
- **Use the Ghidra MCP whenever you can** (`mcp__ghidra__*` tools: decompile, xrefs, strings, renames/comments)
  for reverse engineering; call `get_program_info` first. Headless only when the MCP is unreachable (GUI closed),
  for batch jobs, or for parallel agents — always on a copy (see Reference → Ghidra). If the needed program is not
  the active tab, ask the owner to switch it.
- **Parallel agents** get self-contained briefs and do analysis/patch-writing in their own worktree or scratch dir
  (Ghidra on their own copy); one integrator builds and runs the game. Each worktree gets its own `app/build-<task>`.
- **Never delete build dirs containing `_deps`;** no `rm` with bare shell variables (use `"${VAR:?}"`).
- Keep `AGENTS.md` current when rules or the way of working change. Check usage
  before expensive batches; at ≤10 % left, checkpoint (`work.md`, index) before continuing.

## Essentials
- Build: `cmake --build app/build` (all), `--target dw5xl` (game). Regenerate code: `tools/regen.sh main|ovl00|…`
  (needs the player's disc in `game/dw5xl/`). CTests: `cd app/build && ctest`.
- Run all fast checks plus checks relevant to the changed area (testing rule above). Available issue tiers: `run.py NNN|quick|standard|full`;
  recommend full after at least three specs are fixed and ask the owner before running it. Perf/stutter: `tools/heavy_battle_test.py` (no frame dumps when measuring timing).
- Unsure how the original looks or behaves: compare with PCSX2 first (`tools/pcsx2_reference.py`), then ask the owner
  for what screenshots can't show (feel, sound timing).
- Debug code: `DW5XL-DEBUG` (kept, env-gated) and `TEMP-DEBUG` (remove before committing).

## Reference

### Repository map
- `external/PS2Recomp/` — vendored upstream runtime/recompiler plus our runtime fixes (ordinary files; provenance in
  `external/PS2Recomp/DW5XL_VENDORED.md` and `THIRD_PARTY.md`).
  - `ps2xRuntime/` — EE runtime: memory, DMA, VIF, VU0/VU1 (interpreter `src/lib/vu/ps2_vu1_core.cpp`, lifted-code
    support `include/runtime/ps2_vu1_lift.h`), GS front end + Vulkan paraLLEl-GS backend, kernel/syscalls, SDK HLE
    stubs (`src/lib/Kernel/Stubs/`), EE scheduler (`src/lib/Kernel/EeScheduler.cpp`), async device queue
    (`src/lib/ps2_device_queue.cpp`), GIF arbiter + GS worker (`src/lib/gs/ps2_gif_arbiter.cpp`).
  - `ps2xIOP/` — IOP R3000 interpreter running the game's real IRX modules (KOEISND, LIBSD, CDVDSTM, MCMAN …),
    IOP kernel `src/emulator/core/iop_kernel.cpp`, SPU2 model `src/emulator/core/iop_spu2.cpp`.
  - `ps2xRecomp/` — the recompiler `ps2_recomp` (VU0 macro ops: `src/lib/vu_translator.cpp`).
- `recomp/dw5xl/dw5xl.toml` — recompiler config (stubs, entry points, `[mmio]` map, jump tables, `[patches]`).
  `recomp/dw5xl/output/` and `recomp/dw5xl/overlays/ovl0N/output/` — generated C++ (gitignored; `tools/regen.sh`).
  `recomp/dw5xl/vu1/` — lifted VU1 C++ (gitignored, game-derived; `tools/vu1_translator.py`).
  `recomp/dw5xl/vu0/` — lifted VU0 microprograms (gitignored, game-derived; `tools/vu0_translator.py`, issue 025;
  the launcher's C++ port `app/setup/vu1lift/` writes both, CTests `vu1lift/vu0lift_matches_python`).
- `app/` — our executable: `src/dw5xl_pad2.cpp` (pad bridge), `dw5xl_images.cpp` + `ipu_idec.*` (native IPU stills),
  `dw5xl_overlays.cpp` (overlay lifecycle/dispatch); tests in `app/tests/` (CTest).
- `tools/` — `regen.sh`, `vu1_translator.py`/`vu1_timing.py`/`disc_vu_inputs.py` (VU1 lifting from the player's own
  disc: offsets + hashes only), `heavy_battle_test.py`, `pcsx2_reference.py`, `gsreplay/`, `ghidra_scripts/`, `debug/`.
- Local, gitignored: `game/dw5xl/` (extracted XL disc; run from here; `mc0/`,`mc1/` memory cards), `game/dw5/`,
  `discs/`, `ghidra/` (project dw5xl), `ghidra_iop/` (project koeisnd), `notes/` (issues, evidence, history).

### How our code reaches the game
Generated C++ registers every game function under its PS2 address; calls go through that table. Overrides in `app/src`
register per disc serial (`PS2_REGISTER_GAME_OVERRIDE(..., "SLES_538.60", ...)`), verify the expected instruction,
then `runtime.replaceFunction(addr, fn)`; `runtime.lookupFunction(addr)` returns the original so a replacement can wrap
it. No mid-function hooks. Never patch disc files or generated output by hand.

### Running the game by hand (prefer the issue tests and helpers)
From `game/dw5xl`: `PS2X_GS_BACKEND=vulkan PS2X_HIDDEN=1 PS2X_GS_SCALE=1 PS2X_GS_SSAA=1 PULSE_SINK=dw5xl_codex_tests
PS2X_HOST_INPUT="<tick>:<KEY>:<hold>,..." PS2X_DUMP_DIR=<dir> PS2X_DUMP_EVERY=<n> PS2X_EXIT_TICK=<n> timeout <s>
../../app/build/dw5xl ./SLES_538.60 > run.log 2>&1`. Ticks are PAL vsyncs (50/s). Keys: ENTER (Start), X (Cross),
C (Circle), Z (Square), V (Triangle), UP DOWN LEFT RIGHT, Q/E (L1/R1); leave ~100 ticks between menu presses.
Known routes (main menu, Opening, battle resume, heavy fight, defeat): `notes/issues/tests/dw5test.py`.
Timeline: Koei logo 0–200, Start at 400 skips to the main menu (up by ~750).
A host-input script disables real keyboard polling for the whole run: never set `PS2X_HOST_INPUT`/`PS2X_INPUT` for
manual play. Silent sink missing after a reboot: `pactl load-module module-null-sink sink_name=dw5xl_codex_tests`.
Stop a game with `kill <pid>` (never `pkill -f dw5xl` from a shell whose own command line contains that string).
Presented-window timing without touching the desktop: instead of `PS2X_HIDDEN=1`, prefix the game with
`gamescope --backend headless -W 2560 -H 1440 -w 2560 -h 1440 -r 240 --` (the owner's monitor; used by
`tools/heavy_battle_test.py` and the 007 test). Frame dumps are the presented frames (`frame_<tick>.png`); contact
sheet: `magick montage frame_*.png -tile 4x4 -geometry 256x179+2+2 -label '%f' sheet.png`.

### Debug switches (env, all optional)
Pacing/perf: `PS2X_STATS` (vsync/s per second) · `PS2X_HUD_LOG` ([hud] fps/vsync/frameMs/maxFrameMs every 0.5 s) ·
`PS2X_LATE_VBLANK` (fields >3 ms late) · `PS2X_VU1_ASYNC_TRACE` (device worker busy/drains per second; "explicit=N"
render syncs = 2 x game FPS) · `PS2X_VU1_CENSUS=<file>` (per-VU1-program runs/cycles/host ns; columns
liftedRuns/liftedHostNs = translated share; resumes filed under the pc they continue from) · `PS2X_VU1_LIFT=0`
(interpret all VU1) · `PS2X_VU1_ASYNC=0` (synchronous device) · `PS2X_GS_ASYNC=0` (GS inline, no worker thread).
VU0 microprograms (`VCALLMS`, issue 025): `PS2X_VU0_LIFT=0` (interpret them) · `PS2X_VU0_LIFT_VERIFY=1` (every
native VU0 run repeated by the interpreter and compared; `[vu0verify]`) · `PS2X_VU0_CENSUS=<file>` (per-program
runs/cycles/host ns + the 4 KB image, game data) · `[vu0lift:hit|miss]` counters give the native share.
Test runs without a silent sink (Windows): `PS2X_MUTE=1` (master volume 0, audio timing unchanged).
VU1 exactness/speed: `PS2X_VU1_LIFT_VERIFY=1` (every native VU1 run repeated by the interpreter and compared; slow) ·
`PS2X_DEVICE_PROFILE=1` with the async trace (device time: transfer/VU1/GIF submit/VIF unpack) ·
`PS2X_VU1_RUN_CAPTURE=<prefix>` + `_PC/_FROM/_COUNT` (real run starts for `dw5xl_vu1_lift_test benchcap`; game data,
local only). Translator: `DW5XL_VU1_STATIC=all|none|<segments>` (static timing, default all);
`tools/vu1_find_disc_range.py` adds a VU1 program to the lift from a local census image.
Graphics: `PS2X_GS_SCALE` / `PS2X_GS_SSAA` · `PS2X_NO_WEAVE` · `PS2X_GS_SYNCREADBACK` · `PS2X_GS_TRACE=<file>` +
`_FROM/_TO` · `PS2X_GS_RECORD=<file>` + `_FROM/_TO` · `PS2X_VRAM_DUMP=<file>` (decode: `tools/debug/gs_vram_ct32.py`) ·
`PS2X_XGKICK_CAPTURE=<prefix>` + `_FROM/_TO` · `PS2X_DEBUG_ZALWAYS`, `PS2X_ZWATCH` (experiments only).
Sound/IOP: `PS2X_SPU2_TRACE` · `PS2X_LIBSD_TRACE` · `PS2X_KSNG_TRACE` / `PS2X_KSNH_TRACE` (sound RPCs) ·
`PS2X_AUDIO_DUMP=<wav>` (s16 stereo 48 kHz) · `PS2X_AUDIO_TRACE` · `PS2X_SIFAUDIO_TRACE` · `PS2X_MPEGAUDIO_TRACE` ·
`PS2X_IOP_HEAP_TRACE` · `PS2X_IO_TRACE`. Heavy IOP tracing starves audio streams: judge audio untraced.
Other: `PS2X_IPU_DUMP=<dir>` / `PS2X_IPU_TRACE` · `PS2X_MPG_TRACE` · `PS2X_VIF1_CHAIN_TRACE` · `PS2X_PAD2_TRACE` ·
`PS2X_RAM_DUMP_TICKS=t1,t2` · `PS2X_RAM_WATCH=addr:len` · `PS2X_CTRL=<file>` (dev command file: press/shot/threads/quit).
Debug code in the runtime is tagged `DW5XL-DEBUG`; temporary probes `TEMP-DEBUG` (remove before committing).
No `perf` on this machine: profile with `gprofng collect app` (see notes) or `tools/debug/gdb_sample_profile.sh`.

### Comparing with the original (PCSX2)
`tools/pcsx2_reference.py --out <scratch> --seconds <n> [--interval 0.5] [--keys "<sec>:<keysym>,..."]` runs the
owner's PCSX2 headless on a private Xvfb display with a throwaway copy of the owner's config and the software renderer,
taking F8 screenshots into `<out>/snaps` (keys: Cross=x, Start=Return, Circle=c, Square=z, Triangle=v, D-pad=arrows).
Capture the same moment from the port and compare side by side. Screenshots are game-derived (scratch or
`notes/evidence/` only). Never touch the owner's `~/.config/PCSX2` or send input to the real desktop.

### Rendering bugs: record and replay one frame
`PS2X_GS_RECORD=<file> PS2X_GS_RECORD_FROM=<tick> PS2X_GS_RECORD_TO=<tick>` records every renderer call (starts with
a VRAM snapshot; ~2 MB/tick in battle). `app/build/dw5xl_gsrender <rec> <cpu|vulkan> <outdir> <ticks> [--fb ...]
[--stop-at <record>] [--probe "x,y;..." --z zbp,zpsm]` replays it; bisect with `--stop-at`.
`dw5xl_gsrec dump <rec> --from N --count M --fields` lists the draws. Vulkan renderer fixes are runtime patches in
`vendor/parallel-gs-patches/` (shader SPIR-V via `tools/parallel_gs_splice_shader.py`).

### Ghidra
- Ghidra 12.1.2 at `/opt/ghidra`. EE language `r5900:LE:32:default` (ghidra-emotionengine-reloaded); IRX modules
  `MIPS:LE:32:default`. GhidraMCP plugin serves the active CodeBrowser tab at `http://127.0.0.1:8080/` (bridge command
  and wheel path in `notes/PROJECT_SETUP.md`). Call `get_program_info` first: the GUI may show another program.
- **MCP (interactive, fastest for many small lookups):** Ghidra GUI → File → Open Project `ghidra/dw5xl.gpr` →
  double-click the program (SLES_538.60, OVL_00.BIN, LIBSD.IRX …) → CodeBrowser with GhidraMCP serves
  `http://127.0.0.1:8080/`. Tools: get_program_info (FIRST), decompile_function_by_address, disassemble_function,
  get_function_by_address, get_xrefs_to/from, get_function_calls, list_functions/strings/segments,
  rename_function_by_address, set_*_comment. Limits: hardware registers (0x10000000+, 0x12000000+) and scratchpad
  (0x70000000+) have no xrefs → use the `[mmio]` map in `dw5xl.toml`. Unreachable (GUI closed) → headless.
- ONE source project `ghidra/dw5xl.gpr` (renames/comments go here, via GUI/MCP). Headless never runs on it: copy
  `ghidra/dw5xl.gpr` + `ghidra/dw5xl.rep` (KOEISND: `ghidra_iop/koeisnd.*`) to your scratch dir, run
  `analyzeHeadless <dir> dw5xl -process SLES_538.60 -noanalysis -readOnly -scriptPath tools/ghidra_scripts
  -postScript DecompileAt.java 0xADDR ...` (also `CallersOf.java`, `DecompileAll.java <out>`), then delete the copy.
  Output lines are prefixed `INFO  <Script>.java>`; batch addresses (30–60 s JVM start per run).
- IOP module bases at runtime: SIO2MAN 0x10000, MCMAN 0x11000, MCSERV 0x38400, SIO2D 0x3bc00, DBCMAN 0x3ea00,
  DS2O_D 0x43c00, LIBSD 0x46400, SDHD 0x4c100, CDVDSTM 0x4ef00, KOEISND 0x53b00 (Ghidra addresses are module-relative).
- Overlay import (e.g. OVL_00.BIN): Raw Binary, `r5900:LE:32:default`, base 0x61A800, file offset 0 (MWo3 header
  0x61A800–0x61A87F); no auto-analysis; then `ImportFunctionList.java` with
  `recomp/dw5xl/overlays/ovl00/ghidra_functions.code.csv`.
- Re-create the EE project: `analyzeHeadless ghidra dw5xl -import game/dw5xl/SLES_538.60 -overwrite -processor
  r5900:LE:32:default -scriptPath tools/ghidra_scripts -postScript ExportPS2Functions.java <abs>/recomp/dw5xl/ghidra.toml
  <abs>/recomp/dw5xl/ghidra_functions.csv`, then `tools/fix_ghidra_csv.py ghidra_functions.csv
  ghidra_functions.code.csv 310CE8`.
- Quick look at one function: the generated `recomp/dw5xl/output/*_0x<addr>.cpp` comments every instruction.

### Pitfalls (details in notes/BRINGUP.md and the issue work logs)
- Keep `[patches]` in `dw5xl.toml` empty: `ps2_analyzer` falsely flags ordinary stores as self-modifying code.
- Runtime SDK HLE stubs often differ from the real library (libgraph clears, libvu0 matrix order, libvif tags): prefer
  unbinding them so the game's own code runs; HLE only what touches hardware.
- VIF1 DIRECT carries only its own payload; GIF PATH2 state continues IMAGE transfers across DIRECTs.
- VU0 macro ops are generated by `vu_translator.cpp`, separately from the (better tested) VU1 interpreter: when
  culling/visibility looks wrong on the EE side, compare the op with the VU1 version (VCLIP bug, issue 005).
- IOP RPC server functions run outside any IOP thread; blocking waits there need the host-wait pump (issue 004).
- Never measure stutter with frame dumps on (PNG dumps cause 40 ms hitches); the F2 overlay/[hud] VSync figure reads 48–51
  by aliasing even when pacing is perfect — use late fields and game FPS.
- Lifted VU1 code must stay cycle-exact with the interpreter (Q/flag/XGKICK timing is observable by the game).
