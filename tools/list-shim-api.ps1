# List the FreeRTOS APIs the compatibility layer actually implements.
#
# Why: docs/PORTING.md describes how FreeRTOS calls are mapped onto RT-Thread
# primitives. That description can drift the same way the command docs did - the
# porting guide still showed FT_IRAM guarded by __XTENSA__ months after it became
# unconditional, and claimed 3751 lines when there were 4204.
#
# This prints what is really there, so the table in PORTING.md can be checked
# against it rather than against memory.
#
# Usage:
#   .\list-shim-api.ps1                 # grouped summary
#   .\list-shim-api.ps1 -All            # every function name
#   .\list-shim-api.ps1 -Missing        # names documented but not implemented

param(
    [switch]$All,
    [switch]$Missing,
    [string]$IdfPath = ''
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $IdfPath) {
    if ($env:IDF_PATH) { $IdfPath = $env:IDF_PATH }
    else {
        foreach ($c in @("$repo\..\esp-idf-rtt", "$repo\..\esp-idf")) {
            if (Test-Path (Join-Path $c 'tools\idf.py')) { $IdfPath = $c; break }
        }
    }
}
if (-not $IdfPath -or -not (Test-Path $IdfPath)) { throw "ESP-IDF tree not found; pass -IdfPath." }
$IdfPath = (Resolve-Path $IdfPath).Path

$shim = Join-Path $IdfPath 'components\rtthread\port\freertos'
if (-not (Test-Path $shim)) { throw "shim not found at $shim" }

# FreeRTOS naming: x* returns a value, v* returns void. Anchored at column 0 so
# definitions are matched rather than call sites.
$funcs = @{}
foreach ($f in Get-ChildItem $shim -File -Filter '*.c') {
    $text = Get-Content $f.FullName -Raw -Encoding UTF8
    foreach ($m in [regex]::Matches($text, '(?m)^[A-Za-z_][\w \*]*\b([xv][A-Z]\w+)\s*\(')) {
        $funcs[$m.Groups[1].Value] = $f.Name
    }
}

if ($funcs.Count -eq 0) { throw "no FreeRTOS-style definitions found in $shim" }

function Get-Category([string]$n) {
    if ($n -match '^(x|v)Queue|Semaphore|Mutex') { return 'Queue / Semaphore / Mutex' }
    if ($n -match 'Notify')                      { return 'Task notification' }
    if ($n -match 'EventGroup')                  { return 'Event group' }
    if ($n -match '^(x|v)Task|Scheduler')        { return 'Task / scheduler' }
    return 'Other'
}

$byCategory = $funcs.Keys | Group-Object { Get-Category $_ } | Sort-Object Name

if ($Missing) {
    # Every FreeRTOS API named in PORTING.md's prose and tables.
    $doc = Join-Path $repo 'docs\PORTING.md'
    if (-not (Test-Path $doc)) { throw "missing $doc" }
    $text = Get-Content $doc -Raw -Encoding UTF8
    $named = [regex]::Matches($text, '\b([xv][A-Z]\w+)\b') |
             ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique

    # A name is accounted for if it is defined in the shim, is a macro in a
    # FreeRTOS header, or is defined by IDF's own freertos port. All three are
    # legitimate - xTaskCreate is a macro over xTaskCreatePinnedToCore,
    # xPortGetCoreID lives in the port, vSystimerSetup in port_systick.c.
    $searchRoots = @(
        (Join-Path $IdfPath 'components\rtthread'),
        (Join-Path $IdfPath 'components\freertos')
    )

    function Test-AccountedFor([string]$name) {
        if ($funcs.ContainsKey($name)) { return $true }
        foreach ($r in $searchRoots) {
            if (-not (Test-Path $r)) { continue }
            foreach ($f in Get-ChildItem $r -Recurse -File -Include '*.c', '*.h') {
                $text = Get-Content $f.FullName -Raw -Encoding UTF8
                if ($text -match "(?m)^\s*#\s*define\s+$([regex]::Escape($name))\b") { return $true }
                if ($text -match "(?m)^[A-Za-z_][\w \*]*\b$([regex]::Escape($name))\s*\(") { return $true }
            }
        }
        return $false
    }

    $absent = @()
    foreach ($n in $named) {
        if (Test-AccountedFor $n) { continue }
        $absent += $n
    }
    if ($absent.Count -eq 0) {
        Write-Host ""
        Write-Host "  Every x*/v* name in PORTING.md is defined somewhere: shim, a" -ForegroundColor Green
        Write-Host "  FreeRTOS macro, or IDF's own freertos port." -ForegroundColor Green
        Write-Host ""
        exit 0
    }
    Write-Host ""
    Write-Host "  NAMED IN PORTING.md, NOT DEFINED IN THE SHIM:" -ForegroundColor Yellow
    foreach ($n in $absent) { Write-Host "    $n" -ForegroundColor Yellow }
    Write-Host ""
    Write-Host "  Either the doc is stale, or the name is a macro in a header." -ForegroundColor DarkGray
    Write-Host "  Check include/freertos/ before changing the doc." -ForegroundColor DarkGray
    Write-Host ""
    exit 1
}

Write-Host ""
Write-Host "  shim: $shim"
Write-Host "  $($funcs.Count) FreeRTOS-style function(s), from $(($funcs.Values | Sort-Object -Unique).Count) file(s)"
Write-Host ""

if ($All) {
    foreach ($f in ($funcs.Keys | Sort-Object)) {
        Write-Host ("    {0,-42} {1}" -f $f, $funcs[$f])
    }
} else {
    foreach ($g in $byCategory) {
        Write-Host ("  {0} ({1})" -f $g.Name, $g.Count)
        foreach ($f in ($g.Group | Sort-Object)) { Write-Host "    $f" -ForegroundColor DarkGray }
        Write-Host ""
    }
}

exit 0
