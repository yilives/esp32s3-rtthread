# Launch QEMU with a freshly merged flash image, frozen for GDB.
#
# Written because a hand-rolled qemu-system-xtensa invocation does NOT regenerate
# qemu_flash.bin - qemu.ps1 does that itself. So the ELF handed to GDB and the
# image actually running could be different builds, which is exactly the kind of
# thing that produces contradictory GDB results.
param([switch]$Run)
$ErrorActionPreference = 'Stop'

$repo  = Split-Path -Parent $PSScriptRoot
$proj  = if ($env:S3_PROJECT) { $env:S3_PROJECT } else { Join-Path $repo 'project\s3-rtt-test' }
$bd    = Join-Path $proj 'build-qemu'

# Same discovery as build.ps1 and qemu.ps1, shared rather than copied.
. (Join-Path $PSScriptRoot 'env.ps1')

if (-not $script:S3ToolDirs['qemu']) {
    throw "qemu-system-xtensa.exe not found under $env:ESP_IDF_TOOLS."
}
$py   = $script:S3Python
$qemu = Join-Path $script:S3ToolDirs['qemu'] 'qemu-system-xtensa.exe'

$flash = Join-Path $bd 'qemu_flash.bin'

$bin = Join-Path $bd 's3_rtt_test.bin'
if (-not (Test-Path $bin)) { throw "No build in $bd - run build.ps1 -NoPsram first." }

# esptool wants space-separated address/file PAIRS, not 0xADDR=file.
& $py -m esptool --chip=esp32s3 merge-bin --output=$flash `
    --pad-to-size=8MB --flash-mode dio --flash-freq 80m --flash-size 8MB `
    0x0 (Join-Path $bd 'bootloader\bootloader.bin') `
    0x8000 (Join-Path $bd 'partition_table\partition-table.bin') `
    0x10000 $bin 2>&1 | Out-Null

if (-not (Test-Path $flash)) { throw "merge-bin did not produce $flash" }

Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$qargs = @('-M','esp32s3','-m','32M',
  '-drive',"file=$flash,if=mtd,format=raw",
  '-drive',"file=$(Join-Path $bd 'qemu_efuse.bin'),if=none,id=efuse",
  '-global','driver=nvram.esp32s3.efuse,property=drive,value=efuse',
  '-global','driver=timer.esp32s3.timg,property=wdt_disable,value=true',
  '-nic','user,model=open_eth','-nographic','-serial','mon:stdio','-s')
if (-not $Run) { $qargs += '-S' }

Start-Process -FilePath $qemu -ArgumentList $qargs -NoNewWindow `
  -RedirectStandardOutput "$env:TEMP\qemu-run.log" -RedirectStandardError "$env:TEMP\qemu-run.err" | Out-Null
Start-Sleep -Seconds 3

Write-Host ("flash: {0} bytes, {1}" -f (Get-Item $flash).Length, (Get-Item $flash).LastWriteTime)
Write-Host ("elf  : {0}" -f (Get-Item (Join-Path $bd 's3_rtt_test.elf')).LastWriteTime)
