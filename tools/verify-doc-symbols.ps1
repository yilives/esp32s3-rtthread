# Check that the symbol names the documentation quotes actually exist in the code.
#
# Why: README's support tables name the RT-Thread interface behind each feature -
# "I2C | rt_i2c_bus | i2cscan". Those names are checkable, and one was wrong:
# the PWM row said rt_pwm_device where the code has rt_device_pwm. Same words,
# transposed - reads fine, resolves to nothing.
#
# Scope: only identifiers that look like this port's own interfaces - a
# rt_/heap_caps_/esp_ prefix inside backticks. Function names in prose, GDB
# commands and shell commands are left alone; verify-doc-commands.ps1 owns the
# shell commands and the rest are not mechanically checkable.
#
# This is a weaker check than it sounds. It proves a name appears somewhere in
# the sources, not that it is the right name for the row it sits in. The PWM bug
# would have been caught; a plausible-but-wrong pairing would not.
#
# Usage:
#   .\verify-doc-symbols.ps1
#   .\verify-doc-symbols.ps1 -RepoRoot <path>

param(
    [string]$RepoRoot = '',
    [string]$RtThreadPath = '',
    [string]$IdfPath = ''
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $RepoRoot) { $RepoRoot = $repo }

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

# Sources that could legitimately define or use such a symbol. The upstream
# kernel is included because the tables describe interfaces the kernel provides.
$roots = @(
    (Join-Path $RepoRoot 'rt-thread\bsp\ESP\ESP32_S3'),
    (Join-Path $RepoRoot 'rt-thread\libcpu\xtensa'),
    (Join-Path $RepoRoot 'esp-idf\components\rtthread'),
    (Join-Path $RepoRoot 'project\s3-rtt-test\main')
)
if ($RtThreadPath -and (Test-Path $RtThreadPath)) {
    $roots += (Join-Path $RtThreadPath 'src')
    $roots += (Join-Path $RtThreadPath 'include')
}
# The docs also name IDF APIs - esp_ota_begin, esp_pm, esp_ipc_call_nonblocking.
# Those live upstream, not here, and a name the docs quote from IDF is still a
# name a reader will look up. Headers only: this tree is large and the
# declarations are what matter.
if ($IdfPath -and (Test-Path (Join-Path $IdfPath 'components'))) {
    $roots += (Join-Path $IdfPath 'components')
}

$blob = New-Object System.Text.StringBuilder
$fileCount = 0
foreach ($r in $roots) {
    if (-not (Test-Path $r)) { continue }
    $isIdf = $IdfPath -and $r.StartsWith($IdfPath)
    foreach ($f in Get-ChildItem $r -Recurse -File) {
        if ($f.Extension -notin @('.c', '.h', '.S', '.cpp')) { continue }
        if ($isIdf -and $f.Extension -ne '.h') { continue }
        [void]$blob.Append((Get-Content $f.FullName -Raw -Encoding UTF8))
        $fileCount++
    }
}
$haystack = $blob.ToString()

if ($fileCount -eq 0) { throw "no sources found under: $($roots -join ', ')" }

# ------------------------------------------------------------------ extract --
#
# Backtick-quoted identifiers, and only those that look like this port's
# interfaces. A GDB command such as `p rt_tick` is not a bare identifier and will
# not match.

$mdFiles = @(Get-ChildItem $RepoRoot -File -Filter '*.md')
$docDir = Join-Path $RepoRoot 'docs'
if (Test-Path $docDir) { $mdFiles += Get-ChildItem $docDir -File -Filter '*.md' }

$symbols = @{}
foreach ($f in $mdFiles) {
    $text = Get-Content $f.FullName -Raw -Encoding UTF8
    $lineNo = 0
    foreach ($line in ($text -split "`r?`n")) {
        $lineNo++
        foreach ($m in [regex]::Matches($line, '`((?:rt_|heap_caps_|esp_)[a-z0-9_]+)`')) {
            $name = $m.Groups[1].Value

            # A name the prose says was REMOVED is not a claim that it exists.
            # PORTING.md lists "deleted the rt_xt_ip3 debug probe" among the IDF
            # changes, and that symbol is correctly absent from the tree. The
            # removal words are ASCII here on purpose - see the note at the top
            # of verify-doc-links.ps1 for why a UTF-8 literal in this file would
            # break the parser on Windows PowerShell 5.1.
            $context = $line
            if ($context -match '(delet|remov|drop|strip|no longer|not exist)' -or
                $context -match ([string][char]0x5220 + '|' + [string][char]0x79FB + '|移除')) {
                continue
            }

            if (-not $symbols.ContainsKey($name)) {
                $symbols[$name] = @()
            }
            $symbols[$name] += "$($f.Name):$lineNo"
        }
    }
}

Write-Host ""
Write-Host "  $($symbols.Count) quoted symbol(s) across $($mdFiles.Count) file(s)"
Write-Host "  searched $fileCount source file(s)"
Write-Host ""

$missing = @()
foreach ($name in ($symbols.Keys | Sort-Object)) {
    if ($haystack.Contains($name)) { continue }
    $missing += $name
}

if ($missing.Count -eq 0) {
    Write-Host "  Every quoted symbol exists in the sources." -ForegroundColor Green
    Write-Host ""
    exit 0
}

Write-Host "  QUOTED BUT NOT FOUND ($($missing.Count)):" -ForegroundColor Yellow
foreach ($name in $missing) {
    Write-Host ("    {0}" -f $name) -ForegroundColor Yellow
    foreach ($loc in $symbols[$name]) {
        Write-Host ("        {0}" -f $loc) -ForegroundColor DarkGray
    }
}
Write-Host ""
Write-Host "  Either the docs name something that does not exist, or it lives outside" -ForegroundColor DarkGray
Write-Host "  the trees searched here - widen `$roots if the latter." -ForegroundColor DarkGray
Write-Host ""

exit 1
