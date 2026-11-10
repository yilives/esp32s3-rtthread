# Apply this repository's changes to an upstream RT-Thread + ESP-IDF tree.
#
# Every step is checked before it changes anything: the patches are dry-run with
# `git apply --check` first, and all of them must apply before a single file is
# copied. A half-applied port is worse than none - it builds until it does not.
#
# Usage:
#   .\fetch-upstream.ps1 -RtThreadPath I:\RT_thread\rt-thread -IdfPath I:\RT_thread\esp-idf-rtt
#   .\fetch-upstream.ps1 -RtThreadPath ... -IdfPath ... -SkipPatches
#   .\fetch-upstream.ps1 -RtThreadPath ... -IdfPath ... -Force

param(
    [Parameter(Mandatory = $true)][string]$RtThreadPath,
    [Parameter(Mandatory = $true)][string]$IdfPath,
    [switch]$SkipPatches,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot

function Fail($msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

function Info($msg) {
    Write-Host "  $msg"
}

Write-Host ""
Write-Host "esp32s3-rtthread: apply to upstream"
Write-Host "  repo      : $repo"
Write-Host "  RT-Thread : $RtThreadPath"
Write-Host "  ESP-IDF   : $IdfPath"
Write-Host ""

# ---------------------------------------------------------------- sanity ---

if (-not (Test-Path (Join-Path $RtThreadPath 'src\scheduler_mp.c'))) {
    Fail "RTT_ROOT '$RtThreadPath' does not look like an RT-Thread tree (no src/scheduler_mp.c)"
}
if (-not (Test-Path (Join-Path $IdfPath 'components\esp_system'))) {
    Fail "IDF_PATH '$IdfPath' does not look like an ESP-IDF tree (no components/esp_system)"
}
if (-not (Test-Path (Join-Path $IdfPath '.git'))) {
    Fail "IDF_PATH '$IdfPath' is not a git repository; the patches need git apply"
}
if (-not (Test-Path (Join-Path $RtThreadPath '.git'))) {
    Fail "RTT_ROOT '$RtThreadPath' is not a git repository; the patches need git apply"
}

# --------------------------------------------------------- patch dry run ---
#
# All patches are checked BEFORE anything is written. Applying three of four and
# failing leaves a tree that neither builds nor reverts cleanly.

$patches = @(
    @{ Repo = $RtThreadPath; Name = '0001-kernel-smp-hooks.patch';     Path = "$repo\rt-thread\patches\0001-kernel-smp-hooks.patch" },
    @{ Repo = $IdfPath;      Name = '0001-idf-core-smp-hooks.patch';   Path = "$repo\esp-idf\patches\0001-idf-core-smp-hooks.patch" }
)

if (-not $SkipPatches) {
    Write-Host "Checking patches apply cleanly..."

    foreach ($p in $patches) {
        if (-not (Test-Path $p.Path)) { Fail "patch not found: $($p.Path)" }

        Push-Location $p.Repo
        try {
            $out = git apply --check $p.Path 2>&1
            if ($LASTEXITCODE -ne 0) {
                Write-Host "  FAILED  $($p.Name)" -ForegroundColor Red
                Write-Host "          $out"
                Fail "patch does not apply; nothing has been changed"
            }
            Info "ok      $($p.Name)"
        } finally {
            Pop-Location
        }
    }
    Write-Host ""
}

# ---------------------------------------------------------------- copy ---
#
# Nothing above this point has modified the tree.

function Copy-Tree($from, $to) {
    if (-not (Test-Path $from)) { Fail "source missing: $from" }

    if ((Test-Path $to) -and -not $Force) {
        Write-Host "  exists  $to (use -Force to overwrite)" -ForegroundColor Yellow
        # Still copy files that are missing, so a partially-populated tree gets
        # completed without clobbering local edits.
    }

    Get-ChildItem $from -Recurse -File | Where-Object {
        $_.FullName -notmatch '__pycache__'
    } | ForEach-Object {
        $rel = $_.FullName.Substring($from.Length).TrimStart('\')
        $dst = Join-Path $to $rel
        New-Item -ItemType Directory -Path (Split-Path $dst) -Force | Out-Null
        if ((Test-Path $dst) -and -not $Force) {
            $a = (Get-FileHash $_.FullName).Hash
            $b = (Get-FileHash $dst).Hash
            if ($a -ne $b) {
                Write-Host "  differs $rel" -ForegroundColor Yellow
                return
            }
        }
        Copy-Item $_.FullName $dst -Force
    }
}

Write-Host "Copying RT-Thread BSP and architecture layer..."
Copy-Tree "$repo\rt-thread\bsp\ESP\ESP32_S3"  "$RtThreadPath\bsp\ESP\ESP32_S3"
Copy-Tree "$repo\rt-thread\libcpu\xtensa"     "$RtThreadPath\libcpu\xtensa"

Write-Host "Copying the RT-Thread IDF component..."
Copy-Tree "$repo\esp-idf\components\rtthread" "$IdfPath\components\rtthread"

# --------------------------------------------------------------- patch ---

if (-not $SkipPatches) {
    Write-Host ""
    Write-Host "Applying patches..."

    foreach ($p in $patches) {
        Push-Location $p.Repo
        try {
            $out = git apply $p.Path 2>&1
            if ($LASTEXITCODE -ne 0) {
                Write-Host "  FAILED  $($p.Name)" -ForegroundColor Red
                Write-Host "          $out"
                Fail "apply failed after files were copied; review with 'git diff'"
            }
            Info "applied $($p.Name)"
        } finally {
            Pop-Location
        }
    }
}

# ---------------------------------------------------------------- done ---

Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host ""
Write-Host "Next:"
Write-Host "  .\tools\build.ps1 -Project .\project\s3-rtt-test"
Write-Host ""
Write-Host "If you skipped patches, apply them by hand - see docs/BUILD.md."
