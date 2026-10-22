# Protected cancel-and-clean script (tools/profile), run by the task "\DW5XL\Profile Cancel": stops any WPR
# recording without saving it, clears the lock of a recording task that no longer runs, and deletes every raw
# recording and every export (recordings are deleted after use; capture.py runs it after converting).
# Takes no input.
$env:PSModulePath = "$PSHOME\Modules"
$ErrorActionPreference = 'Continue'
$base = Split-Path $PSScriptRoot -Parent
$state = Join-Path $base 'state'
$out = Join-Path $base 'out'
$utf8 = New-Object System.Text.UTF8Encoding($false)
& (Join-Path $env:WINDIR 'System32\wpr.exe') -cancel 2>&1 | Out-Null
$raw = Join-Path $base 'raw'
$running = $false
$lock0 = Join-Path $state 'running.json'
if (Test-Path -LiteralPath $lock0) {
    $info0 = Get-Content -Raw -Path $lock0 | ConvertFrom-Json
    $running = [bool](Get-Process -Id $info0.pid -ErrorAction SilentlyContinue)
}
if (-not $running) {
    Get-ChildItem -Path $raw | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    Get-ChildItem -Path $out -Directory | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    [System.IO.File]::WriteAllText((Join-Path $out 'raw-index.json'), '[]', $utf8)
}
$lock = Join-Path $state 'running.json'
if (Test-Path -LiteralPath $lock) {
    $info = Get-Content -Raw -Path $lock | ConvertFrom-Json
    if (-not (Get-Process -Id $info.pid -ErrorAction SilentlyContinue)) {
        Remove-Item -Force -LiteralPath $lock
        $now = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        [System.IO.File]::WriteAllText((Join-Path $out 'current.json'),
            (@{ stamp = $info.stamp; state = 'cancelled'; reason = 'cancel task'; time = $now } | ConvertTo-Json -Compress), $utf8)
    }
}
