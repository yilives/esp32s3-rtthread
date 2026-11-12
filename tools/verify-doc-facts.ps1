# Verify the structural claims the documentation makes about this repository.
#
# Why: the docs quote counts - files, lines, patch entries. Those rot silently.
# PORTING.md said the compatibility layer was 4204 lines in 9 files; it is 4943
# in 9 (8 .c + 1 .h), and an earlier revision said 3751. Nobody noticed either,
# because a number in prose does not fail to compile.
#
# This checks only claims that can be verified mechanically:
#
#   lines        a file or a directory of *.c has exactly N source lines
#   files        a directory holds exactly N files
#   patch        a patch file touches exactly N paths
#
# It deliberately does NOT try to verify prose, or timings, or anything that
# needs a board. Those live in prose for a reason.
#
# Usage:
#   .\verify-doc-facts.ps1
#   .\verify-doc-facts.ps1 -RepoRoot <path> -Project <path to s3-rtt-test>

param(
    [string]$RepoRoot = '',
    [string]$Project = '',
    # Upstream trees, resolved the same way the build driver resolves them.
    [string]$RtThreadPath = '',
    [string]$IdfPath = ''
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $RepoRoot) { $RepoRoot = $repo }
if (-not $Project) { $Project = Join-Path $repo 'project\s3-rtt-test' }

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

# ------------------------------------------------------------------- helpers --

# Count every line, blank ones included.
#
# `Get-Content x | Measure-Object -Line` counts only lines with content - it
# reported freertos_api.c as 786 where the file is 933. Split on newlines instead
# and drop the trailing empty element the final newline produces.
function Get-SourceLines([string]$path) {
    if (-not (Test-Path $path)) { return -1 }
    $raw = Get-Content $path -Raw -Encoding UTF8
    if ($null -eq $raw -or $raw.Length -eq 0) { return 0 }
    $parts = $raw -split "`r?`n"
    if ($parts.Count -gt 0 -and $parts[-1] -eq '') { $parts = $parts[0..($parts.Count - 2)] }
    return $parts.Count
}

function Get-SourceLinesIn([string]$dir, [string]$filter) {
    if (-not (Test-Path $dir)) { return -1 }
    $files = Get-ChildItem $dir -File -Filter $filter
    if ($files.Count -eq 0) { return 0 }
    $n = 0
    foreach ($f in $files) { $n += (Get-SourceLines $f.FullName) }
    return $n
}

# Recursive: the counts the docs quote are for whole trees, and the components
# directory has only four files at its top level out of twenty-two.
function Get-FileCount([string]$dir) {
    if (-not (Test-Path $dir)) { return -1 }
    return (Get-ChildItem $dir -Recurse -File).Count
}

function Get-PatchPaths([string]$path) {
    if (-not (Test-Path $path)) { return -1 }
    $n = 0
    foreach ($line in Get-Content $path) {
        if ($line -match '^diff --git ') { $n++ }
    }
    return $n
}

# -------------------------------------------------------------------- checks --
#
# Each entry says where the claim lives, what it claims, and how to measure it.
# Adding a number to the docs means adding it here; that is the point.

$componentDir = Join-Path $repo 'esp-idf\components\rtthread'
$freertosDir = Join-Path $componentDir 'port\freertos'
$bspDir = Join-Path $repo 'rt-thread\bsp\ESP\ESP32_S3'
$libcpuDir = Join-Path $repo 'rt-thread\libcpu\xtensa'
$idfPatch = Join-Path $repo 'esp-idf\patches\0001-idf-core-smp-hooks.patch'
$kernelPatch = Join-Path $repo 'rt-thread\patches\0001-kernel-smp-hooks.patch'

$checks = @(
    @{ Doc = 'PORTING.md'; Claim = 'compat layer, 4943 lines over 9 files';
       Expect = 4943; Actual = { (Get-SourceLinesIn $freertosDir '*.c') + (Get-SourceLinesIn $freertosDir '*.h') } }
    @{ Doc = 'PORTING.md'; Claim = 'compat layer, 9 files';
       Expect = 9; Actual = { Get-FileCount $freertosDir } }
    @{ Doc = 'PORTING.md'; Claim = 'freertos_api.c, 933 lines';
       Expect = 933; Actual = { Get-SourceLines (Join-Path $freertosDir 'freertos_api.c') } }
    @{ Doc = 'PORTING.md'; Claim = 'ft_ipc.c, 1231 lines';
       Expect = 1231; Actual = { Get-SourceLines (Join-Path $freertosDir 'ft_ipc.c') } }
    @{ Doc = 'PORTING.md'; Claim = 'ft_task.c, 988 lines';
       Expect = 988; Actual = { Get-SourceLines (Join-Path $freertosDir 'ft_task.c') } }
    @{ Doc = 'PORTING.md'; Claim = 'ft_notify.c, 414 lines';
       Expect = 414; Actual = { Get-SourceLines (Join-Path $freertosDir 'ft_notify.c') } }
    @{ Doc = 'PORTING.md'; Claim = 'ft_event.c, 177 lines';
       Expect = 177; Actual = { Get-SourceLines (Join-Path $freertosDir 'ft_event.c') } }
    @{ Doc = 'PORTING.md'; Claim = 'ft_qset.c, 273 lines';
       Expect = 273; Actual = { Get-SourceLines (Join-Path $freertosDir 'ft_qset.c') } }
    @{ Doc = 'PORTING.md'; Claim = 'ft_query.c, 239 lines';
       Expect = 239; Actual = { Get-SourceLines (Join-Path $freertosDir 'ft_query.c') } }
    @{ Doc = 'PORTING.md'; Claim = 'ft_probe.c, 129 lines';
       Expect = 129; Actual = { Get-SourceLines (Join-Path $freertosDir 'ft_probe.c') } }
    # 22 and 21 are the counts the repository description states for the two
    # trees it ships. Changing either is a change to what "this repository"
    # means, so a mismatch is worth stopping on.
    @{ Doc = 'README.md'; Claim = 'components/rtthread, 22 files';
       Expect = 22; Actual = { Get-FileCount $componentDir } }
    @{ Doc = 'README.md'; Claim = 'bsp/ESP/ESP32_S3, 21 files';
       Expect = 21; Actual = { Get-FileCount $bspDir } }
    @{ Doc = 'PORTING.md'; Claim = 'IDF patch touches 17 files';
       Expect = 17; Actual = { Get-PatchPaths $idfPatch } }
    @{ Doc = 'PORTING.md'; Claim = 'kernel patch touches 4 files';
       Expect = 4; Actual = { Get-PatchPaths $kernelPatch } }
)

# ---------------------------------------------------------------- run + report --

Write-Host ""
$bad = 0

foreach ($c in $checks) {
    $actual = & $c.Actual
    if ($actual -eq $c.Expect) {
        Write-Host ("  ok    {0,-34} {1}" -f $c.Claim, $actual)
    } else {
        $bad++
        Write-Host ("  WRONG {0,-34} expected {1}, found {2}" -f $c.Claim, $c.Expect, $actual) -ForegroundColor Yellow
    }
}

Write-Host ""
if ($bad -eq 0) {
    Write-Host "  All $($checks.Count) structural claim(s) hold." -ForegroundColor Green
    Write-Host ""
    exit 0
}

Write-Host "  $bad of $($checks.Count) claim(s) are stale. Update the doc, or update" -ForegroundColor Yellow
Write-Host "  this list if the repository really did change shape." -ForegroundColor Yellow
Write-Host ""
exit 1
