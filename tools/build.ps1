# Build an ESP32-S3 ESP-IDF project from PowerShell, without Git Bash.
#
# A thin wrapper around idf.py. Environment discovery - toolchains, IDF tree,
# python venv, IDF version, RT-Thread checkout - lives in env.ps1, which this
# dot-sources and which qemu.ps1 and qemu-gdb.ps1 share. It used to be copied
# into each of them, and the copies drifted: two still pinned tool version
# directories, and one composed a PATH that was missing a level.
#
# Configuration, all optional via environment variables:
#
#   IDF_PATH              ESP-IDF tree            (default: <repo>/../esp-idf-rtt)
#   IDF_PYTHON_ENV_PATH   ESP-IDF python venv     (default: newest under ESP_IDF_TOOLS)
#   ESP_IDF_TOOLS         .espressif/tools        (default: %USERPROFILE%\.espressif\tools)
#   RTTHREAD_ROOT         RT-Thread checkout      (default: <repo>/../rt-thread)
#   ESP_IDF_VERSION       IDF version string      (default: derived)
#
# Usage:
#   .\build.ps1                                   # build project/s3-rtt-test
#   .\build.ps1 -Project <path>
#   .\build.ps1 -Project <path> -IdfArgs fullclean,build
#   .\build.ps1 -Project <path> -NoPsram          # QEMU variant, into build-qemu/
#   .\build.ps1 -Project <path> -Qemu             # build, then boot-test in QEMU

param(
    [string]$Project = '',
    [string]$Target = 'esp32s3',
    [switch]$Qemu,
    [int]$QemuTimeoutSec = 0,
    # Build the QEMU variant: applies sdkconfig.nopsram over sdkconfig.defaults
    # and builds into build-qemu/, leaving build/ as the hardware image.
    [switch]$NoPsram,
    [string[]]$IdfArgs = @(),
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Rest = @()
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot

# ------------------------------------------------------------------ project --
if (-not $Project) {
    foreach ($candidate in @("$repo\project\s3-rtt-test", "$repo\s3-rtt-test")) {
        if (Test-Path (Join-Path $candidate 'CMakeLists.txt')) { $Project = $candidate; break }
    }
}
if (-not $Project) {
    throw "No ESP32-S3 project found. Pass -Project <path>."
}
$Project = (Resolve-Path $Project).Path
if (-not (Test-Path (Join-Path $Project 'CMakeLists.txt'))) {
    throw "'$Project' has no CMakeLists.txt"
}

# ------------------------------------------------------------- environment --
. (Join-Path $PSScriptRoot 'env.ps1')

Write-Host "IDF_PATH      : $env:IDF_PATH"
Write-Host "RTTHREAD_ROOT : $(if ($env:RTTHREAD_ROOT) { $env:RTTHREAD_ROOT } else { '(not set)' })"
Write-Host "Project       : $Project"
Write-Host "Target        : $Target"
Write-Host "Python        : $script:S3Python"
Write-Host ""

# --------------------------------------------------------------- sdkconfig --
#
# A generated `sdkconfig` records one machine's answers and outlives edits to
# sdkconfig.defaults. For the QEMU build that is actively wrong: it would carry
# the hardware image's PSRAM option into a target that has no PSRAM.

if ($NoPsram) {
    $overlayCfg = Join-Path $Project 'build-qemu\sdkconfig'
    $overlays = @(
        (Join-Path $Project 'sdkconfig.defaults'),
        (Join-Path $Project 'sdkconfig.nopsram')
    )
    if (Test-Path $overlayCfg) {
        $cfgTime = (Get-Item $overlayCfg).LastWriteTime
        $newer = $overlays | Where-Object { (Test-Path $_) -and (Get-Item $_).LastWriteTime -gt $cfgTime }
        if ($newer) { Remove-Item $overlayCfg -Force }
    }
}

# ------------------------------------------------------------------- build --

$argv = @()
if ($IdfArgs.Count -gt 0) { $argv += $IdfArgs } else { $argv += 'build' }
$argv += $Rest

# -D options and -B are *global* idf.py options and must precede the subcommand:
# `idf.py build -B build-qemu` is rejected with "No such option '-B'". The
# original version of this script had the -B block but built into build/
# anyway, because the flag never reached idf.py - the QEMU variant and the
# hardware image were overwriting each other while the comments claimed
# otherwise.
if ($NoPsram) {
    # SDKCONFIG_DEFAULTS is applied in order, so the overlay layers over the
    # hardware defaults rather than replacing them.
    $argv = @(
        '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.nopsram',
        '-B', 'build-qemu',
        '-DSDKCONFIG=build-qemu/sdkconfig'
    ) + $argv
}

if ($Target) { $argv = @("-DIDF_TARGET=$Target") + $argv }

Push-Location $Project
try {
    & $script:S3Python $script:S3IdfPy @argv
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($code -ne 0) { exit $code }

if ($Qemu) {
    $qemuArgs = @('-Project', $Project, '-BuildDir', 'build-qemu')
    if ($QemuTimeoutSec -gt 0) { $qemuArgs += @('-TimeoutSec', $QemuTimeoutSec) }
    & (Join-Path $PSScriptRoot 'qemu.ps1') @qemuArgs
    exit $LASTEXITCODE
}
