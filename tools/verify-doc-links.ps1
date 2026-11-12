# Check that the documentation's cross-references still point at something.
#
# Why: the files in docs/ have grown from a few sections to well over a hundred
# across 27 numbered entries in KNOWN-ISSUES.md alone. Every round adds, renames
# and renumbers. Links are the first thing to break silently, because nothing
# reads a Markdown link except a person clicking it.
#
# Three kinds of reference are checked:
#
#   1. file links         [text](other.md)     - the target must exist
#   2. numbered entries   "di N tiao"          - KNOWN-ISSUES.md must have N
#   3. numbered sections  "di N jie" / "section N" - the named doc must have it
#
# (2) and (3) are checked against the headings of the target document rather than
# against a hard-coded range, so renumbering the target keeps them honest.
#
# NOTE ON ENCODING. This file is pure ASCII on purpose.
#
# The Chinese patterns it looks for are assembled from code points below rather
# than written literally. Windows PowerShell 5.1 reads a BOM-less script as ANSI,
# so a UTF-8 literal such as the character for "entry" became two garbage bytes
# that terminated a string early and broke the parser:
#
#   At verify-doc-links.ps1:119 char:31
#   Missing ')' in method call.
#
# The repository forbids BOMs (they broke .gitattributes once), so the fix is not
# to add one. Every other script here is ASCII-only and therefore unaffected;
# this one was the first to need literal CJK and the first to hit it.
#
# Usage:
#   .\verify-doc-links.ps1
#   .\verify-doc-links.ps1 -RepoRoot <path>

param(
    [string]$RepoRoot = ''
)

$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
if (-not (Test-Path $RepoRoot)) { throw "repository not found: $RepoRoot" }

# --- the CJK the patterns need, built from code points ------------------------

$DI  = [char]0x7B2C   # "di"    as in "di N tiao"
$TIAO = [char]0x6761  # "tiao"  as in "di N tiao"
$JIE = [char]0x8282   # "jie"   as in "di N jie"

# "di N tiao"
$EntryPattern = '{0}\s*(\d+)\s*{1}' -f $DI, $TIAO
# "di N jie"  or  "section N"
$SectionPattern = '({0}\s*(\d+)\s*{1})|(?<![\w-])section\s+(\d+)' -f $DI, $JIE

$docDirs = @($RepoRoot, (Join-Path $RepoRoot 'docs'), (Join-Path $RepoRoot 'tools'))
$mdFiles = @()
foreach ($d in $docDirs) {
    if (-not (Test-Path $d)) { continue }
    $mdFiles += Get-ChildItem $d -File -Filter '*.md'
}

if ($mdFiles.Count -eq 0) { throw "no markdown under $RepoRoot" }

Write-Host ""
Write-Host "  $($mdFiles.Count) markdown file(s) under $RepoRoot"
Write-Host ""

$problems = @()

# ------------------------------------------------------------- 1. file links -

# A link shown inside a code fence or inline code is an example, not a
# reference. tools/README.md documents this repository's own link syntax, so
# without this the checker reports its own documentation as broken:
#
#   README.md:271  [file link]  other.md  ->  .../tools/other.md
#
# Strip fenced blocks and inline code before matching. Line numbers are
# preserved by replacing the stripped text with blank lines of the same shape.
function Get-ProseLines([string]$text) {
    $out = @()
    $inFence = $false
    foreach ($line in ($text -split "`r?`n")) {
        if ($line -match '^\s*```') {
            $inFence = -not $inFence
            $out += ''
            continue
        }
        if ($inFence) { $out += ''; continue }
        # Drop inline code spans; keep the rest of the line.
        $out += ($line -replace '`[^`]*`', '``')
    }
    return $out
}

foreach ($f in $mdFiles) {
    $text = Get-Content $f.FullName -Raw -Encoding UTF8
    $prose = Get-ProseLines $text
    $lineNo = 0
    foreach ($line in $prose) {
        $lineNo++
        foreach ($m in [regex]::Matches($line, '\[([^\]]*)\]\(([^)]+)\)')) {
            $target = $m.Groups[2].Value.Trim()

            # External, in-page anchors, and mailto are not this script's business.
            if ($target -match '^(https?:|mailto:|#)') { continue }

            $path = ($target -split '#')[0]
            if (-not $path) { continue }

            $resolved = [System.IO.Path]::GetFullPath(
                [System.IO.Path]::Combine($f.DirectoryName, $path))

            if (-not (Test-Path $resolved)) {
                $problems += [pscustomobject]@{
                    File   = $f.Name
                    Line   = $lineNo
                    Kind   = 'file link'
                    Detail = "$target  ->  $resolved"
                }
            }
        }
    }
}

# --------------------------------------------------- collect heading numbers --

function Get-HeadingNumbers([string]$path, [string]$pattern) {
    if (-not (Test-Path $path)) { return @() }
    $text = Get-Content $path -Raw -Encoding UTF8
    $nums = @()
    foreach ($m in [regex]::Matches($text, $pattern)) {
        $nums += [int]$m.Groups[1].Value
    }
    return $nums
}

$knownIssues = Join-Path $RepoRoot 'docs\KNOWN-ISSUES.md'
$kiNums = Get-HeadingNumbers $knownIssues '(?m)^##\s+(\d+)\.'
$kiMax = if ($kiNums.Count) { ($kiNums | Measure-Object -Maximum).Maximum } else { 0 }

Write-Host "  KNOWN-ISSUES.md has $($kiNums.Count) numbered entries, highest is $kiMax"

# Top-level section numbers per document, so a named reference can be resolved.
$sectionMap = @{}
foreach ($f in $mdFiles) {
    $text = Get-Content $f.FullName -Raw -Encoding UTF8
    $nums = @()
    foreach ($m in [regex]::Matches($text, '(?m)^##\s+(\d+)[a-z]?\.')) {
        $nums += [int]$m.Groups[1].Value
    }
    if ($nums.Count) { $sectionMap[$f.Name] = $nums }
}

# ----------------------------------------------------- 2. numbered entries ---

foreach ($f in $mdFiles) {
    $text = Get-Content $f.FullName -Raw -Encoding UTF8
    $prose = Get-ProseLines $text
    $lineNo = 0
    foreach ($line in $prose) {
        $lineNo++
        foreach ($m in [regex]::Matches($line, $EntryPattern)) {
            $n = [int]$m.Groups[1].Value
            if ($n -gt $kiMax) {
                $problems += [pscustomobject]@{
                    File   = $f.Name
                    Line   = $lineNo
                    Kind   = 'entry ref'
                    Detail = "entry $n, but KNOWN-ISSUES.md only goes to $kiMax"
                }
            }
        }
    }
}

# ---------------------------------------------------- 3. numbered sections ---

foreach ($f in $mdFiles) {
    $text = Get-Content $f.FullName -Raw -Encoding UTF8
    $lines = Get-ProseLines $text

    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]

        # Which document does this reference name, if any?
        $docName = $null
        foreach ($cand in $sectionMap.Keys) {
            if ($line -match [regex]::Escape($cand)) { $docName = $cand; break }
        }

        if (-not $docName) { continue }
        $nums = $sectionMap[$docName]

        foreach ($m in [regex]::Matches($line, $SectionPattern)) {
            $n = if ($m.Groups[2].Success) { [int]$m.Groups[2].Value } else { [int]$m.Groups[3].Value }

            if ($nums -notcontains $n) {
                $have = ($nums | Sort-Object) -join ', '
                $problems += [pscustomobject]@{
                    File   = $f.Name
                    Line   = $i + 1
                    Kind   = 'section ref'
                    Detail = "$docName has no section $n (has: $have)"
                }
            }
        }
    }
}

# ------------------------------------------------------------------- report --

if ($problems.Count -eq 0) {
    Write-Host ""
    Write-Host "  Every cross-reference resolves." -ForegroundColor Green
    Write-Host ""
    exit 0
}

Write-Host ""
Write-Host "  BROKEN REFERENCES ($($problems.Count)):" -ForegroundColor Yellow
foreach ($p in $problems) {
    Write-Host ("    {0}:{1}  [{2}]  {3}" -f $p.File, $p.Line, $p.Kind, $p.Detail) -ForegroundColor Yellow
}
Write-Host ""
Write-Host "  A link that resolves to nothing is worse than no link: it looks like help" -ForegroundColor DarkGray
Write-Host "  and gives none. Fix the reference or remove it." -ForegroundColor DarkGray
Write-Host ""

exit 1
