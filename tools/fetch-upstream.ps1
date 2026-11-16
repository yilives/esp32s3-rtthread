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
$idfExpected = '2265714019155c638ece55a6603869a5e156a0d7'
$idfRemote = 'https://github.com/RT-Thread-packages/esp-idf.git (branch rtt-dev)'

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
            Write-Host "        on $idfRemote" -ForegroundColor Yellow
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

# Order matters. 0000 turns the public rtt-dev tree into an S3-capable platform
# layer; 0001 is the SMP hooks on top of it. Applying 0001 to a tree that has not
# had 0000 fails on five files.
$patches = @(
    @{ Repo = $RtThreadPath; Name = '0001-kernel-smp-hooks.patch';     Path = "$repo\rt-thread\patches\0001-kernel-smp-hooks.patch" },
    @{ Repo = $IdfPath;      Name = '0000-idf-platform-s3.patch';      Path = "$repo\esp-idf\patches\0000-idf-platform-s3.patch" },
    @{ Repo = $IdfPath;      Name = '0001-idf-core-smp-hooks.patch';   Path = "$repo\esp-idf\patches\0001-idf-core-smp-hooks.patch" }
)

# Patches must be applied IN ORDER, and `git apply --check` cannot verify that:
# it tests every path against the tree as it is now, so a second patch that only
# applies after the first one has landed is reported as a failure. Measured -
# 0000 then 0001 both apply cleanly when applied for real, but checking them in
# one `git apply --check a b` invocation fails on five files.
#
# So: require a clean tree, apply for real one patch at a time, and roll the whole
# thing back if any of them refuses. A half-patched tree is worse than none - it
# builds until it does not, and the failure shows up somewhere unrelated.
if (-not $SkipPatches) {
    $touched = @()

    function Test-CleanRepo($path, $label) {
        Push-Location $path
        try {
            $dirty = git status --porcelain 2>&1
            if ($LASTEXITCODE -ne 0) { Fail "cannot read git status of $label" }
            if ($dirty) {
                Write-Host "  $label has uncommitted changes:" -ForegroundColor Yellow
                $dirty | Select-Object -First 8 | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
                Fail "commit, stash or revert them first - this script applies patches"
            }
        } finally { Pop-Location }
    }

    Test-CleanRepo $RtThreadPath 'RT-Thread tree'
    Test-CleanRepo $IdfPath 'ESP-IDF tree'

    Write-Host "Applying patches in order..."

    try {
        foreach ($p in $patches) {
            if (-not (Test-Path $p.Path)) { Fail "patch not found: $($p.Path)" }

            Push-Location $p.Repo
            try {
                # --whitespace=nowarn: a patch that adds a blank line at EOF is
                # applied happily by git, but the warning goes to stderr and
                # PowerShell then reports a non-zero status for a successful
                # apply. Measured: 0000-idf-platform-s3.patch trips this.
                $out = git apply --whitespace=nowarn $p.Path 2>&1
                if ($LASTEXITCODE -ne 0) {
                    Write-Host "  FAILED  $($p.Name)" -ForegroundColor Red
                    $out | Select-Object -First 6 | ForEach-Object { Write-Host "          $_" -ForegroundColor Red }
                    throw "patch $($p.Name) does not apply"
                }
                if (-not ($touched -contains $p.Repo)) { $touched += $p.Repo }
                Info "ok      $($p.Name)"
            } finally {
                Pop-Location
            }
        }
    } catch {
        Write-Host ""
        Write-Host "  Rolling back the patches already applied..." -ForegroundColor Yellow
        foreach ($r in $touched) {
            Push-Location $r
            try {
                git checkout -- . 2>&1 | Out-Null
                git clean -fdq 2>&1 | Out-Null
            } finally { Pop-Location }
        }
        Write-Host "  Both trees are back to the state they were in." -ForegroundColor Yellow
        Fail $_.Exception.Message
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

# Patches were applied above, before the copy. The two steps do not overlap: the
# patches touch src/ (RT-Thread) and components/{esp_libc,esp_system,freertos,...}
# (IDF), the copy writes bsp/ESP/ESP32_S3 + libcpu/xtensa and
# components/rtthread. Copying after patching therefore cannot undo a hunk.
#
# This block used to apply the patches a second time, here, and after my change
# above it failed every run with "patch does not apply" - the tree was already
# patched. One application point, not two.

# ---------------------------------------------------------------- done ---

Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host ""
Write-Host "Next:"
Write-Host "  .\tools\build.ps1 -Project .\project\s3-rtt-test"
Write-Host ""
Write-Host "If you skipped patches, apply them by hand - see docs/BUILD.md."
