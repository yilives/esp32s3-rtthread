# Launch QEMU with a freshly merged flash image, frozen for GDB.
#
# Why this exists: a hand-rolled qemu-system-xtensa invocation does NOT
# regenerate qemu_flash.bin - qemu.ps1 does that. Launching by hand therefore
# risks handing GDB an ELF from one build and running an image from another,
# which produces GDB results that contradict what the console says.
#
# So the flash image comes from qemu.ps1 (-MergedFlash), not from a second copy
# of the merge logic. An earlier version of this file did duplicate it, and the
# copies diverged: this one assembled a -drive line without `format=raw` and
# assumed qemu_efuse.bin already existed, so on a clean build QEMU warned about
# probing the image format instead of opening it as raw.
#
# Usage:
#   .\qemu-gdb.ps1              # start frozen (-S), then attach GDB yourself
#   .\qemu-gdb.ps1 -Run         # start running, useful for a plain boot check
#
# Then, in another shell:
#   xtensa-esp32s3-elf-gdb <build-qemu>/s3_rtt_test.elf
#   (gdb) target remote localhost:1234
#
# Ctrl-A X quits QEMU.

param(
    [switch]$Run,
    [string]$Project = '',
    [string]$BuildDir = 'build-qemu',
    [int]$WaitSeconds = 3
)

$ErrorActionPreference = 'Stop'

# Same discovery as build.ps1 and qemu.ps1, shared rather than copied.
. (Join-Path $PSScriptRoot 'env.ps1')

$repo = $script:S3Repo

if (-not $Project) {
    if ($env:S3_PROJECT) {
        $Project = $env:S3_PROJECT
    } else {
        foreach ($candidate in @("$repo\project\s3-rtt-test", "$repo\s3-rtt-test")) {
            if (Test-Path (Join-Path $candidate 'CMakeLists.txt')) { $Project = $candidate; break }
        }
    }
}
if (-not $Project) { throw "No ESP32-S3 project found. Pass -Project <path>." }
$Project = (Resolve-Path $Project).Path

$bd = Join-Path $Project $BuildDir
if (-not (Test-Path (Join-Path $bd 's3_rtt_test.elf'))) {
    throw "No build in $bd - run build.ps1 -NoPsram first."
}

if (-not $script:S3ToolDirs['qemu']) {
    throw "qemu-system-xtensa.exe not found under $env:ESP_IDF_TOOLS."
}
$qemu = Join-Path $script:S3ToolDirs['qemu'] 'qemu-system-xtensa.exe'

# ---------------------------------------------------------- flash + efuse ---
#
# Both come from qemu.ps1 so there is exactly one implementation of how the
# image is assembled and how the eFuse drive is shaped.

& (Join-Path $PSScriptRoot 'qemu.ps1') -Project $Project -BuildDir $BuildDir -MergedFlash
if ($LASTEXITCODE -ne 0) { throw "qemu.ps1 -MergedFlash failed" }

$flash = Join-Path $bd 'qemu_flash.bin'
$efuse = Join-Path $bd 'qemu_efuse.bin'

if (-not (Test-Path $flash)) { throw "qemu.ps1 did not produce $flash" }
if (-not (Test-Path $efuse)) { throw "qemu.ps1 did not produce $efuse" }

# ------------------------------------------------------------------ launch ---

Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

# argv mirrors QEMU_TARGETS['esp32s3'] in esp-idf/tools/idf_py_actions/qemu_ext.py.
# `format=raw` on both drives matters: without it QEMU probes the image and
# warns that write access to block 0 is restricted.
$qargs = @(
    '-M', 'esp32s3', '-m', '32M',
    '-drive', "file=$flash,if=mtd,format=raw",
    '-drive', "file=$efuse,if=none,format=raw,id=efuse",
    '-global', 'driver=nvram.esp32s3.efuse,property=drive,value=efuse',
    '-global', 'driver=timer.esp32s3.timg,property=wdt_disable,value=true',
    '-nic', 'user,model=open_eth',
    '-nographic',
    '-serial', 'mon:stdio',
    '-s'
)
if (-not $Run) { $qargs += '-S' }

$logOut = Join-Path $bd 'qemu-gdb.out.log'
$logErr = Join-Path $bd 'qemu-gdb.err.log'

# -PassThru so the process can be checked and reported on. Without it the caller
# has no handle, and a QEMU that died immediately looks the same as one that is
# running - which is how a missing input file stayed hidden.
$proc = Start-Process -FilePath $qemu -ArgumentList $qargs -NoNewWindow -PassThru `
    -RedirectStandardOutput $logOut -RedirectStandardError $logErr

Start-Sleep -Seconds $WaitSeconds

if ($proc.HasExited) {
    $err = if (Test-Path $logErr) { Get-Content $logErr -Raw } else { '' }
    throw "QEMU exited immediately (code $($proc.ExitCode)).`n$err"
}

# The GDB stub listens on 1234. Confirm it, because "process alive" and "stub
# reachable" are different claims and only the second one is useful.
$listening = $false
for ($i = 0; $i -lt 20; $i++) {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $client.Connect('127.0.0.1', 1234)
        $listening = $true
        break
    } catch {
        Start-Sleep -Milliseconds 200
    } finally {
        $client.Dispose()
    }
}

Write-Host ""
Write-Host ("qemu   : pid {0}" -f $proc.Id)
Write-Host ("flash  : {0} bytes, {1}" -f (Get-Item $flash).Length, (Get-Item $flash).LastWriteTime)
Write-Host ("elf    : {0}" -f (Get-Item (Join-Path $bd 's3_rtt_test.elf')).LastWriteTime)
Write-Host ("gdb    : {0}" -f $(if ($listening) { 'listening on localhost:1234' } else { "NOT listening - see $logErr" }))
Write-Host ("state  : {0}" -f $(if ($Run) { 'running' } else { 'frozen at reset (-S), waiting for GDB' }))
Write-Host ""

if (-not $listening) {
    throw "QEMU is running but the GDB stub is not reachable. See $logErr"
}

if ($Run) {
    Write-Host "QEMU is running in the background. Ctrl-A X in its console quits it," -ForegroundColor DarkGray
    Write-Host "or stop it with: Get-Process qemu-system-xtensa | Stop-Process -Force" -ForegroundColor DarkGray
} else {
    Write-Host "Attach with:" -ForegroundColor DarkGray
    Write-Host ("  {0}\xtensa-esp32s3-elf-gdb.exe `"$bd\s3_rtt_test.elf`"" -f $script:S3ToolDirs['gdb']) -ForegroundColor DarkGray
    Write-Host  "  (gdb) target remote localhost:1234" -ForegroundColor DarkGray
}
