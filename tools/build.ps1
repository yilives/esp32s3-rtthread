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

# Where the upstream RT-Thread checkout is. The project's CMakeLists resolves
# this itself (flag, then environment, then a sibling directory), but exporting
# it here means the flag does not have to be repeated for every idf.py
# invocation, and it keeps the BSP and the kernel coming from the same tree.
if (-not $env:RTTHREAD_ROOT) {
    foreach ($candidate in @("$repo\..\rt-thread", "$repo\..\..\rt-thread")) {
        if (Test-Path (Join-Path $candidate 'src\scheduler_mp.c')) {
            $env:RTTHREAD_ROOT = (Resolve-Path $candidate).Path
            break
        }
    }
}
if ($env:RTTHREAD_ROOT) {
    # Forward slashes, deliberately, and here rather than only in the project's
    # CMakeLists.
    #
    # components/rtthread/CMakeLists.txt re-reads this variable straight out of
    # the environment and interpolates it into INCLUDE_DIRS, which ESP-IDF then
    # feeds to cmake's foreach() - where a backslash is an escape character. A
    # Windows path like I:\RT_thread\_verify_rtt fails to configure with
    #
    #   Invalid character escape '\R'.
    #
    # A normalised copy in the project scope is overwritten by that re-read, so
    # normalising the environment variable is what actually takes effect.
    $env:RTTHREAD_ROOT = $env:RTTHREAD_ROOT -replace '\\', '/'
    Write-Host "RTTHREAD_ROOT: $env:RTTHREAD_ROOT"
} else {
    Write-Host "RTTHREAD_ROOT: not found next to this repository - set it, or pass -DRTTHREAD_ROOT" -ForegroundColor Yellow
}

# idf.py needs a version string, and it takes it from this variable. It is
# normally set by export.ps1; without it, the first configure in a clean tree
# fails inside idf_component_manager with
#
#   TypeError: expected string or bytes-like object, got 'NoneType'
#
# because Version.coerce() is handed None. That only shows up on a fresh
# checkout - an existing build/ directory never re-runs the code path - which is
# exactly the situation a first-time user is in.
if (-not $env:ESP_IDF_VERSION) {
    $verFile = Join-Path $env:IDF_PATH 'version.txt'
    $fromCmake = Join-Path $env:IDF_PATH 'tools\cmake\version.cmake'

    if (Test-Path $verFile) {
        $env:ESP_IDF_VERSION = (Get-Content $verFile -Raw).Trim()
    } elseif (Test-Path $fromCmake) {
        # Authoritative and local: the components' Python tooling wants a
        # semantic version, and `git describe` on a fork branch yields a bare
        # commit hash, which Version.coerce() rejects.
        $maj = (Select-String -Path $fromCmake -Pattern 'set\(IDF_VERSION_MAJOR\s+(\d+)\)').Matches.Groups[1].Value
        $min = (Select-String -Path $fromCmake -Pattern 'set\(IDF_VERSION_MINOR\s+(\d+)\)').Matches.Groups[1].Value
        $pat = (Select-String -Path $fromCmake -Pattern 'set\(IDF_VERSION_PATCH\s+(\d+)\)').Matches.Groups[1].Value
        if ($maj -and $min -and $pat) {
            $env:ESP_IDF_VERSION = "$maj.$min.$pat"
        }
    }
    if (-not $env:ESP_IDF_VERSION) {
        throw "Cannot determine the ESP-IDF version: no version.txt and no tools/cmake/version.cmake under $env:IDF_PATH. Set ESP_IDF_VERSION (for example 6.1.0) and retry."
    }
    Write-Host "ESP_IDF_VERSION: $env:ESP_IDF_VERSION (derived)"
}

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

# Find the directory containing a given toolchain executable.
#
# The install layout nests differently per tool - xtensa lives at
# tools/xtensa-esp-elf/<version>/xtensa-esp-elf/bin, ninja at
# tools/ninja/<version>/ninja.exe - so the depth is not something to guess.
# Searching for the executable is the only form that survives an IDF upgrade.
function Find-ToolBin($installDirPattern, $exeName) {
    $root = Get-ChildItem $tools -Directory -Filter $installDirPattern -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | Select-Object -First 1
    if (-not $root) { return $null }

    $exe = Get-ChildItem $root.FullName -Recurse -Filter $exeName -File -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($exe) { return $exe.DirectoryName }
    return $null
}

$pathParts = @()

foreach ($spec in @(
    @('xtensa-esp-elf',      'xtensa-esp32s3-elf-gcc.exe'),
    @('riscv32-esp-elf',     'riscv32-esp-elf-gcc.exe'),
    @('xtensa-esp-elf-gdb',  'xtensa-esp32s3-elf-gdb.exe'),
    @('cmake',               'cmake.exe'),
    @('ninja',               'ninja.exe'),
    @('ccache',              'ccache.exe'),
    @('qemu-xtensa',         'qemu-system-xtensa.exe')
)) {
    $dir = Find-ToolBin $spec[0] $spec[1]
    if ($dir) {
        $pathParts += $dir
    } else {
        # riscv32 is only needed for the ULP program and qemu only for the QEMU
        # gate; the others are mandatory.
        if ($spec[0] -in @('xtensa-esp-elf', 'cmake', 'ninja')) {
            throw "Cannot find $($spec[1]) under $tools. Run install.ps1 for target $Target."
        }
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
