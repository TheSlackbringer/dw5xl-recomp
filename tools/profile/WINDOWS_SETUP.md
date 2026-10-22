# Windows profiling setup (debugging tool)

What it is: Windows' own recorder (WPR) records CPU samples, thread switches and wake-ups, each with call stacks,
without pausing the game's threads. Windows only lets administrators start such a recording, so a one-time setup
creates a locked-down scheduled task that `capture.py` can start without a UAC prompt. Developers only: nothing here
is part of the game, CI or the release packages. Interface: `CONTRACT.md`.

## Set up (once, one UAC click)

Needs the Windows Performance Toolkit (from the Windows ADK; `xperf.exe` in its default folder) and an administrator
account. From the repository root:

```
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File <repo>\tools\profile\windows\setup.ps1'"
```

It creates:

| Where | What | Who can change it |
|---|---|---|
| `C:\ProgramData\DW5XL-Profiling\bin` | recording script, filter, recording profile, cancel/remove scripts, fixed config | administrators only (you can read) |
| `C:\ProgramData\DW5XL-Profiling\raw` | the raw recording (whole PC) only while it is exported; deleted right after | administrators only (you cannot read) |
| `C:\ProgramData\DW5XL-Profiling\out` | the game-only export until `capture.py` has converted it (then deleted), audit log | administrators only (you can read) |
| `C:\ProgramData\DW5XL-Profiling\state` | rate counter, lock, temporary files | administrators only |
| `C:\ProgramData\DW5XL-Profiling-switch` | switch, stop flag, list of captures started through `capture.py` (not under AppData: packaged apps, such as the Claude desktop app, see a private copy of AppData) | you (and administrators) |
| Task Scheduler `\DW5XL\Profile Record`, `\DW5XL\Profile Cancel` | on-demand tasks, highest privileges, no window, **no trigger** | administrators only |

Nothing runs at boot or in the background. Recording happens only while `capture.py` runs: it switches on just
before, off right after. A forgotten switch ends by itself (end time = recording limit + 2 minutes).

## Limits (fixed in the protected config)

- One recording at a time, at most 480 s; at most 30 per hour; none with less than 20 GB free.
- Only the programs `dw5xl.exe` and `dw5xl_profile_fixture.exe` leave the admin folder; other programs appear only as
  "other" (no name), where they interrupt or wake ours.
- Recordings are deleted after use: the raw recording right after the export, the export right after conversion.
- The recording profile has no file, disk, registry or network events.
- Every request (accepted or refused) is in `out\audit.jsonl`; a recording not started through `capture.py` is
  listed by `profiling.py status` as "unexplained".

## Remove

```
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File C:\ProgramData\DW5XL-Profiling\bin\remove.ps1'"
```

Or delete the `DW5XL` folder in Task Scheduler and `C:\ProgramData\DW5XL-Profiling` by hand.

## Why it is safe enough (and its limit)

The tasks run one fixed script from a folder only administrators can change and take no input; the script reads only
a strictly formatted switch timestamp and a stop flag from your folder, never deletes or writes there, loads
PowerShell modules only from Windows' own folder and keeps its temporary files in the protected folder. A harmful
program running as you could switch recording on, but would get only game data, at most 30 times an hour, and
every attempt is logged.
