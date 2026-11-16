# Fill in the ESP-IDF submodules that the RT-Thread fork's branch does not declare.
#
# Why this exists.
#
# docs/BUILD.md tells the reader to clone RT-Thread-packages/esp-idf at branch
# rtt-dev. That branch is 15 submodules deep; the official ESP-IDF tree at the
# same era is 24. The nine it drops are still referenced by their components'
# CMakeLists, so the first CMake configure dies on
#
#   Include directory '.../components/unity/unity/src' is not a directory
#
# before compiling a single file - naming a test framework that has nothing to do
# with this port.
#
# `EXCLUDE_COMPONENTS` does not help: project.cmake applies it while assembling
# the link list, which happens after each component has already run
# idf_component_register() and done its include-directory checks. Measured - with
# all eight names excluded the configure still failed on unity.
#
# So the directories have to exist. This script clones them at the commits the
# official tree records, which is the state a working build was verified against.
#
# Usage:
#   .\fill-idf-submodules.ps1 -IdfPath <IDF_PATH>
#   .\fill-idf-submodules.ps1 -IdfPath <IDF_PATH> -WhatIf

param(
    [Parameter(Mandatory = $true)][string]$IdfPath,
    # Print what would be done without touching anything.
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path (Join-Path $IdfPath 'components'))) {
    throw "$IdfPath does not look like an ESP-IDF tree (no components/)"
}

# Path, commit, upstream. The commits are the ones the official ESP-IDF tree
# pins; a rtt-dev build using them was verified to compile.
$subs = @(
    @{ Path = 'components/unity/unity'
       Sha  = 'bf560290f6020737eafaa8b5cbd2177c3956c03f'
       Url  = 'https://github.com/ThrowTheSwitch/Unity.git' }
    @{ Path = 'components/cmock/CMock'
       Sha  = 'eeecc49ce8af123cf8ad40efdb9673e37b56230f'
       Url  = 'https://github.com/ThrowTheSwitch/CMock.git' }
    @{ Path = 'components/spiffs/spiffs'
       Sha  = 'ad902cadceb39d0825a97e25ecb5867641f606ba'
       Url  = 'https://github.com/pellepl/spiffs.git' }
    @{ Path = 'components/protobuf-c/protobuf-c'
       Sha  = 'abc67a11c6db271bedbb9f58be85d6f4e2ea8389'
       Url  = 'https://github.com/protobuf-c/protobuf-c.git' }
    @{ Path = 'components/openthread/openthread'
       Sha  = '43cc05a9bcf780bd758bad53d897e2d88cf8cb75'
       Url  = 'https://github.com/espressif/openthread.git' }
    @{ Path = 'components/openthread/lib'
       Sha  = 'b7bc06ab3cb69d8058192f9177c01468880f4698'
       Url  = 'https://github.com/espressif/esp-thread-lib.git' }
    @{ Path = 'components/bootloader/subproject/components/micro-ecc/micro-ecc'
       Sha  = '24c60e243580c7868f4334a1ba3123481fe1aa48'
       Url  = 'https://github.com/kmackay/micro-ecc.git' }
    @{ Path = 'components/bt/controller/lib_esp32c2/esp32c2-bt-lib'
       Sha  = '426902b1423a093af9af7944a8572adcf335767f'
       Url  = 'https://github.com/espressif/esp32c2-bt-lib.git' }
    @{ Path = 'components/bt/controller/lib_esp32h2/esp32h2-bt-lib'
       Sha  = '6078f34fd0ad60e8cbe22c351726f7bc73b405b5'
       Url  = 'https://github.com/espressif/esp32h2-bt-lib.git' }
)

Write-Host ""
Write-Host "esp32s3-rtthread: ESP-IDF submodules rtt-dev does not declare"
Write-Host "  IDF : $IdfPath"
Write-Host ""

$filled = 0
$already = 0
$failed = @()

foreach ($s in $subs) {
    $dest = Join-Path $IdfPath ($s.Path -replace '/', '\')

    if (Test-Path $dest) {
        $entries = @(Get-ChildItem $dest -Force -ErrorAction SilentlyContinue)
        if ($entries.Count -gt 0) {
            Write-Host ("  present  {0}" -f $s.Path)
            $already++
            continue
        }
    }

    if ($WhatIf) {
        Write-Host ("  would clone {0} @ {1}" -f $s.Path, $s.Sha.Substring(0, 8))
        continue
    }

    Write-Host ("  cloning  {0} ..." -f $s.Path)
    if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Path $dest -Force | Out-Null }

    # A shallow clone of the exact commit keeps this from pulling whole histories
    # for nine repositories, several of which are large.
    # Each step's exit code is read before the next command runs: $LASTEXITCODE
    # is global and `| Out-Null` around a native command still leaves it pointing
    # at whatever ran last, which is how the first version reported "9 fetched"
    # and then exited 1.
    $ok = $true
    & git -C $dest init --quiet 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { $ok = $false }
    if ($ok) {
        & git -C $dest remote add origin $s.Url 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { $ok = $false }
    }
    if ($ok) {
        & git -C $dest fetch --quiet --depth 1 origin $s.Sha 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { $ok = $false }
    }
    if ($ok) {
        & git -C $dest checkout --quiet FETCH_HEAD 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { $ok = $false }
    }
    if ($ok) {
        Write-Host ("           ok" )
        $filled++
    } else {
        Write-Host ("           FAILED - clone it by hand from {0}" -f $s.Url) -ForegroundColor Yellow
        $failed += $s.Path
    }
}

Write-Host ""
if ($failed.Count -gt 0) {
    Write-Host "  $($failed.Count) could not be fetched:" -ForegroundColor Yellow
    foreach ($f in $failed) { Write-Host "    $f" -ForegroundColor Yellow }
    Write-Host ""
    Write-Host "  They are on github.com - clone each one into its path at the commit"
    Write-Host "  listed in this script if the network blocks the shallow fetch above."
    Write-Host ""
    exit 1
}

Write-Host "  $already already present, $filled fetched."
Write-Host ""

if ($WhatIf) {
    Write-Host "  -WhatIf given; nothing was changed."
    Write-Host ""
}

exit 0
