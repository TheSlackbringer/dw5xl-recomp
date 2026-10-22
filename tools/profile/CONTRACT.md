# Profiling tools — interface contract

A debugging tool for developers (issue 025): not part of CI or of the release packages. The project's profiling
acceptance test checks exactly this contract. All times are steady_clock nanoseconds (the clock of the game
markers and of the fixture's `truth.json`).

## Files

| Path | Role |
|---|---|
| `capture.py` | One command: checks, switch on (Windows), record, run the target as its own child, stop, switch off, export, convert, symbolize, report |
| `profiling.py` | `status [--json]`, `off`; Windows test hooks under `test …` (below) |
| `etw_samples.py` / `perf_samples.py` | Recorder export -> `samples.jsonl` |
| `report.py` | `samples.jsonl` + markers -> `report.json` (+ readable `report.txt`) |
| `fixture/profile_fixture.cpp` | Known-behaviour test program |
| `windows/` | Setup guide, recording profile, protected script, filter, helpers (copied to the protected folder by setup) |

## `capture.py`

```
capture.py --target <exe> --out <dir> [--limit-s N] [--label TEXT] [--cancel-after S] [--fail-after-start]
           [--also <exe> [--also-args "<args>"]]... [-- <target args>]
```
- Runs `<exe>` (plus each `--also` program, concurrently) under one recording; `<target args>` are passed through.
  `--also-args` is one string (split like a shell command line) for the `--also` before it.
- `--limit-s`: recording time limit for this capture (never above the fixed 480 s).
- `--cancel-after S`: test hook, cancel the recording S seconds after it started (exit code 3).
- `--fail-after-start`: test hook, raise an error right after the recording started (exit code 4).
- Exit 0 on success. On every exit path the Windows switch is off afterwards.
- Writes into `<dir>`: `manifest.json`, `samples.jsonl`, `report.json`, `report.txt`, `capture.log`, and the
  target's own outputs when the target writes into `<dir>` (the fixture does, with `--out <dir>`).

## `manifest.json`
```
{"schema": 1, "os": "windows|linux", "recorder": {"name": "wpr|perf", "version": str, "settings": {...}},
 "target": {"path": str, "sha256": str, "symbols": "pdb|dwarf|none"},
 "anchors": [{"steady_ns": int, "os_ticks": int, "os_freq": int}, ...],
 "recording_limit_s": number, "recording_s": number, "bytes_written": int, "output_cap_bytes": int,
 "sample_interval_ns": int, "samples": int, "lost_samples": int,
 "switched_off_after": true|false|null (null on Linux), "result": "ok|cancelled|error", "error": str|null}
```

## `report.json`
```
{"schema": 1, "sample_interval_ns": int,
 "threads": [{"name": "<thread name>", "process": "<exe file name>", "pid": int, "tid": int, "samples": int,
              "self": [["<function>", samples], ...],     // descending
              "total": [["<function>", samples], ...],    // descending, inclusive (function anywhere on the stack)
              "waits": [{"start_ns": int, "end_ns": int, "waker_thread": str|null, "waker_process": str|null,
                         "waker_stack": ["<function>", ...]}]}],   // one entry per OS thread (names can repeat)
 "spans": [{"pid": int, "tid": int, "function": str, "start_ns": int, "end_ns": int}],  // first/last own-time sample of
                                         // each run of samples; a gap > 5 sample intervals starts a new span
 "markers": [{"event": int, "seq": int, "tid": int, "t_ns": int}],
 "processes_with_samples": ["<exe file name>", ...],
 "switch_partner_processes": ["<exe file name>", ...]}
```
Function names are demangled base names (no parameter lists), e.g. `fixture_hot_spin`.

## `profiling.py` (Windows)
- `status --json` -> `{"recorder": "wpr", "set_up": bool, "switch": "on|off", "switch_end": str|null,
  "recording": bool, "recordings_last_hour": int, "rate_limit_per_hour": 30, "next_slot_in_s": number,
  "free_gb": number, "min_free_gb": 20, "raw_kept": [{"time": "<ISO 8601>", "bytes": int}], "exports_kept": [str],
  "unexplained": [{"time": str, ...}], "audit_log": "<path>"}`. Recordings are deleted after use: the raw recording right after the export, the export by `profiling.clean()` (the protected cleanup
  task) when `capture.py` has converted it, so both lists are normally empty. Other programs appear only as
  `other (0)`. One capture, including its conversion, at a time. `recordings_last_hour` counts accepted
  recordings only; `next_slot_in_s` is 0 while a recording is allowed now.
- Audit log: JSON lines `{"time": "<ISO 8601>", "accepted": bool, "reason": "ok|off|expired|malformed|rate limit|low disk|busy",
  "duration_s": number|null, "events_kept": int|null}`, appended by the protected script only.
- `off`: switch off and cancel any running recording (waits until the recording task has stopped).
- Switch file `C:\ProgramData\DW5XL-Profiling-switch\switch.txt` (not under AppData: packaged apps see a private
  copy of AppData): exactly one UTC end time `YYYY-MM-DDTHH:MM:SSZ`;
  missing = off, past = expired, anything else = malformed. Stop flag `stop.flag` there: `stop` (save) or
  `cancel` (discard), counted only when written after the recording started.
- Test hooks (used only by the acceptance test): `test raw-request` (start the task without switching on, like a
  direct call), `test raw-switch <text>` (write the switch file verbatim), `test raw-request --switched` (switch on
  and start the task outside `capture.py`, so no capture manifest explains it), `test decide --free-gb N --json`
(runs the protected script's decision without recording, as if switched on and within the rate limit, with N GB
free; prints `{"accepted": bool, "reason": str}`).
On Linux `status --json` returns `{"recorder": "perf", "set_up": bool, ...}`; the switch and task do not exist (see
"Linux" below).

## Linux

- **Recorder:** `perf record` as the user, without extra rights. It records only its own child, `perf_launch.py`,
  and the programs that one starts (the target and every `--also`), so an unrelated program is never recorded. The recording starts disabled (`-D -1`); `capture.py` sends `enable` on perf's control
  fifo just before the programs start and `disable` at the time limit (the programs keep running; perf ends with
  them). Settings (manifest `recorder.settings`): `cpu-clock`, `cpu-clock:u` when `perf_event_paranoid` is 2,
  period 1 ms; `--call-graph fp --user-callchains` (user-space stacks only: the kernel refuses unprivileged call
  stacks otherwise; a sample in a system call is charged to the user function that made it); `-k CLOCK_MONOTONIC` (= steady_clock on Linux); `--switch-events`;
  `sched:sched_waking` (the waker's stack, unwound with DWARF: an 8 KB stack copy per wake-up, because the system
  C++ library has no frame pointers) only when tracefs is readable and paranoid <= -1 or perf has CAP_PERFMON; `--max-size` 8 GiB; `-N` (no copies in `~/.debug`).
- **No switch, scheduled task, audit log or rate limit:** nothing runs with more rights than the user's. Bounds
  that still apply: 480 s limit, 8 GiB output cap, refused below 20 GB free on the output drive (exit 6), one
  recording at a time (exit 2 while any `perf record` runs).
- **Outputs:** as on Windows, plus `perf.data` (the raw recording; deleted when cancelled, or when the recording
  failed before it finished), `perf-record.log`, `perf-script.log`, `launch-spec.json`, `launch-state.json`,
  `target.log`. Manifest: `switched_off_after` null; `raw_bytes` = perf.data; `bytes_written` = samples.jsonl
  (both within `output_cap_bytes`); `lost_samples` from perf's lost-event records; `recorder.command`.
- **samples.jsonl (perf_samples.py from `perf script`):** a stack frame may be a **string** (the name perf
  resolved; `[kernel]`; `<module file>` when unnamed) instead of an address; `report.py` only normalizes such
  frames (Windows unchanged). Switch records are one-sided: OUT has `old_*` (`old_state` "Waiting" when the thread
  went to sleep, "Ready" when preempted) and null `new_*`; IN has `new_*` and null `old_*`. `ready` comes from
  `sched:sched_waking`: `pid`/`tid` = waker (its stack), `rdy_tid` = the woken thread. Process names are the
  executable's file name from the exec mapping (perf's comm is cut to 15 characters); thread names are comm
  (`pthread_setname_np`). Only allowed programs (`dw5xl`, `dw5xl_profile_fixture`) are kept.
- **`profiling.py status --json` (Linux):** `{"recorder": "perf", "set_up": bool, "perf": str|null,
  "perf_version": str|null, "perf_event_paranoid": int, "kernel_frames": bool, "wakeups": bool, "switch": null,
  "recording": bool, "recording_limit_s": 480, "output_cap_bytes": int, "free_gb": number, "min_free_gb": 20}`.
  `off` and the test hooks do nothing on Linux.
