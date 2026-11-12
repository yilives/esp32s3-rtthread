# Look for code that appears more than once.
#
# Why: four bugs in this port came from the same shape - one piece of logic
# written twice, a guard added to one copy, the other left behind. `s3ble
# deinit` open-coded the teardown that s3ble_shutdown_quiet() implements and
# missed the shutdown flag; s3wifi and s3wifiscan each created the station netif
# and only one got the "already exists" check; s3net guarded one branch of two.
#
# This does not find those bugs. It finds the duplication that makes them
# possible, so a fix can be applied to one place instead of remembered in
# several.
#
# Two views, because neither alone is useful:
#   -blocks   identical runs of N non-comment lines (finds copy-paste)
#   -calls    identical runs of N IDF/RT-Thread API calls (finds rewritten copies)
#
# Both skip rt_kprintf and friends, which appear everywhere and drown the signal.
#
# Usage:
#   .\find-duplication.ps1
#   .\find-duplication.ps1 -Mode blocks -MinLines 8
#   .\find-duplication.ps1 -Mode calls -MinLines 5

param(
    [ValidateSet('blocks', 'calls', 'both')]
    [string]$Mode = 'both',
    [int]$MinLines = 6,
    [string]$Project = ''
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Project) { $Project = Join-Path $repo 'project\s3-rtt-test' }
$srcDir = Join-Path $Project 'main'
if (-not (Test-Path $srcDir)) { throw "no main/ under $Project" }

$files = Get-ChildItem $srcDir -File -Filter '*.c'
if ($files.Count -eq 0) { throw "no .c files in $srcDir" }

# Calls that say nothing about structure.
$skipCalls = @(
    'rt_kprintf', 'rt_memset', 'rt_strcmp', 'rt_strlen', 'rt_snprintf',
    'rt_memcpy', 'rt_malloc', 'rt_free'
)

function Get-Lines([string]$path) {
    (Get-Content $path -Raw -Encoding UTF8) -split "`r?`n"
}

function Test-Noise([string]$s) {
    return ($s -eq '' -or $s.StartsWith('//') -or $s.StartsWith('*') -or
            $s.StartsWith('/*') -or $s.StartsWith('#'))
}

$found = @()

if ($Mode -eq 'blocks' -or $Mode -eq 'both') {
    $map = @{}
    foreach ($f in $files) {
        $lines = Get-Lines $f.FullName
        $norm = New-Object System.Collections.ArrayList
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $s = $lines[$i].Trim()
            if (Test-Noise $s) { continue }
            [void]$norm.Add(@(($i + 1), $s))
        }
        for ($i = 0; $i -le $norm.Count - $MinLines; $i++) {
            $key = (@(0..($MinLines - 1) | ForEach-Object { $norm[$i + $_][1] }) -join "`n")
            if (-not $map.ContainsKey($key)) { $map[$key] = @() }
            $map[$key] += ,@($f.Name, $norm[$i][0])
        }
    }
    foreach ($k in $map.Keys) {
        if ($map[$k].Count -lt 2) { continue }
        $found += [pscustomobject]@{
            Kind    = 'block'
            Where   = ($map[$k] | ForEach-Object { "$($_[0]):$($_[1])" }) -join ', '
            Snippet = ($k -split "`n")[0]
        }
    }
}

if ($Mode -eq 'calls' -or $Mode -eq 'both') {
    $map = @{}
    foreach ($f in $files) {
        $lines = Get-Lines $f.FullName
        $seq = New-Object System.Collections.ArrayList
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $s = $lines[$i].Trim()
            if (Test-Noise $s) { continue }
            foreach ($m in [regex]::Matches($s, '\b((?:esp|nimble|ble|rt|nvs|wl)_[a-z0-9_]+)\s*\(')) {
                $name = $m.Groups[1].Value
                if ($skipCalls -contains $name) { continue }
                [void]$seq.Add(@(($i + 1), $name))
            }
        }
        for ($i = 0; $i -le $seq.Count - $MinLines; $i++) {
            $key = (@(0..($MinLines - 1) | ForEach-Object { $seq[$i + $_][1] }) -join ' -> ')
            if (-not $map.ContainsKey($key)) { $map[$key] = @() }
            $map[$key] += ,@($f.Name, $seq[$i][0])
        }
    }
    foreach ($k in $map.Keys) {
        if ($map[$k].Count -lt 2) { continue }
        $found += [pscustomobject]@{
            Kind    = 'calls'
            Where   = ($map[$k] | ForEach-Object { "$($_[0]):$($_[1])" }) -join ', '
            Snippet = $k
        }
    }
}

Write-Host ""
Write-Host "  $($files.Count) file(s), window $MinLines"
Write-Host ""

if ($found.Count -eq 0) {
    Write-Host "  No duplication above the threshold." -ForegroundColor Green
    Write-Host ""
    exit 0
}

foreach ($g in ($found | Group-Object Kind)) {
    Write-Host "  $($g.Name) ($($g.Count))" -ForegroundColor Yellow
    foreach ($item in $g.Group) {
        Write-Host "    $($item.Where)"
        Write-Host "      $($item.Snippet)" -ForegroundColor DarkGray
    }
    Write-Host ""
}

Write-Host "  Duplication is not a bug. It is where a guard gets added to one copy and" -ForegroundColor DarkGray
Write-Host "  not the other - check each hit for logic that should be shared." -ForegroundColor DarkGray
Write-Host ""
exit 0
