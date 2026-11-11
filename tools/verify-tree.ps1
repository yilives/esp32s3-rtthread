# Report whether the two upstream working trees contain only the changes this
# repository ships.
#
# Why this exists: three files in the local ESP-IDF tree once carried
# uncommitted debug instrumentation (RTC_NOINIT counters and diagnostic prints
# referring to a `c5tcp` test, in components/lwip and components/esp_netif).
# They were not part of this repository's patches, they changed the memory
# layout enough to alter behaviour under test, and one of them did not even
# compile. Time was spent investigating a "Guru" and a "BLE assert" that
# existed only because of that residue.
#
# What it does NOT do: judge whether a change is good. It classifies each
# working-tree change as one of
#
#   expected   a file this repository ships or patches - normal
#   foreign    a change this repository knows nothing about - investigate
#   ignored    under components/rtthread/port/ for a chip this repository does
#              not target (esp32s31 and friends), or build output
#
# Usage:
#   .\verify-tree.ps1
#   .\verify-tree.ps1 -RtThreadPath <path> -IdfPath <path>
#   .\verify-tree.ps1 -Fix        # revert the foreign changes (asks per file)

param(
    [string]$RtThreadPath = '',
    [string]$IdfPath = '',
    [switch]$Fix
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot

# Paths, resolved the same way the build driver resolves them.
if (-not $RtThreadPath) {
    if ($env:RTTHREAD_ROOT) { $RtThreadPath = $env:RTTHREAD_ROOT }
    else {
        foreach ($c in @("$repo\..\rt-thread", "$repo\..\..\rt-thread")) {
            if (Test-Path (Join-Path $c 'src\scheduler_mp.c')) { $RtThreadPath = $c; break }
        }
    }
}
if (-not $IdfPath) {
    if ($env:IDF_PATH) { $IdfPath = $env:IDF_PATH }
    else {
        foreach ($c in @("$repo\..\esp-idf-rtt", "$repo\..\esp-idf")) {
            if (Test-Path (Join-Path $c 'tools\idf.py')) { $IdfPath = $c; break }
        }
    }
}
if (-not $RtThreadPath -or -not (Test-Path $RtThreadPath)) { throw "RT-Thread tree not found; pass -RtThreadPath." }
if (-not $IdfPath -or -not (Test-Path $IdfPath)) { throw "ESP-IDF tree not found; pass -IdfPath." }
$RtThreadPath = (Resolve-Path $RtThreadPath).Path
$IdfPath = (Resolve-Path $IdfPath).Path

# --------------------------------------------------------------- expected ---
#
# A change is "expected" if it lands in a file this repository either ships
# verbatim or patches. Collect both sets from the repository itself rather than
# hard-coding lists here, so adding a patch file cannot leave this stale.

function Get-PatchFiles($patchPath) {
    if (-not (Test-Path $patchPath)) { return @() }
    $out = @()
    foreach ($line in Get-Content $patchPath) {
        if ($line -match '^diff --git a/(\S+) b/') { $out += $Matches[1] }
    }
    return $out
}

function Get-ShippedFiles($root) {
    if (-not (Test-Path $root)) { return @() }
    return (Get-ChildItem $root -Recurse -File |
            Where-Object { $_.FullName -notmatch '__pycache__' } |
            ForEach-Object { $_.FullName.Substring($root.Length).TrimStart('\') -replace '\\', '/' })
}

$idfPatches = @()
Get-ChildItem "$repo\esp-idf\patches" -File -Filter '*.patch' -ErrorAction SilentlyContinue |
    ForEach-Object { $idfPatches += Get-PatchFiles $_.FullName }

$kernelPatches = @()
Get-ChildItem "$repo\rt-thread\patches" -File -Filter '*.patch' -ErrorAction SilentlyContinue |
    ForEach-Object { $kernelPatches += Get-PatchFiles $_.FullName }

# Files copied wholesale by fetch-upstream.ps1 - a match here is expected too.
# The component copy lands at <IDF>/components/rtthread, so its own paths are
# relative to that; git reports them from the IDF root. Prefix accordingly, or
# every shipped component file looks foreign.
$idfShipped = Get-ShippedFiles "$repo\esp-idf\components\rtthread" |
              ForEach-Object { "components/rtthread/$_" }
$rttShipped = Get-ShippedFiles "$repo\rt-thread\bsp\ESP\ESP32_S3" | ForEach-Object { "bsp/ESP/ESP32_S3/$_" }
$rttShipped += Get-ShippedFiles "$repo\rt-thread\libcpu\xtensa"    | ForEach-Object { "libcpu/xtensa/$_" }

function Test-Expected($rel, $patchFiles, $shippedFiles) {
    if ($patchFiles -contains $rel) { return $true }

    # git reports an untracked directory as "bsp/ESP/ESP32_S3/" with a trailing
    # slash and no file name. Treat it as expected when everything this
    # repository ships lives under it - the directory IS a shipped tree.
    $prefix = $rel.TrimEnd('/')
    foreach ($s in $shippedFiles) {
        if ($s -eq $rel) { return $true }
        if ($rel.EndsWith('/') -and $s.StartsWith("$prefix/")) { return $true }
    }
    return $false
}

# ---------------------------------------------------------------- scan ------

$report = @()

function Scan-Tree($path, $label, $patchFiles, $shippedFiles, [string[]]$ignorePrefixes) {
    $lines = & git -C $path status --porcelain
    foreach ($line in $lines) {
        if (-not $line.Trim()) { continue }

        $code = $line.Substring(0, 2).Trim()
        $rel  = $line.Substring(3).Trim().Trim('"') -replace '\\', '/'

        $ignored = $false
        foreach ($p in $ignorePrefixes) { if ($rel.StartsWith($p)) { $ignored = $true; break } }
        if ($ignored) { continue }

        $kind = if (Test-Expected $rel $patchFiles $shippedFiles) { 'expected' } else { 'foreign' }

        $script:report += [pscustomobject]@{
            Tree = $label
            Kind = $kind
            Code = $code
            Path = $rel
        }
    }
}

Scan-Tree $RtThreadPath 'rt-thread' $kernelPatches $rttShipped @()
Scan-Tree $IdfPath 'esp-idf' $idfPatches $idfShipped @(
    # Another chip's port. This repository does not target it, and its files are
    # not shipped here, so changes under it are not evidence of anything.
    'components/rtthread/port/esp32s31/',
    'components/rtthread/port/esp32c3/',
    'components/rtthread/port/esp32c5/',
    'components/rtthread/port/esp32c6/'
)

# --------------------------------------------------------------- report -----

$expected = @($report | Where-Object { $_.Kind -eq 'expected' })
$foreign  = @($report | Where-Object { $_.Kind -eq 'foreign' })

Write-Host ""
Write-Host "working tree report"
Write-Host "  rt-thread : $RtThreadPath"
Write-Host "  esp-idf   : $IdfPath"
Write-Host ""

if ($expected.Count -gt 0) {
    Write-Host "  expected ($($expected.Count)) - this repository ships or patches these:" -ForegroundColor DarkGray
    $expected | ForEach-Object { Write-Host ("    {0,-3} {1,-10} {2}" -f $_.Code, $_.Tree, $_.Path) -ForegroundColor DarkGray }
    Write-Host ""
}

if ($foreign.Count -eq 0) {
    Write-Host "  No foreign changes." -ForegroundColor Green
    Write-Host ""
    Write-Host "The tree contains only what this repository ships. A build from it is" -ForegroundColor DarkGray
    Write-Host "the same image the patches and the component copy describe." -ForegroundColor DarkGray
    exit 0
}

Write-Host "  FOREIGN ($($foreign.Count)) - not shipped or patched here:" -ForegroundColor Yellow
$foreign | ForEach-Object { Write-Host ("    {0,-3} {1,-10} {2}" -f $_.Code, $_.Tree, $_.Path) -ForegroundColor Yellow }
Write-Host ""
Write-Host "  These are the changes that make a local build differ from what this" -ForegroundColor Yellow
Write-Host "  repository delivers. Local debug instrumentation is the usual cause, and" -ForegroundColor Yellow
Write-Host "  it has already been mistaken for a port defect once: three such files in" -ForegroundColor Yellow
Write-Host "  components/lwip and components/esp_netif produced a Guru and a BLE assert" -ForegroundColor Yellow
Write-Host "  that did not exist without them." -ForegroundColor Yellow
Write-Host ""

if (-not $Fix) {
    Write-Host "  Re-run with -Fix to revert them (asks for each file)." -ForegroundColor DarkGray
    exit 1
}

# ------------------------------------------------------------------ fix -----

foreach ($f in $foreign) {
    $root = if ($f.Tree -eq 'rt-thread') { $RtThreadPath } else { $IdfPath }

    if ($f.Code -eq '??') {
        Write-Host "  untracked, left alone: $($f.Path)" -ForegroundColor DarkGray
        Write-Host "    (git clean would delete it; doing that automatically is not safe)" -ForegroundColor DarkGray
        continue
    }

    $answer = Read-Host "  revert $($f.Tree)/$($f.Path)? [y/N]"
    if ($answer -match '^[Yy]') {
        & git -C $root checkout -- $f.Path
        if ($LASTEXITCODE -eq 0) { Write-Host "    reverted" -ForegroundColor Green }
        else { Write-Host "    FAILED" -ForegroundColor Red }
    }
}

exit 0
