# Shared environment discovery for the ESP32-S3 port's PowerShell drivers.
#
# Dot-source this from build.ps1 / qemu.ps1 / qemu-gdb.ps1:
#
#   . (Join-Path $PSScriptRoot 'env.ps1')
#
# It exists because the three scripts previously each carried their own copy of
# the tool paths - and those copies had drifted. Two of them still hard-coded
# tool version directories (xtensa-esp-elf\esp-15.2.0_20251204, cmake\4.0.3,
# ninja\1.12.1, qemu-esp_develop_9.2.2_20260417), which means an IDF upgrade
# silently breaks them, and one of them rebuilt PATH in a way that produced a
# path missing a directory level, so the compiler was "not found" while the
# directory it named existed.
#
# Everything below searches for the executable it needs. No version numbers.

$ErrorActionPreference = 'Stop'

# This file lives in <repo>/tools, so its parent is the repository root.
# When dot-sourced, $PSScriptRoot is this file's own directory, not the
# caller's - which is what makes the sibling-checkout lookups below work no
# matter which driver pulled this in.
$script:S3Repo = Split-Path -Parent $PSScriptRoot

# ---------------------------------------------------------------- tools root --

if (-not $env:ESP_IDF_TOOLS) {
    $env:ESP_IDF_TOOLS = Join-Path $env:USERPROFILE '.espressif\tools'
}
$script:S3Tools = $env:ESP_IDF_TOOLS

if (-not (Test-Path $script:S3Tools)) {
    throw "ESP-IDF tools not found at '$script:S3Tools'. Run install.ps1 for the target first, or set ESP_IDF_TOOLS."
}

# Find the directory that contains a given executable, under an install
# directory whose name starts with $installDirPattern.
#
# The nesting depth differs per tool - xtensa lives at
# tools/xtensa-esp-elf/<version>/xtensa-esp-elf/bin, ninja at
# tools/ninja/<version>/ninja.exe - so the depth is not something to guess.
function Find-ToolBin($installDirPattern, $exeName) {
    $root = Get-ChildItem $script:S3Tools -Directory -Filter $installDirPattern -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | Select-Object -First 1
    if (-not $root) { return $null }

    $exe = Get-ChildItem $root.FullName -Recurse -Filter $exeName -File -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($exe) { return $exe.DirectoryName }
    return $null
}

# ------------------------------------------------------------------- IDF tree --

if (-not $env:IDF_PATH) {
    foreach ($candidate in @("$script:S3Repo\..\esp-idf-rtt", "$script:S3Repo\..\esp-idf")) {
        if (Test-Path (Join-Path $candidate 'tools\idf.py')) {
            $env:IDF_PATH = (Resolve-Path $candidate).Path
            break
        }
    }
}
if (-not $env:IDF_PATH -or -not (Test-Path (Join-Path $env:IDF_PATH 'tools\idf.py'))) {
    throw "IDF_PATH is not set and no ESP-IDF checkout was found next to this repository. Set IDF_PATH."
}
$env:IDF_PATH = $env:IDF_PATH -replace '\\', '/'

# The BSP's scons scripts read this; keep it in step with IDF_PATH.
$env:RTT_IDF_PATH = $env:IDF_PATH

# ------------------------------------------------------------- python venv ---

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

# ------------------------------------------------------------ IDF version -----
#
# idf.py needs this and normally gets it from export.ps1. Without it the first
# configure in a clean tree dies inside idf_component_manager:
#
#   TypeError: expected string or bytes-like object, got 'NoneType'
#
# It only affects a fresh checkout - an existing build/ never re-runs that code
# path - which is exactly the first-time-user case, so it is derived here.

if (-not $env:ESP_IDF_VERSION) {
    $verFile = Join-Path $env:IDF_PATH 'version.txt'
    $fromCmake = Join-Path $env:IDF_PATH 'tools/cmake/version.cmake'

    if (Test-Path $verFile) {
        $env:ESP_IDF_VERSION = (Get-Content $verFile -Raw).Trim()
    } elseif (Test-Path $fromCmake) {
        # `git describe` on a fork branch yields a bare commit hash, which
        # Version.coerce() rejects - the cmake file is both local and semantic.
        $maj = (Select-String -Path $fromCmake -Pattern 'set\(IDF_VERSION_MAJOR\s+(\d+)\)').Matches.Groups[1].Value
        $min = (Select-String -Path $fromCmake -Pattern 'set\(IDF_VERSION_MINOR\s+(\d+)\)').Matches.Groups[1].Value
        $pat = (Select-String -Path $fromCmake -Pattern 'set\(IDF_VERSION_PATCH\s+(\d+)\)').Matches.Groups[1].Value
        if ($maj -and $min -and $pat) { $env:ESP_IDF_VERSION = "$maj.$min.$pat" }
    }
    if (-not $env:ESP_IDF_VERSION) {
        throw "Cannot determine the ESP-IDF version: no version.txt and no tools/cmake/version.cmake under $env:IDF_PATH. Set ESP_IDF_VERSION (for example 6.1.0) and retry."
    }
}

# ------------------------------------------------------- RT-Thread checkout ---
#
# components/rtthread/CMakeLists.txt re-reads this from the environment and
# interpolates it into INCLUDE_DIRS, which ESP-IDF feeds to cmake's foreach() -
# where a backslash is an escape character. A Windows path such as
# I:\RT_thread\rt-thread fails to configure with
#
#   Invalid character escape '\R'.
#
# So normalising the environment variable is what takes effect; a normalised
# copy in the project scope gets overwritten by that re-read.

if (-not $env:RTTHREAD_ROOT) {
    foreach ($candidate in @("$script:S3Repo\..\rt-thread", "$script:S3Repo\..\..\rt-thread")) {
        if (Test-Path (Join-Path $candidate 'src\scheduler_mp.c')) {
            $env:RTTHREAD_ROOT = (Resolve-Path $candidate).Path
            break
        }
    }
}
if ($env:RTTHREAD_ROOT) {
    $env:RTTHREAD_ROOT = $env:RTTHREAD_ROOT -replace '\\', '/'
}

# --------------------------------------------------------------- ROM ELFs -----

if (-not $env:ESP_ROM_ELF_DIR) {
    $romElf = Join-Path $script:S3Tools 'esp-rom-elfs'
    if (Test-Path $romElf) {
        $latest = Get-ChildItem $romElf -Directory | Sort-Object Name -Descending |
                  Select-Object -First 1
        if ($latest) { $env:ESP_ROM_ELF_DIR = $latest.FullName + '\' }
    }
}

# --------------------------------------------------------------------- PATH ---

$script:S3ToolDirs = @{}
foreach ($spec in @(
    @('xtensa',     'xtensa-esp-elf',     'xtensa-esp32s3-elf-gcc.exe',  $true),
    @('riscv',      'riscv32-esp-elf',    'riscv32-esp-elf-gcc.exe',     $false),
    @('gdb',        'xtensa-esp-elf-gdb', 'xtensa-esp32s3-elf-gdb.exe',  $false),
    @('cmake',      'cmake',              'cmake.exe',                   $true),
    @('ninja',      'ninja',              'ninja.exe',                   $true),
    @('ccache',     'ccache',             'ccache.exe',                  $false),
    @('qemu',       'qemu-xtensa',        'qemu-system-xtensa.exe',      $false)
)) {
    $dir = Find-ToolBin $spec[1] $spec[2]
    if ($dir) {
        $script:S3ToolDirs[$spec[0]] = $dir
    } elseif ($spec[3]) {
        throw "Cannot find $($spec[2]) under $script:S3Tools. Run install.ps1 for the target first."
    }
}

$pathParts = @($script:S3ToolDirs.Values)
$pathParts += "$env:IDF_PATH/components/espcoredump"
$pathParts += "$env:IDF_PATH/components/partition_table"
$pathParts += "$env:IDF_PATH/components/app_update"
$pathParts += "$env:IDF_PYTHON_ENV_PATH/Scripts"
$pathParts += "$env:IDF_PATH/tools"

$env:PATH = ($pathParts -join ';') + ';' + $env:PATH

$script:S3Python = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$script:S3IdfPy  = Join-Path $env:IDF_PATH 'tools\idf.py'

# ---------------------------------------------------------------- console -----

$env:PYTHONUNBUFFERED = '1'
$env:PYTHONUTF8 = '1'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
cmd /c chcp 65001 | Out-Null
