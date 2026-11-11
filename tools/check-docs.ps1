# Run the documented shell commands against a board and compare what comes back
# with tools/expected-output.txt.
#
# Why: the docs contain example transcripts, and one of them had drifted - the
# README showed "kernel    : ..." where the firmware prints "heap kernel: ...",
# and put "verdict : PASS" inside s3rt's output when that line comes from the
# selftest thread six seconds after boot. Neither is a code defect, but a reader
# who spots one stale example has no reason to trust the others. Checking by
# hand found three such mismatches in one round and would have missed the next.
#
# This is not a substitute for the QEMU gate (tools/qemu.ps1, which asserts a
# handful of boot milestones) or for reading the output. It only answers one
# question: does the firmware still print the shape the documentation claims?
#
# Usage:
#   .\check-docs.ps1                        # all sections, COM19 @ 115200
#   .\check-docs.ps1 -Port COM7
#   .\check-docs.ps1 -Only s3rt,s3smp       # just these sections
#   .\check-docs.ps1 -WifiSsid <ssid> -WifiPass <pass>
#                                           # include the Wi-Fi and TCP sections
#   .\check-docs.ps1 -List                  # print the sections and exit
#
# Exit code is 0 when every pattern matched, 1 otherwise.

param(
    [string]$Port = 'COM19',
    [int]$Baud = 115200,
    [string[]]$Only = @(),
    [string]$WifiSsid = '',
    [string]$WifiPass = '',
    [switch]$List,
    # Seconds to wait for the shell prompt after reset. PSRAM calibration plus
    # Wi-Fi/BLE bring-up takes 15-18 s on this board; the default leaves room.
    [int]$BootSeconds = 36,
    # Only reproduce: capture the output and print it, do not compare.
    [switch]$Dump
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$expectFile = Join-Path $PSScriptRoot 'expected-output.txt'
if (-not (Test-Path $expectFile)) { throw "missing $expectFile" }

# ------------------------------------------------------------------ parse ---
#
# The file is sections of "[command]" followed by regex lines. Blank lines and
# lines starting with '#' are ignored.

$sections = [ordered]@{}
$reboots  = @{}
$current  = $null
foreach ($line in Get-Content $expectFile) {
    $t = $line.Trim()
    if (-not $t) { continue }
    if ($t.StartsWith('#')) { continue }
    if ($t -match '^\[(!?)(.+)\]$') {
        # The leading "!" marks a section whose command reboots the board. It is
        # stripped from the name (which is also the command) and remembered, so
        # the driver can keep capturing across the restart and the run order can
        # put these last.
        $reboot = ($Matches[1] -eq '!')
        $current = $Matches[2]
        $sections[$current] = New-Object System.Collections.ArrayList
        $reboots[$current] = $reboot
        continue
    }
    if (-not $current) { throw "pattern before any [section]: $t" }
    [void]$sections[$current].Add($t)
}

# A section that needs credentials is skipped unless they were supplied; its
# patterns cannot match without them.
# Sections whose command needs credentials. Matched exactly, so the section
# with placeholders in its name has to be listed in that form.
$needsWifi = @('s3wifiscan', 's3net example.com', 's3wifi <ssid> <pass>')
$needsCreds = @{ 's3net example.com' = $true }

if ($List) {
    Write-Host ""
    Write-Host "sections in $expectFile`:"
    foreach ($k in $sections.Keys) {
        $mark = if ($needsWifi -contains $k) { '  (needs -WifiSsid)' } else { '' }
        Write-Host ("  {0,-20} {1,2} pattern(s){2}" -f $k, $sections[$k].Count, $mark)
    }
    Write-Host ""
    exit 0
}

# `powershell -File script.ps1 -Only a,b` hands PowerShell one string "a,b",
# not an array - -File has no notion of arrays. Split it here so the documented
# invocation works with either form.
$onlyList = @()
foreach ($o in $Only) {
    foreach ($part in ($o -split ',')) {
        $p = $part.Trim()
        if ($p) { $onlyList += $p }
    }
}

$selected = @()
foreach ($k in $sections.Keys) {
    if ($onlyList.Count -gt 0 -and $onlyList -notcontains $k) { continue }
    if ($needsWifi -contains $k -and -not $WifiSsid) { continue }
    $selected += $k
}

if ($selected.Count -eq 0) {
    throw "nothing selected. Use -List to see the sections, or pass -WifiSsid for the network ones."
}

# ------------------------------------------------------------------ serial ---
#
# pyserial through the ESP-IDF python, because that is the interpreter this
# repository already depends on and it has pyserial installed.

. (Join-Path $PSScriptRoot 'env.ps1')

$py = $script:S3Python
if (-not (Test-Path $py)) { throw "python not found at $py" }

& $py -c "import serial" 2>$null
if ($LASTEXITCODE -ne 0) {
    throw "pyserial is not installed in $py. Run: `"$py`" -m pip install pyserial"
}

# The driver does the whole run: reset, command sequence, capture. Doing it in
# Python rather than PowerShell keeps the timing logic in one place and avoids
# passing a list of commands through quoting.
$driver = Join-Path $env:TEMP 's3-check-docs.py'

# Reboot sections last: everything after one would run against a freshly
# booted device, so anything that depends on state built earlier must come
# first. Order within each group is the order in the file.
$ordered = @()
foreach ($k in $selected) { if (-not $reboots[$k]) { $ordered += $k } }
foreach ($k in $selected) { if ($reboots[$k])     { $ordered += $k } }

$spec = [ordered]@{}
foreach ($k in $ordered) { $spec[$k] = @{ patterns = @($sections[$k]); reboot = [bool]$reboots[$k] } }

$payload = @{
    port        = $Port
    baud        = $Baud
    boot_s      = $BootSeconds
    dump        = [bool]$Dump
    sections    = $spec
    wifi_ssid   = $WifiSsid
    wifi_pass   = $WifiPass
}

$json = $payload | ConvertTo-Json -Depth 6 -Compress
$json | Set-Content (Join-Path $env:TEMP 's3-check-docs.json') -Encoding utf8

$pySource = @'
import json, io, os, re, sys, time
import serial

CFG = json.loads(io.open(os.environ["TEMP"] + r"\s3-check-docs.json", encoding="utf-8-sig").read())

s = serial.Serial()
s.port = CFG["port"]
s.baudrate = CFG["baud"]
s.timeout = 0.5
s.dtr = False
s.rts = False
s.open()
s.reset_input_buffer()

# Reset via RTS. dtr would pull some CH340 boards into the bootloader.
s.rts = True
time.sleep(0.3)
s.rts = False

boot = CFG["boot_s"]
raw = bytearray()
t0 = time.time()
while time.time() - t0 < boot:
    raw.extend(s.read(8192))

# The prompt is not necessarily visible in the boot capture: the shell prints
# "msh >" only after it has read a line, and a fresh boot ends with the selftest
# verdict instead. Nudge it with one newline and look again - measured, the
# prompt appears immediately after.
text = bytes(raw).decode("utf-8", "replace").replace("\r", "\n")
if "msh >" not in text:
    s.write(b"\r\n")
    s.flush()
    t1 = time.time()
    while time.time() - t1 < 8:
        raw.extend(s.read(4096))
    text = bytes(raw).decode("utf-8", "replace").replace("\r", "\n")

if "msh >" not in text:
    print("FAIL: no shell prompt after %d s, even after sending a newline." % boot)
    print("      Is the board running this image? Captured %d bytes." % len(text))
    for line in text.split("\n")[-20:]:
        if line.strip():
            print("      " + line.strip()[:100])
    s.close()
    sys.exit(2)

# How long to wait for each command's output. A command that takes longer than
# this is measured wrong, not broken; the patterns still have to match.
WAIT = {
    "s3fs": 70, "s3fsbench write": 90, "s3fsbench small": 120,
    "s3wifiscan": 40, "s3ble": 50,
    "s3wifi <ssid> <pass>": 80, "s3net example.com": 35,
    "s3ulp": 30, "list thread": 10, "free": 8,
}
DEFAULT_WAIT = 20

# A reboot section deliberately resets the board. It needs longer than any
# other section (the watchdog has to expire first), and the capture spans the
# restart naturally - the same serial port keeps receiving the boot banner.
REBOOT_WAIT = 30

results = []
for name, spec in CFG["sections"].items():
    patterns = spec["patterns"]
    reboots = spec.get("reboot", False)
    # A section name may carry placeholders for the credentials; the section
    # name IS the command, so substituting here is what makes
    # "s3wifi <ssid> <pass>" runnable without a special case per command.
    cmd = name.replace("<ssid>", CFG["wifi_ssid"]).replace("<pass>", CFG["wifi_pass"])
    wait = REBOOT_WAIT if reboots else WAIT.get(name, WAIT.get(cmd, DEFAULT_WAIT))

    mark = len(raw)
    s.write((cmd + "\r\n").encode())
    s.flush()
    t0 = time.time()
    while time.time() - t0 < wait:
        raw.extend(s.read(4096))

    out = bytes(raw[mark:]).decode("utf-8", "replace").replace("\r", "\n")

    # After a reboot the shell has to be prompted again before the next section
    # can run, exactly as at boot.
    if reboots:
        t1 = time.time()
        while time.time() - t1 < 8:
            raw.extend(s.read(4096))
        s.write(b"\r\n")
        s.flush()
        t1 = time.time()
        while time.time() - t1 < 5:
            raw.extend(s.read(4096))

    if CFG["dump"]:
        print("=== %s ===\n%s" % (name, out))

    missing = []
    for p in patterns:
        try:
            if not re.search(p, out):
                missing.append(p)
        except re.error as e:
            missing.append("%s   (BAD REGEX: %s)" % (p, e))

    results.append((name, len(patterns), missing, out))

s.close()

failed = 0
print("")
for name, total, missing, out in results:
    if not missing:
        print("  PASS  %-20s %d pattern(s)" % (name, total))
    else:
        failed += 1
        print("  FAIL  %-20s %d of %d pattern(s) missing" % (name, len(missing), total))
        for m in missing:
            print("          missing: %s" % m)
        # Show the captured text once, so the mismatch can be read rather than
        # guessed at.
        print("          captured:")
        for line in out.split("\n"):
            if line.strip() and not line.strip().startswith("I ("):
                print("            " + line.strip()[:110])

print("")
if failed:
    print("RESULT: FAIL - %d of %d section(s) did not match" % (failed, len(results)))
    sys.exit(1)

print("RESULT: PASS - all %d section(s) matched" % len(results))
sys.exit(0)
'@

$pySource | Set-Content $driver -Encoding utf8

& $py -u -X utf8 $driver
exit $LASTEXITCODE
