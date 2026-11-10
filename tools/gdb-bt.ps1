# Attach GDB to a running QEMU image, stop it, and dump where it was.
#
# Why this exists: several GDB commands deadlock on this target, and the
# failures look like "GDB hangs" rather than like errors. Measured:
#
#   p rt_thread_self()->parent.name     hangs - it is an inferior function
#                                       call, which needs code injected into
#                                       the target and cannot work against a
#                                       running QEMU
#   p rt_tick                           "No symbol" - RT-Thread SMP keeps the
#                                       tick inside struct rt_cpu, per core
#   p s3_ulp_completions                "No symbol" - statics are per-TU
#
# What does work: info threads, bt, reading globals, and reading registers.
# So this script halts, prints threads and backtraces, and leaves the rest to
# the user.
#
# Usage:
#   .\gdb-bt.ps1                       # attach to an already-running QEMU
#   .\gdb-bt.ps1 -Project <path>
#   .\gdb-bt.ps1 -StartQemu            # start QEMU -Run first, then attach
param(
    [string]$Project = '',
    [string]$BuildDir = 'build-qemu',
    [switch]$StartQemu,
    [string]$Extra = ''
)

$ErrorActionPreference = 'Stop'

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
$elf = Join-Path $bd 's3_rtt_test.elf'
if (-not (Test-Path $elf)) { throw "No ELF in $bd - run build.ps1 -NoPsram first." }

if (-not $script:S3ToolDirs['gdb']) { throw "xtensa-esp32s3-elf-gdb.exe not found under $env:ESP_IDF_TOOLS." }
$gdb = Join-Path $script:S3ToolDirs['gdb'] 'xtensa-esp32s3-elf-gdb.exe'

if ($StartQemu) {
    & (Join-Path $PSScriptRoot 'qemu-gdb.ps1') -Project $Project -BuildDir $BuildDir -Run
    Start-Sleep -Seconds 6
}

$cmds = @(
    'set pagination off',
    'set confirm off',
    'target remote localhost:1234',
    'monitor stop',
    'info threads',
    'thread apply all bt 6'
)
if ($Extra) { $cmds += $Extra -split ';' }
$cmds += @('detach', 'quit')

$tmp = Join-Path $env:TEMP 's3-gdb-cmds.txt'
$cmds -join "`n" | Set-Content $tmp -Encoding ascii

& $gdb --batch -x $tmp $elf 2>&1
exit $LASTEXITCODE
