<#
.SYNOPSIS
    Configure and build the DW5XL port on Windows (clang-cl, Ninja, lld-link).

.DESCRIPTION
    Mirrors the Linux build: Release, LLD, Vulkan paraLLEl-GS, aggressive logs
    off, x86-64-v3. Imports the Visual Studio x64 developer environment when it
    is not already active, checks that the locally generated code and the
    dependency sources are present, then configures and builds app\ into
    -BuildDir.

    This script never downloads or copies game files. CMake downloads the
    prebuilt FFmpeg archive during the build unless -FfmpegArchive points at a
    local copy, and fetches raylib/imgui/rlImGui with git only when app\deps
    has no local copies.

    Windows PowerShell 5.1 and PowerShell 7 are both supported. If script
    execution is blocked, run:
      powershell -ExecutionPolicy Bypass -File tools\windows\build.ps1

.EXAMPLE
    tools\windows\build.ps1
    tools\windows\build.ps1 -Tests
    tools\windows\build.ps1 -FfmpegArchive C:\Downloads\ffmpeg-n7.1-241205-lgpl-amd64-shared.zip
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "app\build-windows",
    [ValidateSet("Release", "RelWithDebInfo", "Debug")]
    [string]$Config = "Release",
    # clang-cl matches the Linux clang build; msvc (cl.exe) is untested.
    [ValidateSet("clang-cl", "msvc")]
    [string]$Compiler = "clang-cl",
    # Folder holding imgui-src, raylib-src, rlimgui-src and parallel-gs.
    [string]$DepsDir = "app\deps",
    # Optional local copy of the FFmpeg archive CMake would otherwise download.
    [string]$FfmpegArchive = "",
    [switch]$Tests,
    [int]$Jobs = 0,
    [switch]$ConfigureOnly
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root

function Fail([string]$Message) {
    Write-Host "error: $Message" -ForegroundColor Red
    exit 1
}

if ($Root -match '[^\x20-\x7E]') {
    Write-Warning "The checkout path contains non-ASCII characters. The runtime converts paths through the ANSI code page; use a short ASCII path such as C:\src\dw5xl."
}
if ($Root.Length -gt 60) {
    Write-Warning "Long checkout path ($($Root.Length) characters). Build object paths can exceed the 260-character Windows limit; prefer a short path such as C:\src\dw5xl."
}
try {
    if (-not [System.Runtime.Intrinsics.X86.Avx2]::IsSupported) {
        Write-Warning "This CPU does not report AVX2. The build targets x86-64-v3 like Linux; the game will not start on this CPU."
    }
} catch {
    # Windows PowerShell 5.1 (.NET Framework) cannot query this; skip the check.
}

function Find-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        Fail "vswhere.exe not found. Install Visual Studio 2022 or Build Tools for Visual Studio 2022 with 'Desktop development with C++'."
    }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $path) {
        Fail "No Visual Studio installation with the x64 C++ tools (MSVC v143) was found."
    }
    return $path
}

# 1. Visual Studio x64 environment (Windows SDK, MSVC STL/CRT libraries, bundled CMake/Ninja).
$vsPath = $null
if ($env:VSCMD_ARG_TGT_ARCH -ne "x64") {
    $vsPath = Find-VisualStudio
    $devShell = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    if (-not (Test-Path $devShell)) { Fail "Missing $devShell" }
    Import-Module $devShell
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
    Set-Location $Root
} elseif ($env:VSINSTALLDIR) {
    $vsPath = $env:VSINSTALLDIR.TrimEnd('\')
}

# 2. Tools.
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Fail "cmake not found (install CMake 3.29+ or the Visual Studio 'C++ CMake tools for Windows' component)."
}
$cmakeVersionLine = & cmake --version | Select-Object -First 1
if ($cmakeVersionLine -notmatch '(\d+)\.(\d+)') { Fail "Cannot read the CMake version from '$cmakeVersionLine'." }
if ([int]$Matches[1] -lt 3 -or ([int]$Matches[1] -eq 3 -and [int]$Matches[2] -lt 29)) {
    Fail "$cmakeVersionLine is too old; CMake 3.29 or newer is needed (CMAKE_LINKER_TYPE)."
}
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    Fail "ninja not found (install Ninja or the Visual Studio 'C++ CMake tools for Windows' component)."
}

$cc = "cl.exe"
$llvmLib = $null
if ($Compiler -eq "clang-cl") {
    $candidates = @()
    $onPath = Get-Command clang-cl -ErrorAction SilentlyContinue
    if ($onPath) { $candidates += $onPath.Source }
    if ($vsPath) { $candidates += (Join-Path $vsPath "VC\Tools\Llvm\x64\bin\clang-cl.exe") }
    $candidates += (Join-Path $env:ProgramFiles "LLVM\bin\clang-cl.exe")
    $cc = $candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if (-not $cc) {
        Fail "clang-cl not found. Install the Visual Studio component 'C++ Clang Compiler for Windows' or LLVM for Windows."
    }
    $llvmBin = Split-Path $cc
    foreach ($tool in @("lld-link.exe", "llvm-lib.exe")) {
        if (-not (Test-Path (Join-Path $llvmBin $tool))) { Fail "$tool is missing next to $cc" }
    }
    $llvmLib = Join-Path $llvmBin "llvm-lib.exe"
    Write-Host "Compiler: $cc"
    & $cc --version | Select-Object -First 1
} else {
    Write-Warning "cl.exe builds are untested and not bit-identical with the Linux clang build."
}

# 3. Inputs that are not in Git.
$generated = @(
    "recomp\dw5xl\output\ps2_recompiled_functions.h",
    "recomp\dw5xl\output\register_functions.cpp",
    "recomp\dw5xl\overlays\ovl00\output\register_functions.cpp",
    "recomp\dw5xl\overlays\ovl01\output\register_functions.cpp",
    "recomp\dw5xl\overlays\ovl02\output\register_functions.cpp"
)
foreach ($file in $generated) {
    if (-not (Test-Path $file)) {
        Fail "Missing $file. Generate the code on Linux (tools/regen.sh all), pack it with tools/windows/pack-inputs.sh and extract it here; see tools\windows\README.md."
    }
}

$deps = Resolve-Path $DepsDir -ErrorAction SilentlyContinue
if (-not $deps) { Fail "$DepsDir not found. Extract the archive from tools/windows/pack-inputs.sh in the checkout root." }
$stamp = Join-Path $deps.Path "WINDOWS_INPUTS.txt"
if (Test-Path $stamp) {
    Write-Host "Inputs packed on Linux:"
    Get-Content $stamp | ForEach-Object { Write-Host "  $_" }
}
$parallelGs = Join-Path $deps.Path "parallel-gs"
if (-not (Test-Path (Join-Path $parallelGs "gs\gs_interface.cpp")) -or
    -not (Test-Path (Join-Path $parallelGs "Granite\third_party\volk\volk.c"))) {
    Fail "paraLLEl-GS sources not found in $parallelGs (pinned checkout with Granite, volk and Vulkan-Headers)."
}

$cmakeArgs = @(
    "-S", "app", "-B", $BuildDir, "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DCMAKE_C_COMPILER=$($cc -replace '\\', '/')", "-DCMAKE_CXX_COMPILER=$($cc -replace '\\', '/')",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`$<`$<CONFIG:Debug>:Debug>DLL",
    "-DPS2X_ENABLE_VULKAN_GS=ON",
    "-DPS2X_PARALLEL_GS_SOURCE_DIR=$($parallelGs -replace '\\', '/')",
    "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF"
)
if ($Compiler -eq "clang-cl") {
    # lld-link and llvm-lib are required: release objects are ThinLTO bitcode.
    $cmakeArgs += "-DCMAKE_LINKER_TYPE=LLD"
    $cmakeArgs += "-DCMAKE_AR=$($llvmLib -replace '\\', '/')"
}
if ($Tests) { $cmakeArgs += "-DDW5XL_BUILD_TESTS=ON" } else { $cmakeArgs += "-DDW5XL_BUILD_TESTS=OFF" }

$localSources = [ordered]@{ "IMGUI" = "imgui-src"; "RAYLIB" = "raylib-src"; "RLIMGUI" = "rlimgui-src" }
$haveAllSources = $true
foreach ($name in $localSources.Keys) {
    $dir = Join-Path $deps.Path $localSources[$name]
    if (Test-Path $dir) {
        $cmakeArgs += "-DFETCHCONTENT_SOURCE_DIR_$name=$($dir -replace '\\', '/')"
    } else {
        $haveAllSources = $false
        Write-Warning "$dir not found; CMake will clone it from GitHub (needs git and internet)."
    }
}
if ($haveAllSources) { $cmakeArgs += "-DFETCHCONTENT_FULLY_DISCONNECTED=ON" }

if ($FfmpegArchive) {
    if (-not (Test-Path $FfmpegArchive)) { Fail "FFmpeg archive not found: $FfmpegArchive" }
    $zip = (Resolve-Path $FfmpegArchive).Path -replace '\\', '/'
    $cmakeArgs += "-DPS2X_FFMPEG_PREBUILT_URL=$zip"
} else {
    Write-Host "Note: the first build downloads the prebuilt FFmpeg archive (System233/ffmpeg-msvc-prebuilt n7.1-241205, LGPL, shared, amd64) from GitHub."
}

# 4. Configure and build.
Write-Host "cmake $($cmakeArgs -join ' ')"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed." }
if ($ConfigureOnly) { exit 0 }

$buildArgs = @("--build", $BuildDir)
if (-not $Tests) { $buildArgs += @("--target", "dw5xl") }
if ($Jobs -gt 0) { $buildArgs += @("-j", "$Jobs") }
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { Fail "Build failed." }

$exe = Join-Path $BuildDir "dw5xl.exe"
Write-Host "Built $exe" -ForegroundColor Green

if ($Tests) {
    # vu1_lift reads VU1 census dumps from notes\evidence relative to the
    # working directory (local notes, not in Git). ctest runs in the build
    # directory, so it is excluded there and run from the checkout root when
    # those notes exist.
    & ctest --test-dir $BuildDir --output-on-failure -E "^vu1_lift$"
    if ($LASTEXITCODE -ne 0) { Fail "Tests failed." }
    if (Test-Path "notes\evidence") {
        & (Join-Path $BuildDir "dw5xl_vu1_lift_test.exe")
        if ($LASTEXITCODE -ne 0) { Fail "vu1_lift test failed." }
    } else {
        Write-Warning "notes\evidence not present: vu1_lift test skipped."
    }
}
