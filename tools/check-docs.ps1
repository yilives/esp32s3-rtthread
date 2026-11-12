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

    # A line starting with "~" constrains a capture group of the pattern above
    # it, rather than matching anything itself:
    #
    #   s3fsbench: create 32 small files -> (\d+) writes .*, (\d+) erases
    #   ~ 1 == 152            exact
    #   ~ 2 in 130..140       range, inclusive
    #
    # Several constraints may follow one pattern, so each entry carries a LIST.
    # (The first version stored a single constraint by replacing the entry with
    # a hashtable, and a second "~" line then pointed "pattern" at that
    # hashtable - which surfaced as "unhashable type: dict" in the driver.)
    #
    # Without constraints, a pattern like "(\d+) KB/s" passes on 57 and on 242
    # alike, so a ten-fold performance regression is invisible.
    if ($t.StartsWith('~')) {
        $last = $sections[$current].Count - 1
        if ($last -lt 0) { throw "'~' constraint before any pattern in [$current]" }
        $sections[$current][$last].constraints += $t.Substring(1).Trim()
        continue
    }

    [void]$sections[$current].Add(@{ pattern = $t; constraints = @() })
}

# Two different requirements, and conflating them meant hiding a section from the
# reader most likely to want it.
#
#   needsRadio  - the section needs a 2.4 GHz access point somewhere in range.
#                 `s3wifiscan` is the only one, and it takes no arguments: it will
#                 scan and report whatever it finds.
#   needsCreds  - the section actually types an SSID and a password, or needs to
#                 reach the internet through an association.
#
# The skip below used to test needsRadio, so a reader with no WiFi credentials -
# exactly the person checking that scanning works - had `s3wifiscan` silently
# dropped from the run with only a "(needs -WifiSsid)" note to explain it.
# Measured: `s3wifiscan` is `RT_UNUSED(argc)` and succeeds on any board with an AP
# in range.
$needsRadio = @('s3wifiscan')
$needsCreds = @('s3net example.com',
                's3wifi <ssid> <pass>', 's3wifi <ssid> <pass> #second call')

# `# scope: U|B|E - ...` sits directly under its [section] header. It is read in a
# second pass because the main loop assigns sections as it goes: reading the
# annotation inline would attribute it to the *next* section, which is exactly the
# off-by-one that made the first listing show [?] for the first section and every
# other one shifted by one.
$scopes = @{}
$pendingSection = $null
foreach ($line in Get-Content $expectFile) {
    $t = $line.Trim()
    if ($t -match '^\[!?(.+)\]$') { $pendingSection = $Matches[1]; continue }
    if ($pendingSection -and $t -match '^#\s*scope:\s*([UBE])\b') {
        $scopes[$pendingSection] = $Matches[1]
        $pendingSection = $null
    }
}

# ------------------------------------------------------ metadata integrity ---
#
# The file now carries two kinds of declarative metadata: `# scope:` on every
# section and `# order:` where position matters. Both are read by code, so both
# can rot: a section added without a scope used to show up as `[?]` in -List and
# nothing failed, and a scope value of anything other than U/B/E was simply
# carried through as a string.
#
# Everything below runs before the port is opened, for the same reason the order
# check does: a malformed file is a two-second fix and a wasted run is forty
# minutes.
$metaErrs = @()

$KnownMeta = @('scope', 'order')
$knownSections = @($sections.Keys)

# Metadata lives in the block directly under a [section] header, before its first
# pattern. That is a structural rule, not a stylistic one: it is the only thing
# that distinguishes `# scope: U - ...` from prose that happens to start the same
# way. `# apart: 8 data bits plus the ACK slot is 9 falling edges` is a sentence
# from the i2cwatch discussion and parsed as a key called "apart".
$metaOwner = $null
$inHeader  = $false
$scopeSeen = @{}
foreach ($line in Get-Content $expectFile) {
    $t = $line.Trim()
    if ($t -match '^\[!?(.+)\]$') {
        $metaOwner = $Matches[1]
        $inHeader  = $true
        continue
    }
    if (-not $metaOwner) { continue }
    if (-not $t) { continue }

    # A pattern or constraint ends the header block for good.
    if (-not $t.StartsWith('#')) { $inHeader = $false }
    if (-not $inHeader) { continue }
    if ($t -notmatch '^#\s*([A-Za-z][A-Za-z0-9_-]*)\s*:') { continue }

    $kind = $Matches[1].ToLower()
    if ($KnownMeta -notcontains $kind) {
        $metaErrs += "[$metaOwner] has '# ${kind}:' - not a known key (known: $($KnownMeta -join ', '))"
        continue
    }

    if ($kind -eq 'scope') {
        $scopeSeen[$metaOwner] = $true
        if ($t -notmatch '^#\s*scope:\s*([UBE])\b') {
            $metaErrs += "[$metaOwner] scope must start with U, B or E - got: $t"
        }
    }
    if ($kind -eq 'order') {
        if ($t -notmatch '^#\s*order:\s*(after|before)\s+(.+)$') {
            $metaErrs += "[$metaOwner] order must read 'after <section>' or 'before <section>' - got: $t"
        } else {
            $target = $Matches[2].Trim()
            if ($knownSections -notcontains $target) {
                $metaErrs += "[$metaOwner] order names '$target', which is not a section"
            }
        }
    }
}

foreach ($k in $knownSections) {
    if (-not $scopeSeen[$k]) {
        $metaErrs += "[$k] has no '# scope:' line - say whether a failure there is a defect (U), board-specific (B) or environmental (E)"
    }
}

if ($metaErrs.Count -gt 0) {
    Write-Host ""
    Write-Host "  METADATA PROBLEMS in $expectFile" -ForegroundColor Yellow
    foreach ($e in $metaErrs) { Write-Host "    $e" -ForegroundColor Yellow }
    Write-Host ""
    Write-Host "  Both keys are read by this script, so a malformed one is a bug in the" -ForegroundColor DarkGray
    Write-Host "  file, not a property of the board." -ForegroundColor DarkGray
    Write-Host ""
    exit 1
}

# `# order: after X` / `# order: before X` lines are checked here, at parse time,
# before the board is opened. A violated order used to be invisible until the run
# reached that section - 40 minutes in - and was found that way twice. The cost of
# reading the file twice is nil next to the cost of a wasted run.
$orderErrs = @()
$ordered = [ordered]@{}
foreach ($k in $sections.Keys) { $ordered[$k] = @{ after = @(); before = @() } }

$curSec = $null
foreach ($line in Get-Content $expectFile) {
    $t = $line.Trim()
    if ($t -match '^\[!?(.+)\]$') { $curSec = $Matches[1]; continue }
    if (-not $curSec) { continue }
    if ($t -match '^#\s*order:\s*(after|before)\s+(.+)$') {
        $rel = $Matches[1]
        $target = $Matches[2].Trim()
        $ordered[$curSec][$rel] += $target
    }
}

$position = @{}
$idx = 0
foreach ($k in $sections.Keys) { $position[$k] = $idx; $idx++ }

foreach ($k in $ordered.Keys) {
    foreach ($target in $ordered[$k].after) {
        if (-not $position.ContainsKey($target)) {
            $orderErrs += "[$k] says 'after $target', but there is no such section"
        } elseif ($position[$k] -lt $position[$target]) {
            $orderErrs += "[$k] (position $($position[$k])) says 'after $target' (position $($position[$target]))"
        }
    }
    foreach ($target in $ordered[$k].before) {
        if (-not $position.ContainsKey($target)) {
            $orderErrs += "[$k] says 'before $target', but there is no such section"
        } elseif ($position[$k] -gt $position[$target]) {
            $orderErrs += "[$k] (position $($position[$k])) says 'before $target' (position $($position[$target]))"
        }
    }
}

if ($orderErrs.Count -gt 0) {
    Write-Host ""
    Write-Host "  ORDER VIOLATIONS in $expectFile" -ForegroundColor Yellow
    foreach ($e in $orderErrs) { Write-Host "    $e" -ForegroundColor Yellow }
    Write-Host ""
    Write-Host "  These are declared in the file, so the fix is to move a section or to" -ForegroundColor DarkGray
    Write-Host "  correct a declaration - not to run the suite and see what breaks." -ForegroundColor DarkGray
    Write-Host ""
    exit 1
}

if ($List) {
    Write-Host ""
    Write-Host "sections in $expectFile`:"
    foreach ($k in $sections.Keys) {
        $mark = if (-not $WifiSsid -and ($needsCreds -contains $k)) { '  (needs -WifiSsid)' }
                elseif ($needsRadio -contains $k) { '  (needs an AP in range)' }
                else { '' }
        $sc = if ($scopes[$k]) { $scopes[$k] } else { '?' }
        Write-Host ("  [{0}] {1,-20} {2,2} pattern(s){3}" -f $sc, $k, $sections[$k].Count, $mark)
    }
    Write-Host ""
    $nu = ($scopes.Values | Where-Object { $_ -eq 'U' }).Count
    $nb = ($scopes.Values | Where-Object { $_ -eq 'B' }).Count
    $ne = ($scopes.Values | Where-Object { $_ -eq 'E' }).Count
    Write-Host "  scope: [U] $nu any ESP32-S3, [B] $nb board-specific, [E] $ne environment"
    Write-Host "  A [B] or [E] failure is not automatically a defect; a [U] failure is."
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
    # Only credentials can be supplied from here. An AP in range cannot, so a
    # radio-only section always runs and reports what it sees.
    if ($needsCreds -contains $k -and -not $WifiSsid) { continue }
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
foreach ($k in $ordered) {
    $norm = @()
    foreach ($item in $sections[$k]) {
        $norm += @{ pattern = $item.pattern; constraints = @($item.constraints) }
    }
    $spec[$k] = @{ patterns = $norm; reboot = [bool]$reboots[$k] }
}

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
import difflib
import json, io, os, re, sys, time
import serial

CFG = json.loads(io.open(os.environ["TEMP"] + r"\s3-check-docs.json", encoding="utf-8-sig").read())

# Opening the port can fail with PermissionError(13) for a second or two after
# a previous process exits on Windows - the handle is closed but not released
# yet. Measured: a full check run failed this way twice, with no other process
# holding COM19 and a standalone open succeeding immediately afterwards. Retry
# rather than making the user rerun the whole thing.
s = serial.Serial()
s.port = CFG["port"]
s.baudrate = CFG["baud"]
s.timeout = 0.5
s.dtr = False
s.rts = False

_opened = False
for _attempt in range(10):
    try:
        s.open()
        _opened = True
        break
    except serial.SerialException as e:
        if _attempt == 9:
            print("FAIL: cannot open %s after 10 attempts: %s" % (CFG["port"], e))
            print("")
            print("      Two causes, and they need different fixes:")
            print("")
            print("      Another program holds it - a terminal, another check-docs")
            print("      run, or esptool. Close it.")
            print("")
            print("      Or the CH340 driver has wedged. The port still appears in")
            print("      the port list but opens with OSError(22, 'incorrect")
            print("      function') and stays that way indefinitely. Measured after")
            print("      a long sequence of runs; retrying does not help, and this")
            print("      restores it in place:")
            print("")
            print("        $d = Get-PnpDevice -Class Ports | Where-Object {")
            print("            $_.FriendlyName -match 'CH340' }")
            print("        Disable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false")
            print("        Enable-PnpDevice  -InstanceId $d.InstanceId -Confirm:$false")
            print("")
            sys.exit(2)
        time.sleep(0.5)

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
    # "[cmd #note]" runs `cmd`; the note only makes the section name unique.
    # Without it a section could never run the same command twice, and "call it
    # twice in a row" is exactly how the missing netif guards were found.
    cmd = name.split(" #")[0].strip()
    cmd = cmd.replace("<ssid>", CFG["wifi_ssid"]).replace("<pass>", CFG["wifi_pass"])
    wait = REBOOT_WAIT if reboots else WAIT.get(name, WAIT.get(cmd, DEFAULT_WAIT))

    mark = len(raw)
    s.write((cmd + "\r\n").encode())
    s.flush()
    t0 = time.time()
    read_error = None
    while time.time() - t0 < wait:
        try:
            raw.extend(s.read(4096))
        except serial.SerialException as e:
            # A read can fail mid-run on Windows while the port itself stays
            # open - measured as "GetOverlappedResult failed (PermissionError
            # (13))" partway through a long sequence, with the port immediately
            # reopenable afterwards. Losing all results because one read failed
            # is worse than reporting which section hit it.
            read_error = e
            break

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
    for pi, spec in enumerate(patterns, 1):
        p = spec["pattern"]
        cons = spec.get("constraints") or []
        try:
            m = re.search(p, out)
        except re.error as e:
            missing.append("[%d/%d] %s   (BAD REGEX: %s)" % (pi, len(patterns), p, e))
            continue
        if not m:
            # When a pattern does not match at all, the useful question is "what
            # is on the board instead". Pick the captured line closest to the
            # pattern's literal skeleton and show it. Measured on a deliberate
            # rename of "entry(ies)" to "entries": the right line wins at 0.89
            # against 0.40 for the next candidate.
            note = ""
            try:
                skel = re.sub(r"\\[a-z]|[\\^$.*+?()\[\]{}|]", " ", p)
                skel = re.sub(r"\s+", " ", skel).strip()
                best, score = "", 0.0
                if skel:
                    for cand in out.split("\n"):
                        cand = cand.strip()
                        if not cand:
                            continue
                        r = difflib.SequenceMatcher(None, skel, cand).ratio()
                        if r > score:
                            best, score = cand, r
                if score >= 0.6:
                    note = "   (closest line on the board: %s)" % best[:120]
            except Exception:
                pass
            missing.append("[%d/%d] %s%s" % (pi, len(patterns), p, note))
            continue

        # "N in LO..HI" or "N == VALUE". N is a capture group number; 0 is the
        # whole match.
        for con in cons:
            cm = re.match(r"^(\d+)\s+(in\s+(-?\d+)\.\.(-?\d+)|==\s*(-?\d+))$", con)
            # Show what the pattern actually matched alongside any complaint.
            # "group 5 is 196608, outside 60000..70000" says the number is wrong;
            # the matched line says WHICH LINE produced it, which is the part a
            # reader needs when several lines in one section look alike.
            hit = m.group(0).replace("\n", " | ")[:120]
            tag = "[%d/%d] %s" % (pi, len(patterns), p)
            if not cm:
                missing.append("%s   (BAD CONSTRAINT: %s)" % (tag, con))
                continue
            try:
                gi = int(cm.group(1))
                got = int(m.group(gi))
            except (IndexError, ValueError):
                missing.append("%s   (group %s is not an integer; matched: %s)"
                               % (tag, cm.group(1), hit))
                continue

            if cm.group(5) is not None:
                want = int(cm.group(5))
                if got != want:
                    missing.append("%s   (group %d is %d, expected %d; matched: %s)"
                                   % (tag, gi, got, want, hit))
            else:
                lo, hi = int(cm.group(3)), int(cm.group(4))
                if not (lo <= got <= hi):
                    missing.append("%s   (group %d is %d, outside %d..%d; matched: %s)"
                                   % (tag, gi, got, lo, hi, hit))

    if read_error is not None:
        missing = missing + ["SERIAL READ FAILED: %s" % read_error]

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
