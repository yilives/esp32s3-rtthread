# 手把手教程

从"板子刚插上"到"每个功能都验证过一遍"。

每一步都给出**期望输出**。对不上就去看 [TROUBLESHOOTING.md](TROUBLESHOOTING.md)。

---

## 第 0 步：确认基础能跑

```powershell
.\tools\build.ps1 -Project .\project\s3-rtt-test
# 烧录（见 BUILD.md 第 4 节）
```

打开串口，看到 `msh >` 提示符。

```
msh >s3rt
s3rt v0.1 - ESP32-S3 / RT-Thread 5.4.0
  tick      : 3009, 1000 Hz
  current   : tshell at priority 20
  counters  : a=4 b=6
  kernel    : total 130976, used 27000, maximum 31000
  commands  : s3rt tick, s3rt irq, s3rt psram, s3rt led, s3rt pwm, list thread, free
```

**看 `verdict`**：`s3rt` 末尾会跑一个自检，输出 `verdict : PASS` 或 `FAIL`。

---

## 第 1 步：内核

### 1.1 时间片和线程切换

```
msh >s3rt
```

看这两个数：
- `tick` —— 每次调用应该增长
- `counters : a=N b=M` —— 两个工作线程各自的自增计数，都应该在涨

如果其中一个不涨，说明那个线程没被调度。

### 1.2 双核

```
msh >s3smp
s3smp: CPUS_NR=2, this core=1
  cpu0: switches=44 current=tidle0 lock_nest=0 irq_nest=0
  cpu1: switches=102 current=tshell lock_nest=0 irq_nest=0
```

**四个数都要看**：

| 字段 | 期望 | 不正常意味着 |
| --- | --- | --- |
| `cpu0 switches` | > 0 且在涨 | CPU0 没在调度 |
| `cpu1 switches` | > 0 且在涨 | CPU1 没参与调度 |
| `lock_nest` | **0** | 有临界区没退出 |
| `irq_nest` | **0** | 有中断没返回 |

连续跑几次，`switches` 必须单调递增。**`lock_nest` 或 `irq_nest` 不为 0 是硬故障**。

### 1.3 线程列表

```
msh >list thread
```

看 `tshell`、`tidle0`、`tidle1`、`main`、`timer` 等线程。两个 idle 线程说明双核都活着。

---

## 第 2 步：外设

### 2.1 GPIO + LED

```
msh >s3rt led
```

板载 LED（GPIO48）应该闪几下。这块板子的 LED 是**灌电流**接法——低电平点亮。

### 2.2 按键中断

按下 BOOT 键（GPIO0），或者：

```
msh >s3rt key pulse
```

**期望**：中断计数 +1。这是验证 `rt_pin_attach_irq → esp_intr_alloc` 这条路。

### 2.3 PWM

```
msh >s3rt pwm
```

LED 会从暗到亮再到暗。这条验证 LEDC。

### 2.4 I2C / SPI / ADC

```
msh >i2cscan                      # 扫描 I2C 总线（i2c0 = GPIO8/9）
msh >i2cprobe 0x50 8              # 只探一个地址 8 次，排查"flaky"
msh >spiloop                      # SPI 回环，需短接 MOSI-MISO
msh >s3adc 1 3                    # 读 ADC1 通道 3（GPIO4）
```

**裸板上 `i2cscan` 应该报 `0 device(s) ... 0 flaky`。** 如果 flaky 不为 0 且每次不同，
那不是信号问题——是驱动的错误位判定有 bug（已在 `i2c.c` 修掉，见
[KNOWN-ISSUES.md](KNOWN-ISSUES.md)）。

---

## 第 3 步：看门狗

这是**唯一会让板子重启**的测试，但必须做——不做就不知道看门狗是不是真的接了硬件。

### 3.1 确认已武装

```
msh >s3wdt
s3wdt: armed=1 feeds=26 paused=0 timeout=5000 ms period=1250 ms
s3wdt: `s3wdt hang` proves the reset path
```

`feeds` 每次调用都应该涨——喂狗线程在跑。

### 3.2 触发真实复位

```
msh >s3wdt hang
s3wdt: suspending the feeder and spinning; the board must reset in ~5000 ms
s3wdt: feeds so far = 26
```

然后**什么都别做**，等 5 秒。板子会自己重启。

**期望重启原因**：

```
rst:0x7 (TG0WDT_SYS_RST)
```

`TG0WDT_SYS_RST` 是关键。如果是 `RTCWDT` 或 `POWERON`，说明不是我们的看门狗触发的。

> **这一步验证了什么**：看门狗接到了真实的硬件复位，而不是一个没人处理的中断。

---

## 第 4 步：文件系统

### 4.1 挂载（开机自动）

```
msh >s3fs read
s3fs: already mounted at /storage
s3fs read: FAIL, /storage/s3fs.txt not found - run `s3fs` first
```

第一次启动没有文件是正常的。

### 4.2 写 + 读

```
msh >s3fs
s3fs: PASS, mounted at /storage, wrote and read back 511 bytes
s3fs: /storage/s3fs.txt left in place; reboot and rerun to prove persistence
```

### 4.3 证明跨重启持久

**复位板子**，然后：

```
msh >s3fs read
s3fs: already mounted at /storage
s3fs read: PASS, 511 bytes intact after reboot
```

> `s3fs` 自己写自己读，在刚格式化的分区上也会 PASS。
> **只有 `s3fs read` 不带写，才能区分"文件系统能用"和"数据真的活过了重启"。**

### 4.4 性能基准（可选）

```
msh >s3fsbench
```

实测数据（1 MB 分区，4 KB 扇区）：

```
s3fsbench: write  256 KB in 1064 ms -> 240 KB/s
s3fsbench: read   256 KB in 30 ms -> 8533 KB/s
s3fsbench: create 32 x 64 B in 5281 ms (6 files/s, 165 ms each)
s3fsbench: delete 32 files in 3512 ms (109 ms each)
```

**小文件慢是正常的**，原因和调优见 [FILESYSTEM.md](FILESYSTEM.md)。

### 4.5 擦除分区（Wi-Fi 会先被停掉）

```
msh >s3fs unmount
msh >s3fs erase
s3fs: erasing 1048576 bytes at 0x310000 ...
s3fs: Wi-Fi suspended for the erase
s3fs: Wi-Fi restarted and reconnecting
s3fs: erased; next `s3fs` will format and mount
```

**擦除期间无线栈必须停。** Wi-Fi 自己的 NVS 写会和长擦除交错，导致 flash 的
跨核握手不再完成——表现为静默挂死，没有 panic 也没有看门狗。`s3fs erase` 现在
自己做这件事，但如果手工调用 `esp_partition_erase_range()` 擦大块，要自己先
`s3wifi stop`。详见 [KNOWN-ISSUES.md](KNOWN-ISSUES.md) 第 2 条。

### 4.6 分区坏了怎么办

如果 `s3fs` 报 `FAIL, cannot open ... errno=13 (Permission denied)`：

```
msh >s3fs unmount
msh >s3fs erase
（复位板子，让开机挂载重新格式化）
```

---

## 第 5 步：Wi-Fi

> **顺序很重要**：Wi-Fi 必须先于 BLE 启动。反过来会 `deauth reason=1`。
> 这是已知限制，见 [KNOWN-ISSUES.md](KNOWN-ISSUES.md)。

### 5.1 先扫一下

```
msh >s3wifiscan
```

确认能看到你的 AP。

### 5.2 连接

> **如果 BLE 已经在跑**，`s3wifi` 会先自动把它拆掉再连（打印
> `BT up: unloading first ok`）。连上之后可以用 `s3ble` 重新打开，
> 不影响已建立的链路。这是一个工作区，根因见
> [KNOWN-ISSUES.md](KNOWN-ISSUES.md) 第 1 条。

```
msh >s3wifi 你的SSID 你的密码
```

**期望**：

```
s3wifi: bringing up esp_wifi on the FreeRTOS shim
  nvs_flash_init             ok
  esp_netif_init             ok
  event loop create default  ok
  create default wifi sta    ok
  event handler register     ok
  ip handler register        ok
s3wifi: STA_START in 10 ms, mac 3c:84:27:c7:4c:00
s3wifi: PASS, connected to "..." in 1620 ms
s3wifi: ip 10.0.0.126 mask 255.255.255.0 gw 10.0.0.1
```

**没有拿到 IP 就是失败**，即使 `connected` 为真。关联成功但 DHCP 没成，链路不可用。

### 5.3 TCP + DNS

```
msh >s3net example.com
```

**期望**：

```
s3net: resolving example.com ...
  gethostbyname          ok -> 172.66.147.243
  socket                 ok (56)
  connect                ok (0)
  send                   ok (81)
  recv                   ok (512 bytes)
s3net: PASS, 512 byte(s) received over TCP
```

> **注意**：socket 走的是 **lwIP 原生 API**（`lwip_socket` / `lwip_connect`），
> 不是 RT-Thread 的 SAL。所以 `ifconfig` / `ping` 这些命令**不存在**。

### 5.4 断线重连

```
msh >s3wifi drop
```

**期望**：

```
s3wifi drop: disconnecting on purpose, retry budget at 0
s3wifi drop: dropped after 10 ms, reason=8, retries now 1, waiting for reconnect...
s3wifi drop: PASS, reconnected in 1880 ms
s3wifi drop: ip 10.0.0.126 mask 255.255.255.0 gw 10.0.0.1
```

**连续跑 5 次**，每次都应该 PASS。`retry budget at 0` 每次都是 0——
说明拿到 IP 后重试预算被重置了。这是修过的一个 bug：原来预算只在命令开头清零，
设备一生中累计掉线 4 次后就再也不重连。

---

## 第 6 步：BLE

```
msh >s3ble
```

**期望**：

```
s3ble: bringing up the BLE controller on the FreeRTOS shim
  nvs_flash_init               ok
ble_gatts_count_cfg          ok
ble_gatts_add_svcs           ok
s3ble: PASS, host synced in 30 ms, addr 3c:84:27:c7:4c:02
s3ble: advertising as "s3-rtt" (service 0x00FF, chr 0xFF01 read, 0xFF02 write)
```

### 6.1 从电脑验证

用任何 BLE 扫描工具（nRF Connect、或者 Python 的 bleak）：

```python
import asyncio
from bleak import BleakScanner, BleakClient

SVC   = "000000ff-0000-1000-8000-00805f9b34fb"
CHR_C = "0000ff01-0000-1000-8000-00805f9b34fb"   # 只读
CHR_L = "0000ff02-0000-1000-8000-00805f9b34fb"   # 只写

async def main():
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or "") == "s3-rtt", timeout=15.0)
    print("found:", dev.address)

    async with BleakClient(dev, timeout=20.0) as cli:
        print("services:",
              [s.uuid for s in cli.services if s.uuid.startswith("000000ff")])
        print("read 0xFF01:", (await cli.read_gatt_char(CHR_C)).hex())
        await cli.write_gatt_char(CHR_L, bytes([1]), response=True)   # LED on
        await asyncio.sleep(1)
        await cli.write_gatt_char(CHR_L, bytes([0]), response=True)   # LED off

asyncio.run(main())
```

**期望**：

```
found: 3C:84:27:C7:4C:02
services: ['000000ff-0000-1000-8000-00805f9b34fb']
read 0xFF01: 01000000
```

同时板子上会打印：

```
s3ble: MTU updated to 256
s3ble: connected, handle=1 (total 1)
s3ble: LED on via GATT write
s3ble: LED off via GATT write
s3ble: disconnected, reason=531 - advertising again
```

### 6.2 只停广播（保留控制器）

```
msh >s3ble adv off
s3ble adv: PASS, advertising stopped (controller still up)

msh >s3ble adv
s3ble adv: PASS, advertising again
```

用来区分"BLE 在占用射频"和"BLE 控制器开着"这两种不同状态。
实测：停掉广播并不能让 BLE→Wi-Fi 的顺序变得可用（0/1），
所以问题不在射频。

### 6.3 状态查询

```
msh >s3ble info
s3ble info: up=1 synced=1 advertising=1 conns=0 last_disc=531 adv_err=0
```

---

## 第 7 步：ULP 协处理器

先说明：**ESP32-S3 没有 LP core**。`SOC_LP_CORE_SUPPORTED` 是 C6/H2/P4 的。
S3 有 ULP-RISC-V——RTC 域里独立的 20 MHz RV32 核。

### 7.1 启动并验证

```
msh >s3ulp
```

**期望**：

```
s3ulp: loading 262 bytes into RTC slow memory
s3ulp: started, wakeup 1000 us, ISR attached
s3ulp: submitting CMD_COUNT, blocking...
s3ulp: PASS, ULP completed in 102 us, result=0x2faeb9e0, isrs=1
s3ulp: PASS, CMD_DOUBLE returned 2 x CMD_COUNT (0x5f5d73c0)
```

三个关键点：

- `pass, ULP completed` —— ULP 真的执行了并回了中断
- `result` 非零 —— 结果是从 RTC 内存读回来的
- `CMD_DOUBLE == 2 x CMD_COUNT` —— 证明结果确实来自 ULP 计算，不是固定缓冲

### 7.2 观察线程挂起

```
msh >s3ulpw
msh >list thread
```

找到 `ulpw` 这一行：

```
ulpw   N/A  2  15  suspend  0x00000130  0x00000800  17%  0x0000000a  EINTRPT
                   ^^^^^^^                                  ^^^^^^^
                   suspend 状态                             被中断唤醒
```

- `suspend` —— 线程真的挂起了，不是忙等
- `EINTRPT` —— 被中断唤醒，而不是超时

**这就是"RT-Thread 能调度 ULP"的实证**。

### 7.3 停止

```
msh >s3ulpw stop
s3ulpw: worker stopped after 1675 kicks, last result 0x2faeb9e0

msh >s3ulp stop
s3ulp: PASS, halted - no completions in 30 ms (total 4)

msh >s3ulp
s3ulp: stopped; run `s3ulp start` first
```

`stop` 会验证计数器确实冻结了——不是只打印一句"已停止"。

---

## 第 8 步：全功能同时跑

按正确顺序（Wi-Fi 先）：

```
msh >s3ulp start
msh >s3ulpw
msh >s3fs
msh >s3wifi 你的SSID 你的密码
msh >s3ble
msh >s3net example.com
msh >s3wifi drop
msh >s3wdt
msh >s3smp
msh >s3rt
```

**期望全部 PASS，且没有 `Guru`**。

---

## 下一步

- 移植是怎么做的：[PORTING.md](PORTING.md)
- 文件系统调优：[FILESYSTEM.md](FILESYSTEM.md)
- ULP 协议设计：[ULP.md](ULP.md)
- 出问题：[TROUBLESHOOTING.md](TROUBLESHOOTING.md)
