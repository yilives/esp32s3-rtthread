# 从零搭建

这份文档假设你手上有一块 ESP32-S3 开发板和一个干净的 Windows 环境。
Linux/macOS 同理，只需把`.ps1`换成`.sh`。

---

## 1. 准备 ESP-IDF

### 为什么不能用官方 v6.1

本移植需要 IDF 侧的几个开关，官方 release 里没有：

| 开关 | 作用 |
| --- | --- |
| `CONFIG_IDF_RTOS_RTTHREAD` | 让 IDF 的组件在 RT-Thread 上跑而不是 FreeRTOS |
| `CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM` | 启用 FreeRTOS API 兼容层 |
| `CONFIG_IDF_RTTHREAD_HEAP_SIZE` | RT-Thread 内核堆大小（独立于 IDF 的 heap_caps） |

这些来自一个 fork。**必须用这个 fork 的确切提交**——本仓库的 IDF 补丁是针对它的
基线写的，换一个 IDF 树补丁打不上。

```powershell
git clone https://github.com/yilives/esp-idf-rtt-c5.git <IDF_PATH>
cd <IDF_PATH>
git checkout 6b6076c8e20206f3e627bf0ab6791bdebf9e7c27
```

仓库名里的 `c5` 是历史原因：这个 fork 起初为 ESP32-C5 做的，后来扩展到了
S3/S31 和 C3/C6。**支持 S3 的是 `main` 分支上的这个提交。**

> **不要用 `RT-Thread-packages/esp-idf`。** 它的 `rtt-dev` 分支缺少本移植需要的
> S3 启动、双核 flash 握手和 FreeRTOS shim 集成。实测在本仓库 `0001` 补丁上
> `git apply --check` 会失败（`esp_libc/src/locks.c`、`esp_system/esp_ipc.c`、
> `freertos/port_systick.c`、`spi_flash/cache_utils.c` 四个文件）。
>
> 早先的文档写的是 `git clone -b rtt-s31 https://github.com/RT-Thread-packages/esp-idf.git`，
> **那个分支不存在**（`ls-remote` 只返回 `master`、`rtt-dev`、`rtt-dev-2024`）。
> 照做的人会卡在 clone 这一步。

### 为什么不提供独立的 IDF patch

一个能把官方 IDF 变成这个 fork 的等价补丁需要覆盖 S3 的启动、中断、双核 flash
握手和 Kconfig 分支，那是这个 fork 本身的工作量。**这里只提供叠加在 fork 之上的
那部分**（`esp-idf/patches/0001-*.patch`，17 个文件）。

### 装工具链

```powershell
cd <IDF_PATH>
.\install.ps1 esp32s3
```

这一步会装：

- `xtensa-esp-elf-gcc` 15.2.0（主工具链）
- `riscv32-esp-elf-gcc` 15.2.0（**只有 ULP 需要**，但缺了构建会失败）
- `cmake`、`ninja`、`ccache`
- Python 环境

> **易错点**：`build.ps1` 需要把 riscv32 的路径也加到 `PATH`。
> ULP 的构建是一个嵌套的 CMake 项目，它用 `find_program` 找 `riscv32-esp-elf-gcc`，
> 找不到就报 `CMAKE_C_COMPILER is not a full path`。

---

## 2. 准备 RT-Thread

```powershell
# 不要加 --depth 1：下面要 checkout 的是一个历史提交，浅克隆里没有它。
git clone https://github.com/RT-Thread/rt-thread.git <RTT_ROOT>
cd <RTT_ROOT>
git checkout 9e6dec3b14f5bf5514b0da829736c2485c38b0a7   # 5.4.0
```

> **`--depth 1` 与 `checkout <历史提交>` 不能同时用。** 实测在
> `git clone --depth 1` 的树里 `git cat-file -e 9e6dec3b...` 返回非零——
> 那个提交根本不在对象库里，`checkout` 必然失败。要节省带宽可以用
> `--filter=blob:none`，它保留历史但按需取文件内容。

### 应用本仓库的改动

```powershell
cd <本仓库>
.\tools\fetch-upstream.ps1 -RtThreadPath <RTT_ROOT> -IdfPath <IDF_PATH>
```

手工做就是三步：

```powershell
# 1. BSP 和架构层
Copy-Item .\rt-thread\bsp\ESP\ESP32_S3  <RTT_ROOT>\bsp\ESP\ -Recurse
Copy-Item .\rt-thread\libcpu\xtensa     <RTT_ROOT>\libcpu\ -Recurse

# 2. 内核补丁
cd <RTT_ROOT>
git apply <本仓库>\rt-thread\patches\0001-kernel-smp-hooks.patch

# 3. IDF 组件 + 补丁
Copy-Item <本仓库>\esp-idf\components\rtthread <IDF_PATH>\components\ -Recurse
cd <IDF_PATH>
git apply <本仓库>\esp-idf\patches\0001-idf-core-smp-hooks.patch
```

> **只有一个 IDF 补丁。** 早先这里还写着 `0002-freertos-shim-iram.patch`，
> **那个文件不存在**。把 shim 的 ISR 路径放进 IRAM 的做法改成了链接片段
> `esp-idf/components/rtthread/linker.lf`，它随组件目录一起复制，不需要单独打补丁。

### 内核补丁改了什么

| 文件 | 改动 | 为什么 |
| --- | --- | --- |
| `src/components.c` | `main` 线程绑到 CPU0 | 否则 `main` 落在 CPU1，而 CPU0 的 IPI 不通，`nvs_flash_init()` 会挂死 |
| `src/scheduler_comm.c` | tick 在 `current_thread == NULL` 时直接返回 | 启动早期 tick 已开但调度器还没装第一个线程，否则是解引用 0 |
| `src/scheduler_mp.c` | 调度器启动的双核汇合点 | 两核都要进调度器 |
| `src/thread.c` | 删一个空行 | 无功能影响 |

---

## 3. 构建

```powershell
cd <本仓库>
.\tools\build.ps1 -Project .\project\s3-rtt-test
```

脚本做的事：

1. 设 `IDF_PATH`、`IDF_PYTHON_ENV_PATH`
2. 拼 `PATH`（xtensa + riscv32 + cmake + ninja + ccache + esptool）
3. 调 `idf.py -B build build`

**成功输出**：

```
Project build complete. To flash, run:
 idf.py flash
...
s3_rtt_test.bin binary size 0x... bytes
```

### 常见构建失败

| 错误 | 原因 | 修法 |
| --- | --- | --- |
| `xtensa_context.h: No such file` | `RTT_IDF_PATH` 未设 | 见下节 |
| `configNUMBER_OF_CORES must be defined` | sdkconfig 被改坏 | 删 `sdkconfig` 重新生成 |
| `riscv32-esp-elf-gcc ... not found` | PATH 缺 riscv32 | 检查 `build.ps1` 的 `$pathParts` |
| `ulp_riscv.h: No such file` | `ulp` 在 `PRIV_REQUIRES` 里 | 改成 `REQUIRES`（见 [ULP.md](ULP.md)） |
| `region 'dram0_0_seg' overflowed` | RT-Thread 堆太大 | 调小 `CONFIG_IDF_RTTHREAD_HEAP_SIZE` |

### 关于 `RTTHREAD_ROOT`

项目要找到上游 RT-Thread 树。解析顺序：

1. `-DRTTHREAD_ROOT=<path>`（idf.py 命令行）
2. 环境变量 `RTTHREAD_ROOT`
3. 仓库旁边的 `../rt-thread`

**不要手写绝对路径进 CMakeLists。** 之前这里写死了 `I:/RT_thread/rt-thread`，
后果不是报错而是**静默用错树**——CMake 发现那个路径存在就直接用了，
你以为在验证 A 树，实际编的是 B 树。这是验证交付流程时才发现的。

`build.ps1` 会把 `RTTHREAD_ROOT` 转成**正斜杠**再导出。这不是洁癖：
`components/rtthread/CMakeLists.txt` 从环境变量直接读它并塞进 `INCLUDE_DIRS`，
ESP-IDF 再传给 `foreach()`，而反斜杠在 CMake 里是转义符——

```
Invalid character escape '\R'.
```

### 关于 `ESP_IDF_VERSION`

`build.ps1` 现在会自己推导它（`tools/cmake/version.cmake` → `6.1.0`）。
不设的话，全新检出的第一次 configure 会在 `idf_component_manager` 里崩：

```
TypeError: expected string or bytes-like object, got 'NoneType'
```

只影响第一次 configure——已有 `build/` 目录时不会走那段代码。

### 关于 `sdkconfig.defaults`

它是从一台**真机验证过的 `sdkconfig` 完整复制**的，不是手工挑的几十行。

手工版本漏掉了整个 ULP 配置块，导致全新检出编出来的镜像里 `s3ulp` 命令在
但 ULP 二进制不存在。生成自实际跑过的配置比手挑的可靠。

IDF 会忽略与目标无关或自动推导的项，所以带上它们无害。

### 关于 `RTT_IDF_PATH`

如果你不用 `build.ps1` 而直接跑 `idf.py`，BSP 的 scons 脚本会找不到 IDF 头文件。
设环境变量：

```powershell
$env:RTT_IDF_PATH   = "<IDF_PATH>"
$env:RTT_EXEC_PATH  = "C:\...\xtensa-esp-elf\bin"
```

---

## 3a. 第一次构建的 git 噪音

全新克隆第一次构建，可能会看到这类输出：

```
fatal: no submodule mapping found in .gitmodules for path 'docs/<某目录>/docs'
fatal: not a git repository: .../esp-idf-rtt/.git/modules/components/openthread/...
```

**这不是构建失败。** 构建继续跑，最后照样 `Project build complete`。

### 它从哪来

CMake 在配置阶段会调 `git submodule` 去查询 IDF 树的子模块状态。如果那棵 IDF 树
是打过补丁的本地副本，它的 git 索引里可能留着**没有 `.gitmodules` 映射的 submodule
记录**——比如上面那个路径，索引里是 `160000` 模式（子模块），但 `.gitmodules` 里
没有对应条目。

`git` 遇到这种不一致会报 `fatal:`，但 CMake **不把它当错误**，继续配置。

### 怎么确认构建真的成功了

看两样东西：

```powershell
# 1. 二进制存在且非空
Get-Item .\project\s3-rtt-test\build\s3_rtt_test.bin

# 2. 最后一行是这个
#    Project build complete. To flash, run:
```

**别只看 stderr 里有 `fatal` 就以为失败。** 这是本项目唯一一处 stderr 有红色输出
但构建成功的地方。

实测：删掉 `build/` 后首次构建，把输出重定向到文件再查 `$LASTEXITCODE`，结果是
**0**。只有在把输出通过管道（`| Select-Object`）转给别的命令时，PowerShell 才会把
native stderr 当成错误记录，于是显示 `[exit code: 1]`——**那个 1 来自 PowerShell
的错误处理，不是构建本身**。

想确认构建真的成功，最可靠的是这两条：

```powershell
powershell -NoProfile -File .\tools\build.ps1 -Project .\project\s3-rtt-test *> build.log
"exit code: $LASTEXITCODE"        # 0 就是成功
Select-String -Path build.log -Pattern "Project build complete"
```

### 想让噪音消失

在 IDF 树里把那条野记录去掉：

```powershell
cd <IDF_PATH>
git rm --cached <那个路径>     # 只从索引移除，不删磁盘文件
```

或者忽略它——不影响构建结果。

---

## 4. 烧录

```powershell
$env:ESP_ROM_ELF_DIR = 'C:\Users\<you>\.espressif\tools\esp-rom-elfs\20241011\'

python -m esptool --chip esp32s3 -p COM19 -b 921600 write-flash `
  --flash-mode dio --flash-freq 80m --flash-size 8MB `
  0x0     .\project\s3-rtt-test\build\bootloader\bootloader.bin `
  0x8000  .\project\s3-rtt-test\build\partition_table\partition-table.bin `
  0x10000 .\project\s3-rtt-test\build\s3_rtt_test.bin
```

三个偏移不能改：

- `0x0` bootloader
- `0x8000` 分区表
- `0x10000` 应用

`$env:ESP_ROM_ELF_DIR` 是给 `esptool` 用的 ROM ELF 目录，用于生成/校验镜像。
不设的话某些 esptool 版本会警告或失败。

### 分区表

`project/s3-rtt-test/partitions.csv`：

```
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x6000
phy_init, data, phy,     0xf000,   0x1000
factory,  app,  factory, 0x10000,  0x300000
storage,  data, fat,     0x310000, 0x100000
```

- 应用 3 MB —— 链接了 Wi-Fi + BLE 之后 1 MB 放不下
- `storage` 1 MB —— FAT 文件系统
- 剩余约 4.9 MB 未分配，留给以后加 OTA 或扩分区

---

## 5. 连控制台

本移植用 UART0（GPIO43/44）。CH340K 之类的 USB 串口芯片会枚举成 `COMx`。

```python
import serial, time

s = serial.Serial()
s.port = 'COM19'
s.baudrate = 115200
s.timeout = 0.4
s.dtr = False
s.rts = False
s.open()
s.reset_input_buffer()

# 用 RTS 复位
s.rts = True
time.sleep(0.2)
s.rts = False

raw = b''
t = time.time()
while time.time() - t < 20:
    raw += s.read(8192)

print(raw.decode('utf-8', 'replace'))
```

**关键点**：

- `dtr=False, rts=False` 在 `open()` **之前**设，否则某些 CH340 会拉着芯片进 bootloader
- 复位用 `rts`，不是 `dtr`
- 上电到 shell 出来约 15-18 秒（PSRAM 校准 + Wi-Fi/BLE 初始化）

**期望看到**：

```
ESP-ROM:esp32s3-20210327
...
I (352) cpu_start: Multicore app
...
 \ | /
- RT -     Thread Operating System
 / | \     5.4.0 build Nov  8 2026
 2006 - 2025 Copyright by rt-thread team
msh >
```

---

## 5a. 第一次构建：确认 `build/` 是空的

```powershell
Remove-Item .\project\s3-rtt-test\build -Recurse -Force -ErrorAction SilentlyContinue
```

**这不是可选的谨慎，是必要的验证。** 一个已有的 `build/` 会把
`RTTHREAD_ROOT` 留在 CMake cache 里，于是即使顶层 CMakeLists 有 bug 也能编过。
删掉之后重新 configure 才能证明这套 CMake 是自洽的。

曾经有两个这样的 bug 同时存在，只在空 `build/` 下暴露：

1. sibling 路径少上一级，找不到同级的 RT-Thread 检出
2. 顶层用普通 `set()` 设 `RTTHREAD_ROOT`，而组件读的是
   `CACHE{RTTHREAD_ROOT}`——两个变量在 CMake 里不是一回事

两个都已修复，但**下次改动 CMake 后请仍然从空目录验证一次**。

## 5b. 确认上游树是干净的

```powershell
.\tools\verify-tree.ps1
```

**期望**：

```
  expected (26) - this repository ships or patches these:
    ...
  No foreign changes.
```

出现 `FOREIGN` 就是有问题：说明上游树里有本仓库不知道的改动，而它们会让你的
构建和本仓库描述的镜像不一致。

**这条建议来自一次真实的误判。** 本地 `components/lwip/lwip/src/api/tcpip.c`、
`components/lwip/port/freertos/sys_arch.c`、
`components/esp_netif/lwip/esp_netif_lwip.c` 里各有未提交的调试代码，
其中一处用了 lwip 里不可见的 `rt_uint32_t`，直接编不过；还有一处改变了内存
布局，让 `s3ble` 表现出两种不同的失败形态。清掉之后同一测试 6/6、Guru 0。

`-Fix` 会逐个询问是否回退。未跟踪的文件不会被自动处理——`git clean` 会删文件，
不该替你做这个决定。

## 5c. 核对文档输出（有板子时）

```powershell
.\tools\check-docs.ps1 -Port COM7
```

**不带 WiFi 凭据也能跑**，会跑 47 节：

```powershell
# 只有板子，没有 WiFi 环境
.\tools\check-docs.ps1 -Port COM7
  -> RESULT: PASS - all 47 section(s) matched
```

跳过的三节是 `s3net example.com` 和两个 `s3wifi`——它们要真网络。

**`s3wifiscan` 不在跳过之列**，即使没有凭据：它不接受参数，只需要附近有个
2.4 GHz AP。想连这一步也跳过就用 `-Only` 挑节。

```powershell
# 有 WiFi 环境，跑全部 50 节
.\tools\check-docs.ps1 -Port COM7 -WifiSsid <ssid> -WifiPass <pass>
  -> RESULT: PASS - all 50 section(s) matched
```

### 下面的内容（逐节说明）

```powershell
.\tools\check-docs.ps1 -WifiSsid <ssid> -WifiPass <pass>
```

它在板上按顺序跑文档里出现的所有命令，比对实际输出与
`tools/expected-output.txt` 里的正则。

**期望**：

```
RESULT: PASS - all 19 section(s) matched
```

改动任何命令的输出格式后都应该跑一次——否则文档会悄悄过时。这个脚本第一次
跑就发现了 `s3wifiscan` 的两个崩溃，而那条命令在之前的回归里从未被执行过。



> `-NoPsram` 曾经**完全没生效**：注释说建到 `build-qemu/`，实际建到 `build/`，
> 两个变体互相覆盖。原因是 `-B build-qemu` 从没传给 idf.py——`-B` 是 idf.py 的
> *全局*选项，必须放在子命令前面，而那段代码的位置错了。现在已验证：
> `build/` 是硬件镜像（有 PSRAM、QIO），`build-qemu/` 是模拟器镜像（无 PSRAM、DIO）。


## 6. QEMU（可选）

没有硬件时可以用 QEMU 跑回归。注意 QEMU 不模拟 PSRAM，需要另一套 sdkconfig：

```powershell
.\tools\build.ps1 -Project .\project\s3-rtt-test -NoPsram
.\tools\qemu.ps1  -Project .\project\s3-rtt-test -BuildDir build-qemu -TimeoutSec 70
```

**QEMU 里也能看到两个核。** gate 会跑 `s3smp` 并检查 `CPUS_NR=2` ——
原来的 5 条期望全都能被单核启动满足，所以 CPU1 起没起来它不知道。

```
s3smp: CPUS_NR=2, this core=0
  cpu0: switches=82 current=tshell
  cpu1: switches=60 current=tidle1
```

**期望**：

```
RESULT: PASS - all 9 expectation(s) matched
```

九个断言除了内核启动、双核、tick、UART 中断和自检，还覆盖：

| 断言 | 为什么值得加 |
| --- | --- |
| `s3fs: PASS, mounted at` | QEMU 模拟了 SPI flash 分区，文件系统**完整可用**——挂载、写 511 字节、读回。这是唯一能在无硬件时验证的文件系统路径 |
| `psram : built without PSRAM support` | QEMU 镜像用 `-NoPsram` 编译，命令必须明确报告，而不是含糊失败 |

### QEMU 验证不了什么

GPIO 在 QEMU 里恒为 0，所以依赖真实引脚的命令会**如实失败**而非假装成功：

```
i2cscan: idle levels SCL(GPIO9)=0 SDA(GPIO8)=0 (bus NOT idle high ...)
i2cscan: 0 device(s) on i2c0, 0 flaky, 112 timeout(s)
```

板子上同一条命令是 `0 timeout(s)`。**这些命令不能进 QEMU 门禁**，否则门禁会因为
模拟器的空白而失败，而不是因为移植的缺陷：

| 命令 | QEMU 下的表现 |
| --- | --- |
| `i2cscan` / `i2cprobe` | 112 次超时 |
| `spiloop` | 读回全零 |
| `s3adc` | 悬空读数 |
| `s3rt led` / `pwm` / `key` | 无输出可观测 |
| `s3ulp` | **明确 FAIL**：100 ms 后报 `magic 0x0 != 0x55334c50`，并附一句说明这是 QEMU 下预期结果 |
| `s3wifi` | **静默挂起**：走到 `phy_init` 的"full calibration"就停住，300 秒超时也不继续 |
| `s3ble` | **静默挂起**：打完 `bringing up the BLE controller` 一行就卡住，后续命令也不响应 |

三者的失败模式差别很大，值得分别记住：

- **`s3ulp` 是正确的那种**：QEMU 不模拟 ULP-RISC-V，程序从没执行，命令在 100 ms
  内如实报告，还提示了原因。
- **`s3wifi` 和 `s3ble` 是静默挂起**：它们需要真实射频，PHY 全校准和控制器初始化
  都会等一个永远不会到来的硬件事件。这种失败方式对使用者不友好——看起来像死机，
  而不是"这个功能在模拟器上不可用"。

`qemu-system-xtensa` 不模拟 WiFi/BT 射频，也不模拟 ULP-RISC-V 协处理器。这不是
移植的缺陷。

> **坑**：构建失败时 `qemu.ps1` 会静默跑上一次的镜像。
> 每次都确认 `.elf` / `.bin` 的时间戳是新的。

### 构建目录是独立的

`-NoPsram` 写 `build-qemu/`，不影响 `build/` 的硬件镜像。实测两个 `.bin`
的修改时间各自独立，可以交替构建。

---

### GDB

QEMU 停在复位点等 GDB：

```powershell
.\tools\qemu-gdb.ps1                    # 冻结
.\tools\gdb-bt.ps1 -StartQemu           # 起来 + attach + 打印双核 backtrace
```

**几条 GDB 命令在这个目标上会挂**，报错方式不像报错：

| 命令 | 结果 |
| --- | --- |
| `p rt_thread_self()->parent.name` | **挂死** —— inferior function call 需要往目标注入代码，跑着的 QEMU 做不到 |
| `p rt_tick` | `No symbol` —— RT-Thread SMP 的 tick 在 `struct rt_cpu` 里，每核一份 |
| `info threads` / `bt` / `p <全局变量>` | ✅ 正常 |

所以 `gdb-bt.ps1` 只做能做的：`monitor stop` 后打印 `info threads` 和两核 backtrace。

## 7. 下一步

跑通了就去看 [TUTORIAL.md](TUTORIAL.md)，逐个验证内核、外设、无线、文件系统、ULP。
