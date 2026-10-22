# One-time setup for Windows profiling (tools/profile). Run once as administrator (one UAC click). It:
#  - creates C:\ProgramData\DW5XL-Profiling with bin\ and out\ (administrators change, the user's account reads),
#    raw\ and state\ (administrators and SYSTEM only);
#  - copies the recording script, filter, recording profile, cancel and remove scripts into bin\ and writes the fixed
#    configuration (allowed program names and limits);
#  - creates C:\ProgramData\DW5XL-Profiling-switch, where only the user writes the switch, stop flag and capture list;
#  - registers two on-demand tasks in Task Scheduler folder \DW5XL\ ("Profile Record", "Profile Cancel"), run with
#    highest privileges as the user without a visible window. They have no trigger: nothing runs at boot or in the
#    background; a task runs only when started (schtasks /run).
# Running it again updates everything. remove.ps1 (also copied to bin\) undoes it.
# The user's folder is not under AppData: packaged apps (e.g. the Claude desktop app and what it starts) see a private
# copy of AppData, so files written there never reach this task.
param([string]$UserDir = (Join-Path $env:ProgramData 'DW5XL-Profiling-switch'))
$env:PSModulePath = "$PSHOME\Modules"
$ErrorActionPreference = 'Continue'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error 'Run this as administrator.'
    exit 1
}
$src = $PSScriptRoot
$base = Join-Path $env:ProgramData 'DW5XL-Profiling'
$bin = Join-Path $base 'bin'
$out = Join-Path $base 'out'
$raw = Join-Path $base 'raw'
$state = Join-Path $base 'state'
foreach ($d in @($base, $bin, $out, $raw, $state, $UserDir)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
$log = Join-Path $out 'setup.log'
"setup $(Get-Date -Format o) by $($id.Name)" | Out-File -Encoding utf8 $log

# Access (SIDs, so it works in any Windows language): administrators and SYSTEM full control; users read bin\ and out\.
$admins = '*S-1-5-32-544'; $system = '*S-1-5-18'; $users = '*S-1-5-32-545'
& icacls $base /setowner $admins /T /C /Q | Out-File -Append -Encoding utf8 $log
& icacls $base /inheritance:r /grant:r "${admins}:(OI)(CI)F" "${system}:(OI)(CI)F" "${users}:(RX)" /Q | Out-File -Append -Encoding utf8 $log
foreach ($d in @($bin, $out)) {
    & icacls $d /inheritance:r /grant:r "${admins}:(OI)(CI)F" "${system}:(OI)(CI)F" "${users}:(OI)(CI)RX" /Q | Out-File -Append -Encoding utf8 $log
}
foreach ($d in @($raw, $state)) {
    & icacls $d /inheritance:r /grant:r "${admins}:(OI)(CI)F" "${system}:(OI)(CI)F" /Q | Out-File -Append -Encoding utf8 $log
}
# The user's folder: only the user (plus administrators and SYSTEM) may write the switch, stop flag and capture list;
# the elevated script only reads it.
& icacls $UserDir /inheritance:r /grant:r "${admins}:(OI)(CI)F" "${system}:(OI)(CI)F" "$($id.Name):(OI)(CI)M" /Q | Out-File -Append -Encoding utf8 $log

foreach ($f in @('record-task.ps1', 'cancel-task.ps1', 'remove.ps1', 'Dw5xlFilter.cs', 'dw5xl.wprp')) {
    Copy-Item -Force -Path (Join-Path $src $f) -Destination (Join-Path $bin $f)
}
$xperf = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Windows Performance Toolkit\xperf.exe'
$cfg = [ordered]@{
    allowed_programs = @('dw5xl.exe', 'dw5xl_profile_fixture.exe')
    recording_limit_s = 480
    rate_limit_per_hour = 30
    min_free_gb = 20
    raw_keep = 0
    output_cap_bytes = 8GB
    user_dir = $UserDir
    xperf = $xperf
}
[System.IO.File]::WriteAllText((Join-Path $bin 'config.json'), ($cfg | ConvertTo-Json), (New-Object System.Text.UTF8Encoding($false)))
Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath (Join-Path $state 'recordings.json')   # old rate list (replaced by the audit log)
# Recordings are deleted after use: nothing from earlier captures stays.
Get-ChildItem -Path $raw | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
Get-ChildItem -Path $out -Directory | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
[System.IO.File]::WriteAllText((Join-Path $out 'raw-index.json'), '[]', (New-Object System.Text.UTF8Encoding($false)))
foreach ($f in @('audit.jsonl')) { $p = Join-Path $out $f; if (-not (Test-Path $p)) { New-Item -ItemType File -Path $p | Out-Null } }
if (-not (Test-Path $xperf)) { "WARNING: xperf not found at $xperf" | Out-File -Append -Encoding utf8 $log }

$ps = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$principal = New-ScheduledTaskPrincipal -UserId $id.Name -LogonType S4U -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 45) `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
foreach ($t in @(@('Profile Record', 'record-task.ps1'), @('Profile Cancel', 'cancel-task.ps1'))) {
    $action = New-ScheduledTaskAction -Execute $ps -Argument "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$(Join-Path $bin $t[1])`""
    Register-ScheduledTask -TaskPath '\DW5XL\' -TaskName $t[0] -Action $action -Principal $principal -Settings $settings `
        -Description 'dw5xl-recomp issue 025 profiling: on demand only, see tools/profile/WINDOWS_SETUP.md' -Force |
        Out-File -Append -Encoding utf8 $log
}
Get-ScheduledTask -TaskPath '\DW5XL\' | Format-Table -AutoSize TaskName, State | Out-File -Append -Encoding utf8 $log
"setup done $(Get-Date -Format o)" | Out-File -Append -Encoding utf8 $log
