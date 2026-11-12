# Compare the commands the firmware exports against the ones the docs claim.
#
# Why: the roadmap claimed "BLE then Wi-Fi is broken, in progress" for several
# versions after it had been fixed. Prose does not fail to compile, so nothing
# catches it. Command names are checkable though - if documentation tells a
# reader to type `s3foo` and no such command is exported, that is a factual
# error a script can find.
#
# What it checks:
#   1. every command the code exports is mentioned somewhere in the docs
#   2. every `msh >xxx` in the docs corresponds to an exported command, or to
#      one of the kernel-provided ones in $KernelCommands
#
# It does NOT check whether the documented output is correct - that is
# check-docs.ps1's job, and it needs a board.
#
# Usage:
#   .\verify-doc-commands.ps1
#   .\verify-doc-commands.ps1 -Project <path to s3-rtt-test>

param(
    [string]$Project = '',
    [string]$RepoRoot = ''
)

$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
if (-not $Project) {
    $Project = Join-Path $RepoRoot 'project\s3-rtt-test'
}
if (-not (Test-Path $Project)) { throw "project not found: $Project" }

. (Join-Path $PSScriptRoot 'env.ps1')

# Commands the RT-Thread kernel provides itself; they are exported by the
# kernel, not by this port, so they will never appear in the application's
# sources.
$KernelCommands = @(
    'free', 'list', 'list_thread', 'ps', 'help', 'version', 'date', 'df',
    'mkdir', 'rm', 'cat', 'cd', 'pwd', 'ls', 'echo', 'clear', 'reboot'
)

function Get-ExportedCommands([string[]]$roots) {
    $found = @{}
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        Get-ChildItem $root -Recurse -File -Include '*.c' | ForEach-Object {
            $text = Get-Content $_.FullName -Raw -Encoding UTF8
            foreach ($m in [regex]::Matches($text, 'MSH_CMD_EXPORT_ALIAS\s*\(\s*\w+\s*,\s*(\w+)\s*,')) {
                $found[$m.Groups[1].Value] = $_.Name
            }
            foreach ($m in [regex]::Matches($text, 'MSH_CMD_EXPORT\s*\(\s*(\w+)\s*,')) {
                $found[$m.Groups[1].Value] = $_.Name
            }
        }
    }
    return $found
}

# Application commands and board-level commands live in different trees.
$roots = @(
    (Join-Path $Project 'main'),
    (Join-Path $RepoRoot 'rt-thread\bsp\ESP\ESP32_S3\board')
)
$exported = Get-ExportedCommands $roots

if ($exported.Count -eq 0) {
    throw "no MSH_CMD_EXPORT found under $($roots -join ', ') - is -Project right?"
}

# Everything a reader is told to type.
$docFiles = @(Join-Path $RepoRoot 'README.md')
$docFiles += (Get-ChildItem (Join-Path $RepoRoot 'docs') -File -Filter '*.md' | ForEach-Object { $_.FullName })

$mentioned = @{}
foreach ($f in $docFiles) {
    if (-not (Test-Path $f)) { continue }
    $text = Get-Content $f -Raw -Encoding UTF8
    foreach ($m in [regex]::Matches($text, '(?m)^\s*msh\s*>\s*(\w+)')) {
        $cmd = $m.Groups[1].Value
        if (-not $mentioned.ContainsKey($cmd)) { $mentioned[$cmd] = $f }
    }
}

Write-Host ""
Write-Host "  code exports $($exported.Count) command(s)"
Write-Host "  docs invoke  $($mentioned.Count) command(s) via 'msh >'"
Write-Host ""

$bad = 0

# 1. Documented but not exported.
$unknown = @()
foreach ($cmd in $mentioned.Keys) {
    if ($exported.ContainsKey($cmd)) { continue }
    if ($KernelCommands -contains $cmd) { continue }
    $unknown += $cmd
}
if ($unknown.Count -gt 0) {
    $bad += $unknown.Count
    Write-Host "  DOCUMENTED BUT NOT EXPORTED - these would fail if typed:" -ForegroundColor Yellow
    foreach ($cmd in ($unknown | Sort-Object)) {
        $file = Split-Path -Leaf $mentioned[$cmd]
        Write-Host ("    {0,-16} mentioned in {1}" -f $cmd, $file) -ForegroundColor Yellow
    }
    Write-Host ""
}

# 2. Exported but never documented. Not an error - some commands are
#    diagnostics - but worth listing so the omission is a decision.
$undocumented = @()
foreach ($cmd in $exported.Keys) {
    $hit = $false
    foreach ($f in $docFiles) {
        if (-not (Test-Path $f)) { continue }
        $text = Get-Content $f -Raw -Encoding UTF8
        if ($text -match "\b$([regex]::Escape($cmd))\b") { $hit = $true; break }
    }
    if (-not $hit) { $undocumented += $cmd }
}
if ($undocumented.Count -gt 0) {
    Write-Host "  EXPORTED BUT NEVER MENTIONED (not an error, just a choice):" -ForegroundColor DarkGray
    foreach ($cmd in ($undocumented | Sort-Object)) {
        Write-Host ("    {0,-16} defined in {1}" -f $cmd, $exported[$cmd]) -ForegroundColor DarkGray
    }
    Write-Host ""
}

if ($bad -eq 0) {
    Write-Host "  Every command the docs tell a reader to type exists." -ForegroundColor Green
    Write-Host ""
    exit 0
}

Write-Host "RESULT: FAIL - $bad documented command(s) are not exported" -ForegroundColor Yellow
exit 1
