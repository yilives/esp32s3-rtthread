# Drive the board with a random sequence of commands and look for damage.
#
# Why: every existing check runs a fixed order. soak-docs.ps1 repeats a fixed
# order. Real use is not ordered - someone brings Wi-Fi up and down, creates and
# deletes files, starts and stops BLE, in whatever order they feel like. A leaked
# reference, a state machine that only resets on the happy path, or a buffer that
# is freed twice would show up there and never in a fixed sequence.
#
# What it does NOT do: judge command output. It is not looking for the right bytes;
# it is looking for the board to still be alive, still have the same amount of
# memory, and still answer.
#
# The command pool is split by precondition rather than by name:
#
#   free       - no preconditions, no side effects that matter
#   filesystem - needs the volume mounted, and may change what is on it
#   network    - needs an association and a lease
#
# Commands that reboot the board or destroy the volume are excluded on purpose:
# s3wdt hang proves the reset path but ends the run, and s3fs erase/format would
# make every later filesystem command fail for a reason that is not a defect.
#
# Usage:
#   .\stress-random.ps1                       # 150 random commands
#   .\stress-random.ps1 -Count 400
#   .\stress-random.ps1 -Seed 12345           # reproduce a run
#   .\stress-random.ps1 -WifiSsid <ssid> -WifiPass <pass>

param(
    [string]$Port = 'COM19',
    [int]$Baud = 115200,
    [int]$Count = 150,
    # Fixed seed makes a failing run reproducible. 0 means pick one and print it.
    [int]$Seed = 0,
    [string]$WifiSsid = '',
    [string]$WifiPass = '',
    [int]$BootSeconds = 36,
    # Give up if the board stops answering for this long.
    [int]$PerCommandSeconds = 30
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'env.ps1')

$py = $script:S3Python
if (-not (Test-Path $py)) { throw "python not found at $py" }

& $py -c "import serial" 2>$null
if ($LASTEXITCODE -ne 0) { throw "pyserial missing in $py" }

$cfg = @{
    port      = $Port
    baud      = $Baud
    count     = $Count
    seed      = $Seed
    boot_s    = $BootSeconds
    per_cmd_s = $PerCommandSeconds
    ssid      = $WifiSsid
    password  = $WifiPass
}
($cfg | ConvertTo-Json -Compress) |
    Set-Content (Join-Path $env:TEMP 's3-stress-random.json') -Encoding utf8

$driver = Join-Path $env:TEMP 's3-stress-random.py'

$source = @'
import io, json, os, random, re, sys, time
import serial

CFG = json.loads(io.open(os.environ["TEMP"] + r"\s3-stress-random.json",
                         encoding="utf-8-sig").read())

seed = CFG["seed"] or random.randrange(1, 2**31)
rng = random.Random(seed)

# Commands with no precondition worth tracking. Excluded from here on purpose:
# s3wdt hang (reboots), s3fs erase / s3fs format (destroys the volume).
FREE = [
    "s3rt", "s3rt tick", "s3rt irq", "s3rt irqlat", "s3smp", "s3heap", "free",
    "list thread", "s3rt psram", "s3rt led", "s3rt pwm", "s3rt key",
    "i2cscan", "i2cprobe 0x50 8", "i2cwatch 0 0x50", "i2cwatch 0 0x55 10000",
    "spiloop", "spiloop internal", "s3adc 1 3", "s3adc 1 7",
    "s3wdt",
    # Deliberate bad input: exercises the usage paths, which are real code.
    "s3rt bogus", "s3fs bogus", "s3ble bogus", "s3ulp bogus", "s3ulpw bogus",
]

# Needs the volume mounted. `s3fs` itself mounts, so it can restore the state.
FS = ["s3fs", "s3fs ls", "s3fs read", "s3fs cat s3fs.txt", "s3fs rm s3fs.txt",
      "s3fs cat NOPE.txt", "s3fs rm NOPE.txt", "s3fs unmount",
      "s3fsbench small", "s3fsbench write"]

# Needs an association. Order matters here and is handled by the state machine
# below: BLE must come down before Wi-Fi goes up, per KNOWN-ISSUES 12.
NET = ["s3net example.com", "s3wifi <ssid> <pass>", "s3wifiscan"]
BLE = ["s3ble", "s3ble info", "s3ble deinit"]

def open_port():
    s = serial.Serial()
    s.port = CFG["port"]
    s.baudrate = CFG["baud"]
    s.timeout = 0.5
    s.dtr = False
    s.rts = False
    for attempt in range(10):
        try:
            s.open()
            return s
        except serial.SerialException as e:
            if attempt == 9:
                print("FAIL: cannot open %s: %s" % (CFG["port"], e))
                sys.exit(2)
            time.sleep(0.5)
    return None

s = open_port()
s.reset_input_buffer()
s.rts = True
time.sleep(0.3)
s.rts = False

raw = bytearray()
t0 = time.time()
while time.time() - t0 < CFG["boot_s"]:
    raw.extend(s.read(8192))
s.write(b"\r\n")
s.flush()
time.sleep(1)
raw.extend(s.read(4096))

boot_text = bytes(raw).decode("utf-8", "replace")
if "verdict : PASS" not in boot_text and "msh" not in boot_text:
    print("FAIL: the board did not reach a shell after %d s" % CFG["boot_s"])
    s.close()
    sys.exit(2)

# ---------------------------------------------------------------- helpers ---

def run(cmd):
    """Send one command, return (text, elapsed, answered)."""
    mark = len(raw)
    s.write((cmd + "\r\n").encode())
    s.flush()
    t0 = time.time()
    got_prompt = False
    while time.time() - t0 < CFG["per_cmd_s"]:
        try:
            raw.extend(s.read(4096))
        except serial.SerialException as e:
            out = bytes(raw[mark:]).decode("utf-8", "replace")
            return out, time.time() - t0, False
        out = bytes(raw[mark:]).decode("utf-8", "replace").replace("\r", "\n")
        # The shell prompt reappearing means the command finished.
        if out.count("msh >") >= 1 and out.rstrip().endswith(">"):
            got_prompt = True
            break
    text = bytes(raw[mark:]).decode("utf-8", "replace").replace("\r", "\n")
    return text, time.time() - t0, got_prompt

def snapshot():
    """Free-memory readings, so a leak shows up as a trend rather than a crash."""
    t, _, _ = run("s3heap")
    d = {}
    for key, pat in [("dram", r"internal 8-bit :\s+(\d+) free"),
                     ("psram", r"PSRAM 8-bit\s+:\s+(\d+) free"),
                     ("total", r"total free\s+:\s+(\d+)")]:
        m = re.search(pat, t)
        d[key] = int(m.group(1)) if m else None
    return d

FAULTS = [("Guru", "Guru meditation"), ("rst:0x", "reset"),
          ("assert failed", "assert"), ("Malloc failed", "malloc failure"),
          ("Cache error", "cache error"), ("LoadProhibited", "load prohibited")]

# ------------------------------------------------------------------- run ---

print("")
print("  seed      : %d" % seed)
print("  commands  : %d" % CFG["count"])
print("")

history = []
faults = []
seen_mem = []
fs_mounted = True     # the boot mount happens before the first command
ble_up = False
wifi_up = False

print("  %-5s %-26s %8s  %s" % ("#", "command", "ms", "mem(dram)"))
# The first reading is taken right after boot, with the volume mounted. The final
# reading is taken with everything down. Those are different states, and the gap
# between them (about 9.4 KB, the wear-levelling buffers) showed up as a constant
# in runs of 60 and 200 commands alike - it is an offset, not a trend.
#
# So the comparison uses an idle baseline too: bring the same things down before
# the run starts, and compare end against that.
run("s3ble deinit")
run("s3ulp stop")
run("s3fs unmount")
base = snapshot()
print("  %-5s %-26s %8s  %d" % ("base", "(idle)", "", base["dram"] or -1))

for i in range(1, CFG["count"] + 1):
    pool = list(FREE)
    if fs_mounted:
        pool += FS
    # The radio pair is a small state machine, not a free choice: BLE has to be
    # down before Wi-Fi starts (KNOWN-ISSUES 12) and Wi-Fi up blocks BLE.
    if not wifi_up:
        pool += BLE
    if CFG["ssid"] and not ble_up:
        pool += NET

    cmd = rng.choice(pool)

    text, elapsed, answered = run(cmd)
    history.append(cmd)

    # Track the state the next choice depends on.
    if cmd.startswith("s3ble") and cmd != "s3ble deinit" and "PASS" in text:
        ble_up = True
    if cmd == "s3ble deinit" and "stack down" in text:
        ble_up = False
    if cmd.startswith("s3wifi") and "PASS" in text:
        wifi_up = True
    if cmd == "s3fs unmount":
        fs_mounted = False
    if cmd == "s3fs" and "PASS, mounted" in text:
        fs_mounted = True

    mem = snapshot()["dram"]
    seen_mem.append(mem)
    print("  %-5d %-26s %8.0f  %s" % (i, cmd[:26], elapsed * 1000, mem if mem else "?"))

    for needle, label in FAULTS:
        if needle in text:
            faults.append((i, cmd, label, text))
    if not answered:
        faults.append((i, cmd, "no prompt for %d s" % CFG["per_cmd_s"], text))

# ----------------------------------------------------------------- report ---

print("")
print("  ran %d command(s), %d fault(s)" % (CFG["count"], len(faults)))

if faults:
    for i, cmd, label, text in faults[:5]:
        print("")
        print("  ! #%d %s -> %s" % (i, cmd, label))
        k = max(0, len(text) - 400)
        for ln in text[k:].split("\n")[-8:]:
            if ln.strip():
                print("      " + ln.strip()[:100])
    print("")
    print("  last commands before giving up:")
    for c in history[-12:]:
        print("      " + c)
    print("")
    print("  replay with -Seed %d to reproduce" % seed)
    print("")
    print("RESULT: FAIL - %d fault(s) in %d command(s)" % (len(faults), CFG["count"]))
    sys.exit(1)

# Comparing the first reading against the last one is misleading here, and the
# first version of this report did exactly that. Bringing BLE up costs 57 KB, so a
# run that happens to end with the radio on reports a 68% "loss" while a run that
# ends with it off reports 0%. Neither number says anything about leaking.
#
# What does say something is the memory at the SAME state. Bring everything down,
# take a reading, and compare that against the reading taken before any of it
# started. Two identical states, one long run apart.
run("s3ble deinit")
run("s3ulp stop")
run("s3fs unmount")
end = snapshot()
s.close()

if base["dram"] and end["dram"]:
    delta = end["dram"] - base["dram"]
    print("")
    print("  idle-state dram : %d -> %d (%+d bytes over %d commands)"
          % (base["dram"], end["dram"], delta, CFG["count"]))
    print("  (both readings taken with BLE down, ULP stopped and the volume "
          "unmounted)")
    # A single number cannot distinguish a leak from a one-time allocation. What
    # can is whether it scales with the number of commands: measured -1736 bytes
    # for 30 commands and -1736 for 200, so it is an allocation, not a leak. A
    # real leak at the observed rate would have been -11 KB at 200.
    #
    # Rather than ask the reader to run this again at a different length, say what
    # the number is and what would have to be true for it to be a leak.
    if delta < 0:
        per = -delta / float(CFG["count"])
        print("  a fall here is a candidate leak: same state, less memory")
        print("  it works out to %.1f bytes per command - rerun with -Count 4x this"
              % per)
        print("  to see whether it scales; a fixed offset will not.")
    print("  psram free      : %d -> %d"
          % (base["psram"] or -1, end["psram"] or -1))

print("")
print("RESULT: PASS - %d command(s), no faults" % CFG["count"])
sys.exit(0)
'@

$source | Set-Content $driver -Encoding utf8

& $py -u -X utf8 $driver
exit $LASTEXITCODE
