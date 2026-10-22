# Removes the profiling setup completely (tools/profile). Run as administrator (one UAC click): cancels any
# recording, deletes both tasks and the Task Scheduler folder \DW5XL\, and deletes C:\ProgramData\DW5XL-Profiling with
# all raw recordings. The user's own switch folder is left for the user (it holds no recordings).
$env:PSModulePath = "$PSHOME\Modules"
$ErrorActionPreference = 'Continue'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error 'Run this as administrator.'
    exit 1
}
& (Join-Path $env:WINDIR 'System32\wpr.exe') -cancel 2>&1 | Out-Null
foreach ($name in @('Profile Record', 'Profile Cancel')) {
    Unregister-ScheduledTask -TaskPath '\DW5XL\' -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
}
try {
    $svc = New-Object -ComObject Schedule.Service
    $svc.Connect()
    $svc.GetFolder('\').DeleteFolder('DW5XL', 0)
} catch { }
$base = Join-Path $env:ProgramData 'DW5XL-Profiling'
if (Test-Path -LiteralPath $base) { Remove-Item -Recurse -Force -LiteralPath $base }
'removed'
