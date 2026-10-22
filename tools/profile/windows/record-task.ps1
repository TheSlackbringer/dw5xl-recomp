# Protected recording script (tools/profile). Setup copies it to C:\ProgramData\DW5XL-Profiling\bin, which only
# administrators can change; the scheduled task "\DW5XL\Profile Record" runs it with highest privileges. It takes no
# input from the caller: the only things it reads from the user's account are the switch file (one strict UTC
# timestamp) and the stop flag (existence and the word "cancel"). Interface: CONTRACT.md.
#
# One run: decide (busy / switched off / expired / malformed / rate limit / low disk) -> record with WPR (at most the
# fixed time limit, until the stop flag or the switch ends) -> export with xperf, keeping only the allowed programs'
# events (other programs only as "other" where they meet ours) -> delete the raw recording -> audit log. The export is
# deleted by the cleanup task once capture.py has converted it (recordings are deleted after use).
# -DecideOnly -SimulatedFreeGb N [-AuditPath file]: print the decision for N GB free, as if switched on, with the
# recordings of the last hour counted from the given audit log (none if not given); changes nothing (used by the tests,
# runs without admin rights).
param([switch]$DecideOnly, [double]$SimulatedFreeGb = -1, [string]$AuditPath = '')
# Running as administrator: load modules only from Windows' own folder (never from the user's Documents), before any
# command can trigger module loading.
$env:PSModulePath = "$PSHOME\Modules"
# Not 'Stop': in Windows PowerShell 5.1 a native tool's stderr would end the script; exit codes are checked instead.
$ErrorActionPreference = 'Continue'
$bin = $PSScriptRoot
$base = Split-Path $bin -Parent
$cfg = Get-Content -Raw -Path (Join-Path $bin 'config.json') | ConvertFrom-Json
$out = Join-Path $base 'out'
$raw = Join-Path $base 'raw'
$state = Join-Path $base 'state'
$switchFile = Join-Path $cfg.user_dir 'switch.txt'
$stopFile = Join-Path $cfg.user_dir 'stop.flag'
$utf8 = New-Object System.Text.UTF8Encoding($false)
if (-not $DecideOnly) {
    # Compiler and tool temporary files go to the protected folder, not the user's (changeable) TEMP.
    $tmp = Join-Path $state 'tmp'
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $env:TEMP = $tmp
    $env:TMP = $tmp
}

function NowIso { (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') }

function AppendLine([string]$path, [string]$line) { [System.IO.File]::AppendAllText($path, $line + "`n", $utf8) }

# -InputObject, not the pipeline: piping an array to ConvertTo-Json unrolls it (one element would become a scalar).
function WriteJson([string]$path, $obj) {
    [System.IO.File]::WriteAllText("$path.tmp", (ConvertTo-Json -InputObject $obj -Compress -Depth 5), $utf8)
    Move-Item -Force -Path "$path.tmp" -Destination $path
}

function Audit($accepted, [string]$reason, $duration, $kept, $stamp) {
    AppendLine (Join-Path $out 'audit.jsonl') (@{ time = (NowIso); accepted = $accepted; reason = $reason;
        duration_s = $duration; events_kept = $kept; stamp = $stamp } | ConvertTo-Json -Compress)
}

# Switch: exactly one timestamp "yyyy-MM-ddTHH:mm:ssZ"; returns 'off', 'malformed', 'expired' or 'ok'.
function SwitchState {
    if (-not (Test-Path -LiteralPath $switchFile)) { return 'off' }
    $text = ([System.IO.File]::ReadAllText($switchFile)).Trim()
    if ($text -notmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$') { return 'malformed' }
    $end = [DateTime]::MinValue
    if (-not [DateTime]::TryParseExact($text, 'yyyy-MM-ddTHH:mm:ssZ', [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AdjustToUniversal -bor [Globalization.DateTimeStyles]::AssumeUniversal, [ref]$end)) {
        return 'malformed'
    }
    if ($end -lt (Get-Date).ToUniversalTime()) { return 'expired' }
    return 'ok'
}

# Accepted recordings in the last hour, counted from the audit log: the same count profiling.py shows. (The first
# version kept its own list, which Windows PowerShell nested one level deeper on every save, so it undercounted and
# let one recording too many through.)
function RecentRecordings([string]$path) {
    $cut = (Get-Date).ToUniversalTime().AddHours(-1)
    $n = 0
    if ($path -and (Test-Path -LiteralPath $path)) {
        foreach ($line in [System.IO.File]::ReadAllLines($path)) {
            if (-not $line.Trim()) { continue }
            $e = ConvertFrom-Json -InputObject $line
            if ($e.accepted -ne $true) { continue }
            $d = [DateTime]::MinValue
            if ([DateTime]::TryParseExact([string]$e.time, 'yyyy-MM-ddTHH:mm:ssZ', [Globalization.CultureInfo]::InvariantCulture,
                    [Globalization.DateTimeStyles]::AdjustToUniversal -bor [Globalization.DateTimeStyles]::AssumeUniversal, [ref]$d) -and
                $d -ge $cut) { $n++ }
        }
    }
    return $n
}

function FreeGb {
    $drive = (Split-Path -Qualifier $base)
    $d = Get-CimInstance -ClassName Win32_LogicalDisk -Filter "DeviceID='$drive'"
    return [double]$d.FreeSpace / 1GB
}

function Busy {
    $lock = Join-Path $state 'running.json'
    if (-not (Test-Path -LiteralPath $lock)) { return $false }
    $info = Get-Content -Raw -Path $lock | ConvertFrom-Json
    return [bool](Get-Process -Id $info.pid -ErrorAction SilentlyContinue)
}

function Decide([bool]$busy, [string]$switchState, [int]$recent, [double]$freeGb) {
    if ($busy) { return 'busy' }
    if ($switchState -ne 'ok') { return $switchState }
    if ($recent -ge [int]$cfg.rate_limit_per_hour) { return 'rate limit' }
    if ($freeGb -lt [double]$cfg.min_free_gb) { return 'low disk' }
    return 'ok'
}

if ($DecideOnly) {
    $reason = Decide $false 'ok' (RecentRecordings $AuditPath) $SimulatedFreeGb
    (@{ accepted = ($reason -eq 'ok'); reason = $reason } | ConvertTo-Json -Compress)
    exit 0
}

$switchNow = SwitchState
$reason = Decide (Busy) $switchNow (RecentRecordings (Join-Path $out 'audit.jsonl')) (FreeGb)
# Diagnostics for every request (paths and the switch timestamp only): what this task can see of the user's folder.
$who = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$dirSeen = try { if (Test-Path -LiteralPath $cfg.user_dir) { 'visible' } else { 'NOT visible' } } catch { "error $($_.Exception.Message)" }
$switchRead = try { ([System.IO.File]::ReadAllText($switchFile)).Trim() } catch { "$($_.Exception.GetType().Name): $($_.Exception.Message)" }
AppendLine (Join-Path $out 'task.log') "$(NowIso) run as $who; user_dir $($cfg.user_dir) $dirSeen; switch: $switchRead -> $switchNow; decision: $reason"
if ($reason -ne 'ok') {
    Audit $false $reason $null $null $null
    if ($reason -ne 'busy') { WriteJson (Join-Path $out 'current.json') @{ stamp = $null; state = 'refused'; reason = $reason; time = (NowIso) } }
    exit 0
}

# ---- accepted: record ----
# Leftovers of an earlier capture (one runs at a time, so it has finished) go before a new recording starts.
Get-ChildItem -Path $out -Directory | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
Get-ChildItem -Path $raw | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
$dir = Join-Path $out $stamp
New-Item -ItemType Directory -Force -Path $dir | Out-Null
WriteJson (Join-Path $state 'running.json') @{ pid = $PID; stamp = $stamp }
# Nothing in the user's folder is ever deleted or written from here (a link there could point elsewhere): a stop
# flag counts only when written after this recording started.
$wpr = Join-Path $env:WINDIR 'System32\wpr.exe'
$etl = Join-Path $raw "$stamp.etl"
& $wpr -cancel 2>&1 | Out-Null
& $wpr -start ((Join-Path $bin 'dw5xl.wprp') + '!DW5XL') -filemode -recordtempto $raw 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Audit $false 'recorder error' $null $null $stamp
    WriteJson (Join-Path $out 'current.json') @{ stamp = $stamp; state = 'error'; reason = "wpr -start exit $LASTEXITCODE"; time = (NowIso) }
    Remove-Item -Recurse -Force -LiteralPath $dir -ErrorAction SilentlyContinue
    Remove-Item -Force -LiteralPath (Join-Path $state 'running.json') -ErrorAction SilentlyContinue
    exit 0
}
$started = Get-Date
WriteJson (Join-Path $out 'current.json') @{ stamp = $stamp; state = 'recording'; reason = 'ok'; time = (NowIso) }
$cancel = $false
while ($true) {
    Start-Sleep -Milliseconds 200
    if ((Test-Path -LiteralPath $stopFile) -and (Get-Item -LiteralPath $stopFile).LastWriteTimeUtc -gt $started.ToUniversalTime()) {
        $cancel = (([System.IO.File]::ReadAllText($stopFile)).Trim() -eq 'cancel')
        break
    }
    if ((SwitchState) -ne 'ok') { $cancel = $true; break }
    if (((Get-Date) - $started).TotalSeconds -ge [double]$cfg.recording_limit_s) { break }
}
$duration = [math]::Round(((Get-Date) - $started).TotalSeconds, 3)
if ($cancel) {
    & $wpr -cancel 2>&1 | Out-Null
    # Nothing is left of a cancelled recording (its export folder was made at the start).
    Remove-Item -Recurse -Force -LiteralPath $dir -ErrorAction SilentlyContinue
    Get-ChildItem -Path $raw | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    Audit $true 'ok' $duration 0 $stamp
    WriteJson (Join-Path $out 'current.json') @{ stamp = $stamp; state = 'cancelled'; reason = 'cancelled'; time = (NowIso); recording_s = $duration }
    Remove-Item -Force -LiteralPath (Join-Path $state 'running.json') -ErrorAction SilentlyContinue
    exit 0
}
WriteJson (Join-Path $out 'current.json') @{ stamp = $stamp; state = 'exporting'; reason = 'ok'; time = (NowIso); recording_s = $duration }
& $wpr -stop $etl 2>&1 | Out-Null

# ---- export: only the allowed programs' events leave the admin folder ----
Add-Type -TypeDefinition (Get-Content -Raw -Path (Join-Path $bin 'Dw5xlFilter.cs')) -Language CSharp
$events = Join-Path $dir 'events.txt'
$kept = [Dw5xlFilter]::Run($cfg.xperf, $etl, $events, [string[]]$cfg.allowed_programs, [long]$cfg.output_cap_bytes)
[Dw5xlFilter]::Anchors($cfg.xperf, $etl, (Join-Path $dir 'anchors.txt'), 64) | Out-Null
$bytes = (Get-Item -LiteralPath $events).Length
$etlBytes = if (Test-Path -LiteralPath $etl) { (Get-Item -LiteralPath $etl).Length } else { 0 }

# ---- the raw (whole-PC) recording is deleted as soon as the game-only export exists ----
Get-ChildItem -Path $raw | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
WriteJson (Join-Path $out 'raw-index.json') @(Get-ChildItem -Path $raw -Filter '*.etl' | ForEach-Object {
        @{ time = $_.LastWriteTimeUtc.ToString('yyyy-MM-ddTHH:mm:ssZ'); bytes = $_.Length } })

WriteJson (Join-Path $dir 'done.json') @{ stamp = $stamp; recording_s = $duration; etl_bytes = $etlBytes;
    events_kept = $kept; bytes_written = $bytes; output_cap_bytes = [long]$cfg.output_cap_bytes; truncated = ($kept -lt 0) }
Audit $true 'ok' $duration $kept $stamp
WriteJson (Join-Path $out 'current.json') @{ stamp = $stamp; state = 'done'; reason = 'ok'; time = (NowIso); recording_s = $duration }
Remove-Item -Force -LiteralPath (Join-Path $state 'running.json') -ErrorAction SilentlyContinue
