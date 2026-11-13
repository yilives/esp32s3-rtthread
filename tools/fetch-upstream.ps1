# Apply this repository's changes to an upstream RT-Thread + ESP-IDF tree.
#
# Every step is checked before it changes anything: the patches are dry-run with
# `git apply --check` first, and all of them must apply before a single file is
# copied. A half-applied port is worse than none - it builds until it does not.
#
# Usage:
#   .\fetch-upstream.ps1 -RtThreadPath I:\RT_thread\rt-thread -IdfPath I:\RT_thread\esp-idf-rtt
#   .\fetch-upstream.ps1 -RtThreadPath ... -IdfPath ... -SkipPatches
#   .\fetch-upstream.ps1 -RtThreadPath ... -IdfPath ... -KeepLocal

param(
    [Parameter(Mandatory = $true)][string]$RtThreadPath,
    [Parameter(Mandatory = $true)][string]$IdfPath,
    [switch]$SkipPatches,
    [switch]$KeepLocal
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

# ------------------------------------------------------- upstream identity --
#
# The IDF side is not "any ESP-IDF v6.1" - it is one fork at one commit, and the
# patch in this repository is written against that baseline. Handing this script
# RT-Thread-packages/rtt-dev instead fails at `git apply --check` with four files
# and no explanation of why, which is exactly what a reader reported.
#
# So check the baseline up front and say what the right one is, rather than
# letting them read a patch rejection.
$idfExpected = '6b6076c8e20206f3e627bf0ab6791bdebf9e7c27'
$idfRemote = 'https://github.com/yilives/esp-idf-rtt-c5.git'

if (Get-Command git -ErrorAction SilentlyContinue) {
    $idfHead = (& git -C $IdfPath rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -eq 0 -and $idfHead) {
        $idfHead = $idfHead.Trim()
        if ($idfHead -ne $idfExpected) {
            # Not fatal on its own - the patch check below is the real test - but
            # an unexpected baseline is worth stating before anything is written.
            & git -C $IdfPath merge-base --is-ancestor $idfExpected HEAD 2>$null | Out-Null
            $isAncestor = ($LASTEXITCODE -eq 0)
            Write-Host ""
            Write-Host "  NOTE: IDF tree is at $idfHead" -ForegroundColor Yellow
            Write-Host "        this repository targets $idfExpected" -ForegroundColor Yellow
            Write-Host "        on $idfRemote (branch main)" -ForegroundColor Yellow
            if (-not $isAncestor) {
                Write-Host "        and that commit is not an ancestor of the tree you gave," -ForegroundColor Yellow
                Write-Host "        so the IDF patch will almost certainly fail below." -ForegroundColor Yellow
            }
            Write-Host ""
        }
    }

    $rttHead = (& git -C $RtThreadPath rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -eq 0 -and $rttHead) {
        $rttHead = $rttHead.Trim()
        $rttExpected = '9e6dec3b14f5bf5514b0da829736c2485c38b0a7'
        if ($rttHead -ne $rttExpected) {
            Write-Host ""
            Write-Host "  NOTE: RT-Thread tree is at $rttHead" -ForegroundColor Yellow
            Write-Host "        this repository targets $rttExpected (v5.4.0)" -ForegroundColor Yellow
            Write-Host ""
        }
    }
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

# Copy the port over the upstream tree.
#
# These files ARE the port, so copying is the whole point and a file that
# differs from what is already there is the normal case, not a conflict to
# protect against. An earlier version skipped differing files unless -Force was
# given, which meant a user following the README got a tree containing none of
# the actual changes - measured on a clean worktree: nine of the component's
# files, including ft_ipc.c and ft_core.h, were silently left at the upstream
# version, and the build would have lacked the IRAM fixes.
#
# -KeepLocal inverts the default for anyone who has edited the tree by hand.
function Copy-Tree($from, $to) {
    if (-not (Test-Path $from)) { Fail "source missing: $from" }

    $copied = 0
    $skipped = @()

    Get-ChildItem $from -Recurse -File | Where-Object {
        $_.FullName -notmatch '__pycache__'
    } | ForEach-Object {
        $rel = $_.FullName.Substring($from.Length).TrimStart('\')
        $dst = Join-Path $to $rel
        New-Item -ItemType Directory -Path (Split-Path $dst) -Force | Out-Null

        if ($KeepLocal -and (Test-Path $dst)) {
            $a = (Get-FileHash $_.FullName).Hash
            $b = (Get-FileHash $dst).Hash
            if ($a -ne $b) {
                $skipped += $rel
                return
            }
        }

        Copy-Item $_.FullName $dst -Force
        $copied++
    }

    if ($skipped.Count -gt 0) {
        Write-Host "  kept local: $($skipped -join ', ')" -ForegroundColor Yellow
    }
    Info "$copied file(s) -> $to"
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
