# Run the documented commands repeatedly on one boot and look for drift.
#
# Why: every check in this repository is a single run. The defects recorded in
# docs/KNOWN-ISSUES.md that were hardest to find were the ones that only appear
# after a while - an `errno=113` that showed up once in a long sequence, a Guru
# during a BLE restart. A single green run says nothing about those.
#
# What it watches, between iterations:
#   - heap: internal DRAM free, PSRAM free, and the RT-Thread kernel heap
#   - counters that should be monotonic but not runaway: irq entries, tick
#   - any Guru, watchdog reset, assert, or Malloc failure in the captured text
#
# What it does NOT do: judge the command output. That is check-docs.ps1's job.
# This answers a different question - does the board still behave the same after
# having done all of it N times.
#
# Usage:
#   .\soak-docs.ps1                                  # 10 iterations
#   .\soak-docs.ps1 -Iterations 30
#   .\soak-docs.ps1 -WifiSsid <ssid> -WifiPass <pass> # include the network steps
#   .\soak-docs.ps1 -Port COM7

param(
    [string]$Port = 'COM19',
    [int]$Baud = 115200,
    [int]$Iterations = 10,
    [string]$WifiSsid = '',
    [string]$WifiPass = '',
    # Seconds to wait after reset. PSRAM calibration takes 15-18 s here.
    [int]$BootSeconds = 36
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'env.ps1')

$py = $script:S3Python
if (-not (Test-Path $py)) { throw "python not found at $py" }

& $py -c "import serial" 2>$null
if ($LASTEXITCODE -ne 0) { throw "pyserial missing in $py" }

$driver = Join-Path $env:TEMP 's3-soak-docs.py'

$cfg = @{
    port     = $Port
    baud     = $Baud
    iters    = $Iterations
    boot_s   = $BootSeconds
    ssid     = $WifiSsid
    password = $WifiPass
}
($cfg | ConvertTo-Json -Compress) | Set-Content (Join-Path $env:TEMP 's3-soak-docs.json') -Encoding utf8

$source = @'
import json, io, os, re, sys, time
import serial

CFG = json.loads(io.open(os.environ["TEMP"] + r"\s3-soak-docs.json", encoding="utf-8-sig").read())

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

def run(cmd, wait):
    """Send a command and return only the text it produced."""
    mark = len(raw)
    s.write((cmd + "\r\n").encode())
    s.flush()
    t0 = time.time()
    err = None
    while time.time() - t0 < wait:
        try:
            raw.extend(s.read(4096))
        except serial.SerialException as e:
            err = e
            break
    out = bytes(raw[mark:]).decode("utf-8", "replace").replace("\r", "\n")
    if err:
        out += "\n@@SERIAL-READ-FAILED %s" % err
    return out

def grab(pattern, text, default=None):
    m = re.search(pattern, text)
    return m.group(1) if m else default

print("")
print("  %-4s %10s %10s %10s %8s %8s" %
      ("iter", "dram free", "psram free", "kern avail", "irq", "tick"))

rows = []
failures = 0

for i in range(1, CFG["iters"] + 1):
    # The workload: everything that touches an allocator, an interrupt or the
    # flash, in an order that keeps the documented preconditions.
    steps = [
        ("s3smp", 8),
        ("s3heap", 10),
        ("free", 8),
        ("s3rt irqlat", 18),
        ("s3rt psram", 20),
        ("i2cwatch 0 0x50", 12),
        ("s3fs", 70),
        ("s3fsbench small", 150),
        ("s3fs read", 25),
        ("s3ble", 50),
        ("s3ulp", 25),
    ]
    if CFG["ssid"]:
        steps += [
            ("s3wifi %s %s" % (CFG["ssid"], CFG["password"]), 80),
            ("s3net example.com", 35),
        ]

    iter_text = ""
    for cmd, wait in steps:
        iter_text += run(cmd, wait)

    # Snapshot after the workload.
    snap = run("s3heap", 12)
    kern = run("free", 8)
    rt   = run("s3rt", 12)
    irqs = run("s3rt irq", 8)

    dram  = grab(r"internal 8-bit :\s+(\d+) free", snap)
    psram = grab(r"PSRAM 8-bit\s+:\s+(\d+) free", snap)
    avail = grab(r"available:\s*(\d+)", kern)
    # The UART counter comes from `s3rt irq`, not `s3rt` - they are separate
    # commands, which is why this column read "?" on the first run.
    irq   = grab(r"uart0 irq : (\d+) entries", irqs)
    tick  = grab(r"tick      : (\d+),", rt)

    rows.append((i, dram, psram, avail, irq, tick))
    print("  %-4d %10s %10s %10s %8s %8s" %
          (i, dram or "?", psram or "?", avail or "?", irq or "?", tick or "?"))

    # Anything that means the board reset or faulted mid-iteration.
    bad = []
    for needle, label in [("Guru", "Guru"), ("rst:0x7", "watchdog reset"),
                          ("assert failed", "assert"),
                          ("Malloc failed", "Malloc failed"),
                          ("@@SERIAL-READ-FAILED", "serial read failed")]:
        if needle in iter_text:
            bad.append(label)
    if bad:
        failures += 1
        print("        !! %s" % ", ".join(bad))
        # Show where it happened, so the cause is not a guess.
        for needle in ("Guru", "assert failed", "Malloc failed"):
            k = iter_text.find(needle)
            if k >= 0:
                print("        ... %s" % iter_text[max(0, k - 120):k + 160]
                      .replace("\n", " | ")[:280])

s.close()

# ------------------------------------------------------------------ report ---

print("")

def as_int(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return None

drams  = [as_int(r[1]) for r in rows]
psrams = [as_int(r[2]) for r in rows]
avails = [as_int(r[3]) for r in rows]

def trend(name, vals, unit="bytes"):
    v = [x for x in vals if x is not None]
    if len(v) < 2:
        print("  %-12s no data" % name)
        return
    first, last = v[0], v[-1]
    delta = last - first
    print("  %-12s first %8d, last %8d, delta %+d %s" % (name, first, last, delta, unit))
    # A leak is a sustained fall, not one noisy sample. Compare the first third
    # against the last third.
    third = max(1, len(v) // 3)
    a = sum(v[:third]) / third
    b = sum(v[-third:]) / third
    drop = a - b
    if drop > 0:
        pct = 100.0 * drop / a if a else 0
        print("  %-12s first third avg %8.0f, last third avg %8.0f (%+.2f%%)" %
              ("", a, b, -pct))

print("")
trend("dram free", drams)
trend("psram free", psrams)
trend("kernel avail", avails)

print("")
if failures:
    print("RESULT: FAIL - %d of %d iteration(s) reported a fault" %
          (failures, CFG["iters"]))
    sys.exit(1)

print("RESULT: PASS - %d iteration(s), no fault markers" % CFG["iters"])
sys.exit(0)
'@

$source | Set-Content $driver -Encoding utf8

& $py -u -X utf8 $driver
exit $LASTEXITCODE
