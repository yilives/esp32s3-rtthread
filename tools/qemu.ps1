# Boot an ESP32-S3 ESP-IDF image in the bundled Espressif QEMU, from PowerShell.
#
# Two routes, and the choice is forced by whether stdin is a terminal:
#
#   1. `idf.py qemu` (interactive only). IDF builds build\qemu_flash.bin from
#      the build's flash_args, creates build\qemu_efuse.bin from the S3's default
#      eFuse image, and assembles argv from QEMU_TARGETS['esp32s3'] in
#      tools/idf_py_actions/qemu_ext.py. Use this when a human is at the
#      keyboard; Ctrl-A X quits QEMU.
#
#   2. Direct qemu-system-xtensa.exe with the same argv (any stdin). This is
#      what a harness/scheduled task/CI must use, because idf.py gives QEMU the
#      console with '-serial mon:stdio' and QEMU exits -- silently, exit code 0,
#      no serial output -- as soon as that stdin reports EOF.
#
# The argv both routes need for -M esp32s3, and why each part matters:
#   -m 32M
#   -drive file=qemu_flash.bin,if=mtd,format=raw     the merged flash image
#   -drive file=qemu_efuse.bin,if=none,id=efuse      + the -global below
#   -global driver=nvram.esp32s3.efuse,property=drive,value=efuse
#   -global driver=timer.esp32s3.timg,property=wdt_disable,value=true
#       ^ mandatory: without it the timer WDT resets the image during startup,
#         which looks like an instant boot loop with no application output.
#   -nic user,model=open_eth
#   -nographic -serial mon:stdio
#
# Usage:
#   .\qemu.ps1                                   # idf.py qemu, blocks (Ctrl-A X to quit)
#   .\qemu.ps1 -TimeoutSec 60                    # run 60 s, capture, kill, grep, PASS/FAIL
#   .\qemu.ps1 -Project I:\RT_thread\s3-rtt-test-baseline -TimeoutSec 60
#   .\qemu.ps1 -Direct                           # skip idf.py, run QEMU by hand
#   .\qemu.ps1 -MergedFlash                      # only produce build\qemu_flash.bin
#
# -TimeoutSec implies -Direct: a timed run kills QEMU, which idf.py would not
# survive cleanly. It writes build\qemu-serial.log and exits 0 on PASS, 1 on
# FAIL, so it can be used as a test gate.

param(
    [string]$Project = '',
    [string]$Target = 'esp32s3',
    [int]$TimeoutSec = 0,           # 0 = run in the foreground until QEMU exits
    # Build directory inside the project. 'build' is the hardware image;
    # build.ps1 -NoPsram produces the QEMU variant in 'build-qemu', because QEMU
    # does not model this module's octal PSRAM.
    [string]$BuildDir = 'build',
    [switch]$Direct,                # force the manual qemu-system-xtensa route
    [switch]$MergedFlash,           # only produce build\qemu_flash.bin, then stop
    [string]$QemuExtraArgs = '',
    # Regexes that must all match somewhere in the capture for RESULT: PASS,
    # and the lines typed into the guest console during a timed run.
    #
    # The defaults describe the bring-up image in this tree and are what the
    # gate actually checks, so a bare `qemu.ps1 -TimeoutSec N` is a full test:
    #
    #   - RT -     Thread Operating System   the kernel banner
    #   threads started                      the main thread reached main()
    #   verdict : PASS                       the selftest thread measured a
    #                                        3000-tick delta over three seconds
    #                                        with no FPU errors
    #   s3rt v0.1                            the shell answered a typed command,
    #                                        i.e. RX works, not just TX
    #   uart0 irq : N entries                the UART really interrupted, i.e.
    #                                        the vector, the trampoline and
    #                                        INTENABLE all work. This one is the
    #                                        reason input is interrupt driven:
    #                                        with a polling reader the whole
    #                                        interrupt API was untested.
    #
    # Note for a caller: `powershell -File script.ps1 -Expect a,b` collapses the
    # array into one string, because -File has no notion of PowerShell arrays.
    # Override them with -Command instead if that is ever needed; the defaults
    # exist so that the common case needs neither.
    [string[]]$Expect = @(
        '- RT -     Thread Operating System',
        'threads started',
        'verdict : PASS',
        's3rt v0\.1',
        'uart0 irq : [1-9][0-9]* entries'
    ),
    [string[]]$InputLine = @('s3rt', 's3rt irq', 's3rt tick', 'list thread', 'free'),
    # How long to wait before typing InputLine, so the shell prompt exists and
    # the selftest has printed its verdict.
    [int]$InputDelaySec = 12,
    [ValidateSet('run', 'none')]
    [string]$Efuse = 'run'
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot

$tools = if ($env:ESP_IDF_TOOLS) { $env:ESP_IDF_TOOLS }
         else { Join-Path $env:USERPROFILE '.espressif\tools' }

if (-not $Project) {
    foreach ($candidate in @("$repo\project\s3-rtt-test", "$repo\s3-rtt-test")) {
        if (Test-Path (Join-Path $candidate 'CMakeLists.txt')) { $Project = $candidate; break }
    }
}
if (-not $Project) { throw "No ESP32-S3 project found. Pass -Project <path>." }
$Project = (Resolve-Path $Project).Path

# ---------------------------------------------------------------------- env --
# Same recipe as build.ps1: idf.py qemu shells out to both esptool and
# qemu-system-xtensa, so the full tool PATH and the IDF env must be present.
if (-not $env:IDF_PATH) {
    foreach ($candidate in @("$repo\..\esp-idf-rtt", "$repo\..\esp-idf")) {
        if (Test-Path (Join-Path $candidate 'tools\idf.py')) {
            $env:IDF_PATH = (Resolve-Path $candidate).Path
            break
        }
    }
}
if (-not $env:IDF_PATH) { throw "IDF_PATH is not set and no ESP-IDF checkout was found next to this repo." }

if (-not $env:IDF_PYTHON_ENV_PATH) {
    $envRoot = Join-Path $env:USERPROFILE '.espressif\python_env'
    if (Test-Path $envRoot) {
        $venv = Get-ChildItem $envRoot -Directory -Filter 'idf*_env' -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($venv) { $env:IDF_PYTHON_ENV_PATH = $venv.FullName }
    }
}
if (-not $env:IDF_PYTHON_ENV_PATH) { throw "IDF_PYTHON_ENV_PATH is not set and no venv was found." }
$env:RTT_IDF_PATH = $env:IDF_PATH
# Must match build.ps1. idf.py qemu depends on "all", and a different
# CCACHE_ENABLE value makes CMake reconfigure from scratch and rebuild the
# whole IDF (minutes) before QEMU even starts.
$env:IDF_CCACHE_ENABLE = '1'
if (-not $env:LOCALAPPDATA) { $env:LOCALAPPDATA = Join-Path $env:USERPROFILE 'AppData\Local' }
if (-not $env:ESP_ROM_ELF_DIR) {
    $romElf = Join-Path $tools 'esp-rom-elfs'
    if (Test-Path $romElf) {
        $latest = Get-ChildItem $romElf -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($latest) { $env:ESP_ROM_ELF_DIR = $latest.FullName + '\' }
    }
}
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue

# idf.py warns (on stderr) when Python's stdio is not UTF-8. With a redirected
# console that is always the case, so force UTF-8 mode; the unbuffered switch
# matters for -TimeoutSec, where QEMU is killed and a buffered capture would be
# lost.
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$env:PYTHONUNBUFFERED = '1'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
cmd /c chcp 65001 | Out-Null

$qemuDir = "$tools\qemu-xtensa\esp_develop_9.2.2_20260417\qemu\bin"
$pathParts = @(
    "$tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin",
    "$tools\xtensa-esp-elf-gdb\17.1_20260402\xtensa-esp-elf-gdb\bin",
    "$tools\cmake\4.0.3\bin",
    "$tools\ninja\1.12.1",
    "$tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64",
    $qemuDir,
    "$env:IDF_PATH\components\espcoredump",
    "$env:IDF_PATH\components\partition_table",
    "$env:IDF_PATH\components\app_update",
    "$env:IDF_PYTHON_ENV_PATH\Scripts",
    "$env:IDF_PATH\tools"
)
$env:PATH = ($pathParts -join ';') + ';' + $env:PATH

$python = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$idfPy = Join-Path $env:IDF_PATH 'tools\idf.py'
$qemuExe = Join-Path $qemuDir 'qemu-system-xtensa.exe'
$buildDir = Join-Path $Project $BuildDir
$flashBin = Join-Path $buildDir 'qemu_flash.bin'
$efuseBin = Join-Path $buildDir 'qemu_efuse.bin'
$capture = Join-Path $buildDir 'qemu-serial.log'

if (-not (Test-Path (Join-Path $buildDir 'project_description.json'))) {
    throw "$Project has not been built yet (no build\project_description.json). Run build.ps1 first."
}
if (-not (Test-Path $qemuExe)) { throw "qemu-system-xtensa.exe not found at $qemuExe" }

# ------------------------------------------------------------- flash image --
# idf.py qemu would generate this itself, but only when it runs. Doing it here
# means -Direct, -MergedFlash and plain runs all share one code path, and a
# failure to merge is reported before QEMU starts.
function New-FlashImage {
    # Rebuild when the image is missing or older than any of its inputs, so a
    # fresh `build.ps1` run is not silently paired with yesterday's flash.
    if (Test-Path $flashBin) {
        $newest = Get-ChildItem $buildDir -Filter *.bin -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -ne $flashBin } |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $newest -or $newest.LastWriteTime -lt (Get-Item $flashBin).LastWriteTime) {
            Write-Host "flash image : $flashBin (reused)" -ForegroundColor DarkGray
            return
        }
        Write-Host "flash image : $flashBin (stale, regenerating)" -ForegroundColor DarkGray
    }
    else {
        Write-Host "flash image : $flashBin (merging)" -ForegroundColor DarkGray
    }
    Remove-Item $flashBin -Force -ErrorAction SilentlyContinue
    Push-Location $buildDir
    try {
        # esptool 5.x renamed merge_bin -> merge-bin; the old spelling still
        # works but prints a deprecation warning. @flash_args carries the
        # offsets and file names, --pad-to-size makes the image match the
        # 8 MB the header advertises (QEMU rejects a short image otherwise).
        & $python -m esptool "--chip=$Target" merge-bin "--output=$flashBin" `
            '--pad-to-size=8MB' '@flash_args'
        if ($LASTEXITCODE -ne 0) { throw "esptool merge-bin failed with $LASTEXITCODE" }
    }
    finally {
        Pop-Location
    }
}

function New-EfuseImage {
    if (Test-Path $efuseBin) { return }
    # 2048 bytes of zeros: chip revision 0.3 default eFuse block. QEMU needs
    # the drive to exist, the values themselves do not matter for an S3 with
    # no secure boot.
    Write-Host "efuse image : $efuseBin (creating 2 KiB zero image)" -ForegroundColor DarkGray
    [byte[]]$zeros = New-Object byte[] 2048
    [System.IO.File]::WriteAllBytes($efuseBin, $zeros)
}

# --------------------------------------------------------------- qemu argv --
function Get-QemuArgs {
    # Mirrors QEMU_TARGETS['esp32s3'] in
    # esp-idf/tools/idf_py_actions/qemu_ext.py.
    $a = @(
        '-M', 'esp32s3', '-m', '32M',
        '-drive', "file=$flashBin,if=mtd,format=raw",
        '-drive', "file=$efuseBin,if=none,format=raw,id=efuse",
        '-global', "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
        '-global', 'driver=timer.esp32s3.timg,property=wdt_disable,value=true',
        '-nic', 'user,model=open_eth',
        '-nographic',
        '-serial', 'mon:stdio'
    )
    if ($QemuExtraArgs) { $a += $QemuExtraArgs.Split(' ') }
    return $a
}

# -------------------------------------------------------------------- main --
New-FlashImage
if ($Efuse -ne 'none') { New-EfuseImage }

if ($MergedFlash) {
    Write-Host "merged flash image ready: $flashBin"
    exit 0
}

if ($TimeoutSec -gt 0) {
    # Timed run: capture and kill. -nographic with mon:stdio keeps QEMU in the
    # foreground and out of a window, so redirecting stdin/stdout is enough.
    Write-Host "qemu        : timed run, ${TimeoutSec}s, capture -> $capture" -ForegroundColor DarkGray
    $argv = Get-QemuArgs
    $quote = { param($s) if ($s -match '[\s,]' -and $s -notmatch '^[A-Za-z]:\\') { '"' + $s + '"' } else { $s } }
    $argLine = ($argv | ForEach-Object { & $quote $_ }) -join ' '
    Write-Host "cmd         : $qemuExe $argLine"
    Remove-Item $capture, "$capture.err" -Force -ErrorAction SilentlyContinue
    Remove-Item "$buildDir\qemu-stdin.txt" -Force -ErrorAction SilentlyContinue

    # Input is fed from a file rather than a pipeline: Start-Process cannot
    # write to a child's stdin, and a pipeline would make the whole run
    # synchronous. Each -InputLine becomes one line plus CR - CR, not LF,
    # because that is what an Enter key sends to a UART and what msh expects.
    $stdinFile = Join-Path $buildDir 'qemu-stdin.txt'
    if ($InputLine.Count -gt 0) {
        ($InputLine -join "`r") + "`r" | Set-Content -Path $stdinFile -NoNewline -Encoding ASCII
        Write-Host "stdin       : $stdinFile ($($InputLine.Count) line(s) after ${InputDelaySec}s)" -ForegroundColor DarkGray
    }

    if ($InputLine.Count -gt 0) {
        # Need a pipe we can write to after the child starts, so use .NET's
        # Process with redirected stdin instead of Start-Process.
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $qemuExe
        $psi.Arguments = $argLine
        $psi.UseShellExecute = $false
        $psi.RedirectStandardInput = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.CreateNoWindow = $true
        $proc = New-Object System.Diagnostics.Process
        $proc.StartInfo = $psi
        # Console.InputEncoding decides the encoding of the StreamWriter that
        # Process creates for StandardInput, and PowerShell's default there
        # carries a UTF-8 BOM. That BOM is written into the UART before the first
        # command, so the shell reads it as part of the command name - measured
        # in the capture as
        #
        #   EF BB BF 73 33 72 74   ->  "s3rt: command not found."
        #
        # with only the FIRST line affected, which made the gate's `s3rt v0.1`
        # expectation fail while every later command worked. ASCIIEncoding has an
        # empty preamble, which is what a serial line wants.
        $prevInEncoding = [Console]::InputEncoding
        try { [Console]::InputEncoding = New-Object System.Text.ASCIIEncoding } catch { }
        [void]$proc.Start()
        try { [Console]::InputEncoding = $prevInEncoding } catch { }
        $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
        $stderrTask = $proc.StandardError.ReadToEndAsync()
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        Start-Sleep -Seconds $InputDelaySec
        foreach ($line in $InputLine) {
            # CR LF, not a bare CR.
            #
            # QEMU's stdio chardev goes through the C runtime's text mode on
            # Windows, which drops a CR that is not followed by LF: measured,
            # four bare-CR lines arrived as "s3rts3rt ticklist_threadfree" -
            # every character echoed, no line ever ended, nothing executed.
            # shell.c accepts '\r' or '\n' as end of line, so sending both is
            # safe either way: if the CR survives it ends the line and the LF
            # produces an empty one.
            try {
                $bytes = [System.Text.Encoding]::ASCII.GetBytes("$line`r`n")
                $proc.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
                $proc.StandardInput.BaseStream.Flush()
            }
            catch { }
            Start-Sleep -Milliseconds 900
        }
        $remaining = [int][Math]::Max(0, ($deadline - (Get-Date)).TotalSeconds)
        if (-not $proc.WaitForExit($remaining * 1000)) {
            try { $proc.Kill() } catch { }
            $proc.WaitForExit(5000) | Out-Null
            Write-Host "qemu        : still running after ${TimeoutSec}s -> killed" -ForegroundColor Yellow
        }
        $combined = @()
        if ($stdoutTask.Wait(3000)) { $combined += ($stdoutTask.Result -split "`r?`n") }
        if ($stderrTask.Wait(3000)) { $combined += ($stderrTask.Result -split "`r?`n") }
    }
    else {
    $proc = Start-Process -FilePath $qemuExe -ArgumentList $argLine -NoNewWindow -PassThru `
        -RedirectStandardOutput $capture -RedirectStandardError "$capture.err"
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        try { $proc.Kill() } catch { }
        $proc.WaitForExit(5000) | Out-Null
        Write-Host "qemu        : still running after ${TimeoutSec}s -> killed" -ForegroundColor Yellow
    }
    else {
        Write-Host "qemu        : exited on its own with $($proc.ExitCode)" -ForegroundColor Yellow
    }
    # QEMU holds the redirected files open through the child handles; wait for
    # the handles to close, then read both halves and write them back as one
    # capture file.
    Start-Sleep -Milliseconds 500
    $combined = @()
    foreach ($f in @($capture, "$capture.err")) {
        if (Test-Path $f) { $combined += Get-Content $f -ErrorAction SilentlyContinue }
    }
    }
    $combined | Set-Content -Path $capture -Encoding UTF8
    Remove-Item "$capture.err" -ErrorAction SilentlyContinue
    Write-Host ""
    Write-Host "===== serial capture =====" -ForegroundColor Cyan
    # Write-Host per line, not a pipeline: if this script's stdout is itself
    # redirected, mixed Write-Host and pipeline output interleaves out of order.
    if ($combined.Count -gt 0) {
        foreach ($line in $combined) { Write-Host $line }
    }
    else {
        Write-Host '(no output)'
    }
    Write-Host "===== end capture ========" -ForegroundColor Cyan

    # Every -Expect regex has to match somewhere in the capture. Matching the
    # whole capture rather than line by line because the banner spans lines and
    # because QEMU's CR/LF mix makes line boundaries unreliable.
    #
    # U+FEFF is stripped first. It appears in the capture occasionally - twice
    # in one measured run, immediately before a typed command and before the
    # shell's answer to it - and a leading BOM turns "s3rt" into a command name
    # the shell does not know:
    #
    #   EF BB BF 73 33 72 74   ->  "s3rt"   ->  "s3rt: command not found."
    #
    # That is a transport artefact of the pty/host encoding chain, not a guest
    # behaviour, so it must not be allowed to fail the gate. Stripping it is
    # safe: the expectations are ASCII substrings.
    $flat = ($combined -join "`n").Replace([string][char]0xFEFF, '')
    $missing = @()
    foreach ($re in $Expect) {
        if ($flat -notmatch $re) { $missing += $re }
    }
    if ($missing.Count -eq 0) {
        Write-Host "RESULT: PASS - all $($Expect.Count) expectation(s) matched" -ForegroundColor Green
        exit 0
    }
    Write-Host "RESULT: FAIL - $($missing.Count) of $($Expect.Count) expectation(s) missing:" -ForegroundColor Red
    foreach ($re in $missing) { Write-Host "  /$re/" -ForegroundColor Red }
    exit 1
}

if (-not $Direct -and -not [Console]::IsInputRedirected) {
    # Preferred route when a human is watching: let IDF build the image, the
    # efuse drive and the argv (it reads them from QEMU_TARGETS['esp32s3'] in
    # tools/idf_py_actions/qemu_ext.py, so it stays correct if the machine's
    # requirements change). -qemu-extra-args is available for experiments.
    #
    # Only usable interactively: idf.py runs QEMU with '-serial mon:stdio' and
    # gives it the console, and QEMU quits the moment that stdin hits EOF. With
    # this script's own stdin redirected (a harness, a scheduled task, a CI
    # job) that is immediately, and it exits 0 with no serial output at all.
    # Redirected runs therefore take the -Direct path below instead.
    $idfArgs = @('-DIDF_TARGET=' + $Target, 'qemu')
    if ($QemuExtraArgs) { $idfArgs += @('--qemu-extra-args', $QemuExtraArgs) }
    Write-Host "running     : python $idfPy $($idfArgs -join ' ')" -ForegroundColor DarkGray
    Push-Location $Project
    try {
        # Same trap as in build.ps1: idf.py writes its coloured status lines to
        # stderr, and PowerShell 5.1 turns the first native stderr line into a
        # terminating NativeCommandError while the preference is Stop.
        $ErrorActionPreference = 'Continue'
        & $python -u $idfPy @idfArgs
        $code = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    Write-Host "idf.py qemu finished with exit code $code" -ForegroundColor DarkGray
    exit $code
}

if (-not $Direct) {
    Write-Host "note        : stdin is redirected, so 'idf.py qemu' cannot be used" -ForegroundColor DarkGray
    Write-Host "              (QEMU exits on stdin EOF). Running qemu-system-xtensa directly." -ForegroundColor DarkGray
}

$argv = Get-QemuArgs
Write-Host "running     : $qemuExe $($argv -join ' ')" -ForegroundColor DarkGray
& $qemuExe @argv
exit $LASTEXITCODE
