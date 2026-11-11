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

这些来自一个 fork。你可以：

**选项 A：用现成的 fork（推荐）**

```powershell
git clone -b rtt-s31 https://github.com/RT-Thread-packages/esp-idf.git <IDF_PATH>
cd <IDF_PATH>
git checkout <commit>
```

**选项 B：自己给官方 IDF 打补丁**

需要一个定义这三个 Kconfig 和对应 CMake 分支的 patch。当前仓库没有单独提供
（它假定你在用上面的 fork），如果你需要，提 issue。

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
git clone --depth 1 https://github.com/RT-Thread/rt-thread.git <RTT_ROOT>
cd <RTT_ROOT>
git checkout 9e6dec3b14f5bf5514b0da829736c2485c38b0a7   # 5.4.0
```

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
git apply <本仓库>\esp-idf\patches\0002-freertos-shim-iram.patch
```

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

## 6. QEMU（可选）

> `-NoPsram` 曾经**完全没生效**：注释说建到 `build-qemu/`，实际建到 `build/`，
> 两个变体互相覆盖。原因是 `-B build-qemu` 从没传给 idf.py——`-B` 是 idf.py 的
> *全局*选项，必须放在子命令前面，而那段代码的位置错了。现在已验证：
> `build/` 是硬件镜像（有 PSRAM、QIO），`build-qemu/` 是模拟器镜像（无 PSRAM、DIO）。


没有硬件时可以用 QEMU 跑回归。注意 QEMU 不模拟 PSRAM，需要另一套 sdkconfig：

```powershell
.\tools\build.ps1 -Project .\project\s3-rtt-test -NoPsram
.\tools\qemu.ps1  -Project .\project\s3-rtt-test -BuildDir build-qemu -TimeoutSec 70
```

**QEMU 里也能看到两个核。** gate 现在会跑 `s3smp` 并检查 `CPUS_NR=2` ——
原来的 5 条期望全都能被单核启动满足，所以 CPU1 起没起来它不知道。

```
s3smp: CPUS_NR=2, this core=0
  cpu0: switches=82 current=tshell
  cpu1: switches=60 current=tidle1
```

**期望**：

```
RESULT: PASS - all 5 expectation(s) matched
```

> **坑**：构建失败时 `qemu.ps1` 会静默跑上一次的镜像。
> 每次都确认 `.elf` / `.bin` 的时间戳是新的。

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
