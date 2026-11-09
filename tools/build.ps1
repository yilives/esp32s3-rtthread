# Build an ESP32-S3 ESP-IDF project from PowerShell, without Git Bash.
#
# This build driver is a thin wrapper around idf.py whose only real job is
# composing PATH: idf.py is a Python script that shells out to cmake, ninja,
# ccache and the cross compilers, and when any of them is missing from PATH the
# failure surfaces as a CMake error deep in the configure step rather than as an
# obvious "not found".
#
# Configuration, all optional via environment variables:
#
#   IDF_PATH              ESP-IDF tree            (default: <repo>/../esp-idf-rtt)
#   IDF_PYTHON_ENV_PATH   ESP-IDF python venv     (default: newest under ESP_IDF_TOOLS)
#   ESP_IDF_TOOLS         .espressif/tools        (default: %USERPROFILE%\.espressif\tools)
#
# RTT_IDF_PATH is set from IDF_PATH because the BSP's scons scripts read it.
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

$tools = if ($env:ESP_IDF_TOOLS) { $env:ESP_IDF_TOOLS }
         else { Join-Path $env:USERPROFILE '.espressif\tools' }

if (-not (Test-Path $tools)) {
    throw "ESP-IDF tools not found at '$tools'. Run install.ps1 for target $Target first, or set ESP_IDF_TOOLS."
}

# ESP-IDF tree. Prefer an explicit IDF_PATH; otherwise look for a sibling
# checkout, which is how this repository's docs suggest laying things out.
if (-not $env:IDF_PATH) {
    foreach ($candidate in @("$repo\..\esp-idf-rtt", "$repo\..\esp-idf")) {
        if (Test-Path (Join-Path $candidate 'tools\idf.py')) {
            $env:IDF_PATH = (Resolve-Path $candidate).Path
            break
        }
    }
}
if (-not $env:IDF_PATH -or -not (Test-Path (Join-Path $env:IDF_PATH 'tools\idf.py'))) {
    throw "IDF_PATH is not set and no ESP-IDF checkout was found next to this repo. Set IDF_PATH."
}

# The python venv install.ps1 created. Its directory name carries the IDF
# version, so discover it rather than hard-coding one.
if (-not $env:IDF_PYTHON_ENV_PATH) {
    $envRoot = Join-Path $env:USERPROFILE '.espressif\python_env'
    if (Test-Path $envRoot) {
        $venv = Get-ChildItem $envRoot -Directory -Filter 'idf*_env' -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($venv) { $env:IDF_PYTHON_ENV_PATH = $venv.FullName }
    }
}
if (-not $env:IDF_PYTHON_ENV_PATH -or
    -not (Test-Path (Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'))) {
    throw "IDF_PYTHON_ENV_PATH not set and no python venv found under $env:USERPROFILE\.espressif\python_env"
}

# The BSP's scons scripts read this; keep it in step with IDF_PATH.
$env:RTT_IDF_PATH = $env:IDF_PATH

# esptool wants a ROM ELF directory to build and verify images on some versions.
if (-not $env:ESP_ROM_ELF_DIR) {
    $romElf = Join-Path $tools 'esp-rom-elfs'
    if (Test-Path $romElf) {
        $latest = Get-ChildItem $romElf -Directory | Sort-Object Name -Descending |
                  Select-Object -First 1
        if ($latest) { $env:ESP_ROM_ELF_DIR = $latest.FullName + '\' }
    }
}

$env:PYTHONUNBUFFERED = '1'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
cmd /c chcp 65001 | Out-Null

# ------------------------------------------------------------------- PATH ---
#
# Tool directories come before the inherited PATH so a system-wide cmake or ninja
# cannot shadow the ones ESP-IDF was installed against.

function Find-Tool($pattern) {
    if (-not (Test-Path $tools)) { return $null }
    $d = Get-ChildItem $tools -Directory -Filter $pattern -ErrorAction SilentlyContinue |
         Sort-Object Name -Descending | Select-Object -First 1
    if ($d) { return $d.FullName }
    return $null
}

$pathParts = @()

# Xtensa: the target's own toolchain. The versioned directory contains another
# directory (xtensa-esp-elf) whose bin/ is the one on PATH.
$xtensa = Find-Tool 'xtensa-esp-elf'
if ($xtensa) {
    $inner = Get-ChildItem $xtensa -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($inner) {
        $p = Join-Path $inner.FullName 'bin'
        if (Test-Path $p) { $pathParts += $p }
    }
}

# riscv32: only needed for the ULP co-processor program. That build is a nested
# CMake project which looks riscv32-esp-elf-gcc up on PATH, so without this the
# main build fails inside the nested configure step with
# "CMAKE_C_COMPILER riscv32-esp-elf-gcc is not a full path".
$riscv = Find-Tool 'riscv32-esp-elf'
if ($riscv) {
    $inner = Get-ChildItem $riscv -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($inner) {
        $p = Join-Path $inner.FullName 'bin'
        if (Test-Path $p) { $pathParts += $p }
    }
}

$gdb = Find-Tool 'xtensa-esp-elf-gdb'
if ($gdb) {
    $inner = Get-ChildItem $gdb -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($inner) {
        $p = Join-Path $inner.FullName 'bin'
        if (Test-Path $p) { $pathParts += $p }
    }
}

foreach ($pair in @(@('cmake', 'bin'), @('ninja', ''), @('ccache', ''), @('qemu-xtensa', 'qemu\bin'))) {
    $d = Find-Tool $pair[0]
    if (-not $d) { continue }

    if ($pair[1] -eq '') {
        # The tool may sit directly in the versioned directory or one level in.
        $exe = Get-ChildItem $d -Recurse -Depth 2 -Filter '*.exe' -ErrorAction SilentlyContinue |
               Select-Object -First 1
        if ($exe) { $pathParts += $exe.DirectoryName }
        continue
    }

    $cand = Join-Path $d $pair[1]
    if (Test-Path $cand) { $pathParts += $cand; continue }

    $inner = Get-ChildItem $d -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($inner) {
        $cand = Join-Path $inner.FullName $pair[1]
        if (Test-Path $cand) { $pathParts += $cand }
    }
}

# idf.py imports helpers out of these component directories.
$pathParts += "$env:IDF_PATH\components\espcoredump"
$pathParts += "$env:IDF_PATH\components\partition_table"
$pathParts += "$env:IDF_PATH\components\app_update"
$pathParts += "$env:IDF_PYTHON_ENV_PATH\Scripts"
$pathParts += "$env:IDF_PATH\tools"

$env:PATH = ($pathParts -join ';') + ';' + $env:PATH

$python = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$idfPy  = Join-Path $env:IDF_PATH 'tools\idf.py'

Write-Host "IDF_PATH   : $env:IDF_PATH"
Write-Host "Project    : $Project"
Write-Host "Target     : $Target"
Write-Host "Python     : $python"
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

if ($Target) { $argv = @("-DIDF_TARGET=$Target") + $argv }

Push-Location $Project
try {
    & $python $idfPy @argv
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
