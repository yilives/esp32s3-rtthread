# RT-Thread 移植到 ESP32-S3

[![RT-Thread](https://img.shields.io/badge/RT--Thread-5.4.0-blue)]()
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-6.1-red)]()
[![Target](https://img.shields.io/badge/target-ESP32--S3-green)]()
[![Arch](https://img.shields.io/badge/arch-Xtensa%20LX7-purple)]()
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)]()

把 **RT-Thread 5.4.0** 移植到 **ESP32-S3**（双核 Xtensa LX7、240 MHz、Wi-Fi + BLE、
8 MB flash + 8 MB octal PSRAM），用 RT-Thread 的调度器替换 FreeRTOS，ESP-IDF 的组件
（Wi-Fi、BLE、NVS、lwIP、FATFS）原样运行在 FreeRTOS API 兼容层之上。

**上板实测：SMP 双核同时调度，Wi-Fi 连接拿 IP，BLE 可被手机扫描并读写，FAT 文件系统跨重启持久，
硬件看门狗可触发真实复位，ULP-RISC-V 协处理器可被 RT-Thread 线程阻塞等待。**

---

## 这个仓库是什么

一个**只含移植代码**的仓库。不含 RT-Thread 源码，也不含 ESP-IDF 源码——两者按下面的方式引入。

```
esp32s3-rtthread/
├── rt-thread/                  ← overlay 到 RT-Thread 上游
│   ├── bsp/ESP/ESP32_S3/       BSP（板级：控制台、UART、GPIO、LEDC、I2C、SPI、ADC、SMP、临界区）
│   ├── libcpu/xtensa/          ★ 架构层（上下文切换、FPU 策略、原子操作、backtrace）
│   └── patches/
│       └── 0001-kernel-smp-hooks.patch     内核 4 处改动（SMP 必需）
├── esp-idf/                    ← overlay 到 ESP-IDF
│   ├── components/rtthread/    ★ RT-Thread 的 IDF 组件 + FreeRTOS API 兼容层
│   └── patches/
│       └── 0001-idf-core-smp-hooks.patch   16 个 IDF 文件的双核挂钩
├── project/s3-rtt-test/        示例工程：自检 + Wi-Fi + BLE + FS + WDT + ULP
├── tools/                      env / 构建 / QEMU / GDB / 应用补丁
└── docs/                       移植说明、教程、排障
```

**为什么架构层要单独拿出来：** `libcpu/xtensa/` 是新写的，RT-Thread 上游没有 Xtensa 支持。
它不是板级代码——上下文切换、窗口化 ABI、FPU 策略和任何一块 Xtensa 板子都相关。

**为什么用 patch 而不是直接改 IDF：** RT-Thread 要替换 FreeRTOS，必须动 IDF 内部的十几处。
做成 patch 才能审查「改了什么」和「升级 IDF 时哪些会冲突」。

---

## 快速开始

### 前置条件

- **ESP-IDF v6.1**（`CONFIG_IDF_RTOS_RTTHREAD` 是 fork 加的，见下）
- Xtensa 工具链：`xtensa-esp-elf-gcc` 15.2.0
- RISC-V 工具链：`riscv32-esp-elf-gcc` 15.2.0 —— **只有 ULP 需要**，但构建时会找不到就报错
- Python `pyserial`：`python -m pip install pyserial`

> **重要**：本移植需要 ESP-IDF 侧的 `CONFIG_IDF_RTOS_RTTHREAD` 等一系列开关，
> 这些来自一个 fork。见 [docs/BUILD.md](docs/BUILD.md) 的「准备 ESP-IDF」一节。

### 1. 拿代码

```bash
git clone https://github.com/yilives/esp32s3-rtthread.git
cd esp32s3-rtthread
```

### 2. 准备上游

**先把上游放到仓库的同级目录。** `tools/env.ps1` 按这个顺序找：

```
<你的目录>/
├── esp32s3-rtthread/     <- 刚克隆的
├── rt-thread/            <- 上游 RT-Thread
└── esp-idf-rtt/          <- IDF fork
```

找不到会报 `IDF_PATH is not set and no ESP-IDF checkout was found next to this
repository`。也可以直接设 `IDF_PATH` / `RTTHREAD_ROOT` 环境变量指向任意位置。

```powershell
# 路径按你的实际位置填
.\tools\fetch-upstream.ps1 -RtThreadPath ..\rt-thread -IdfPath ..\esp-idf-rtt
```

脚本做三件事：

1. 把 `rt-thread/bsp/ESP/ESP32_S3/` 和 `rt-thread/libcpu/xtensa/` 复制进你的 RT-Thread 树
2. 打 `rt-thread/patches/0001-kernel-smp-hooks.patch`（4 个内核文件）
3. 把 `esp-idf/components/rtthread/` 复制进 IDF，并打 IDF patch

> 手工做也可以，见 [docs/BUILD.md](docs/BUILD.md)。

### 3. 构建

```powershell
.\tools\build.ps1 -Project .\project\s3-rtt-test
```

> **这一步在干净目录里验证过。** 全新 `git clone` 到 `C:\s3clone`，上游放成同级
> 目录，只按本 README 走——构建成功，产出的 `s3_rtt_test.bin` 与开发机上的
> **字节数完全一致**（1113664 / 1129360，随配置而变，但同配置下相同）。
>
> 第一次构建可能打印几行 `fatal: no submodule mapping found ...`。那是 IDF 树里
> 的 git 残留，**不是构建失败**——构建仍然成功。见
> [docs/BUILD.md](docs/BUILD.md) 的「第一次构建的 git 噪音」。

### 4. 烧录

```powershell
python -m esptool --chip esp32s3 -p COM19 -b 921600 write-flash `
  --flash-mode dio --flash-freq 80m --flash-size 8MB `
  0x0  .\project\s3-rtt-test\build\bootloader\bootloader.bin `
  0x8000 .\project\s3-rtt-test\build\partition_table\partition-table.bin `
  0x10000 .\project\s3-rtt-test\build\s3_rtt_test.bin
```

### 5. 验证

```
msh >s3rt
s3rt v0.1 - ESP32-S3 / RT-Thread 5.4.0
  tick      : 35128, 1000 Hz
  current   : tshell at priority 20
  counters  : a=36 b=71
  heap kernel: 130976 total, 28816 used, 32640 peak
  commands  : s3rt tick, s3rt irq, s3rt irqlat, s3rt psram, s3rt led,
              s3rt pwm, s3rt key, list thread, free, s3heap

msh >s3smp
s3smp: CPUS_NR=2, this core=1
  cpu0: switches=103 current=tidle0 lock_nest=0 irq_nest=1
  cpu1: switches=335 current=tshell lock_nest=0 irq_nest=0
```

**或者让脚本自动核对全部输出**——`tools/check-docs.ps1` 会按顺序跑 50 个命令，
逐条比对文档里承诺的输出：

```powershell
# 只有板子：跑 47 节，跳过需要 WiFi 凭据的 3 节
.\tools\check-docs.ps1 -Port COM7

# 有 WiFi 环境：跑全部 50 节
.\tools\check-docs.ps1 -Port COM7 -WifiSsid <ssid> -WifiPass <pass>
```

端口改成你自己的。`-List` 会列出所有节以及哪些需要凭据。

`irq_nest` 是瞬时值：打印那一刻该核正在处理几层中断。撞上 tick 中断时就是 1，
这是正常的；持续大于 1 才是问题。`lock_nest` 必须为 0。

```
msh >s3heap
idf heap:
  internal 8-bit :    93512 free,    53248 largest,    93512 min ever
  internal 32-bit:    93512 free
  internal DMA   :    85724 free
  IRAM 8-bit     :        0 free
  PSRAM 8-bit    :  8386120 free,  8257536 largest
  total free     :  8479632
```

---

### 6. 没有硬件时：QEMU

```powershell
.\tools\build.ps1 -Project .\project\s3-rtt-test -NoPsram
.\tools\qemu.ps1   -Project .\project\s3-rtt-test -BuildDir build-qemu -TimeoutSec 90
```

**期望**：

```
RESULT: PASS - all 9 expectation(s) matched
```

九个断言包含双核（`CPUS_NR=2`）、文件系统（QEMU 模拟了 SPI flash，`s3fs` 能
挂载、写 511 字节、读回）和 `-NoPsram` 的明确报告。

GPIO 在 QEMU 里恒为 0，所以 `i2cscan` / `spiloop` / `s3adc` **不在门禁里** ——
它们会因为在模拟器上没有引脚而失败，那不是移植的缺陷。
```

`-NoPsram` 建到 `build-qemu/`（无 PSRAM、DIO flash），`build/` 留给硬件镜像。
两个变体的配置不同，别混用。

> **坑**：构建失败时 `qemu.ps1` 会静默跑上一次的镜像。每次确认
> `build-qemu/s3_rtt_test.bin` 的时间戳是新的。

### 7. 调试

QEMU 停在复位点等 GDB：

```powershell
.\tools\gdb-bt.ps1 -StartQemu          # 起 QEMU + attach + 打印双核 backtrace
```

或手动：

```powershell
.\tools\qemu-gdb.ps1                   # 冻结在复位点
# 另一个窗口
xtensa-esp32s3-elf-gdb .\project\s3-rtt-test\build-qemu\s3_rtt_test.elf
(gdb) target remote localhost:1234
```

**三条 GDB 命令在这个目标上会挂**，报错方式不像报错：

| 命令 | 结果 |
| --- | --- |
| `p rt_thread_self()->parent.name` | **挂死**——inferior function call 需要往目标注入代码，跑着的 QEMU 做不到 |
| `p rt_tick` | `No symbol`——SMP 下 tick 在 `struct rt_cpu` 里，每核一份 |
| `info threads` / `bt` / `p <全局变量>` | ✅ 正常 |

详见 [docs/BUILD.md](docs/BUILD.md)。

## 支持情况

### 内核

| 能力 | 状态 | 验证命令 |
| --- | --- | --- |
| 调度、线程、信号量、互斥量、事件、邮箱、消息队列 | ✅ | `s3rt` |
| 软件定时器、动态/静态对象 | ✅ | `s3rt` |
| **SMP 双核调度**（每核独立 tick、ISR 栈、IPI） | ✅ | `s3smp` |
| Xtensa 窗口化 ABI 上下文切换 | ✅ | `s3rt` |
| FPU（单精度协处理器 0） | ✅ | `s3rt` |
| 硬件原子操作（S32C1I） | ✅ | `s3rt` |
| FinSH / msh | ✅ | 默认 |

### 外设

| 外设 | 接口 | 验证命令 |
| --- | --- | --- |
| UART0 | `rt_serial` | 控制台 |
| GPIO / 引脚中断 | `rt_pin_ops` | `s3rt led` / `s3rt key` |
| I2C | `rt_i2c_bus` | `i2cscan` / `i2cprobe` |
| SPI | `rt_spi_bus` | `spiloop`（需 MOSI-MISO 短接）|
| ADC（SAR） | `rt_adc_device` | `s3adc <1\|2> <ch>` |
| LEDC PWM | `rt_device_pwm` | `s3rt pwm` |
| SYSTIMER | tick 源 | `s3rt tick` |
| PSRAM 8 MB octal | `heap_caps_malloc` | `s3rt psram` |
| IDF 堆分区 | `heap_caps_*` | `s3heap`（与 `free` 不同：后者是 RT-Thread 堆）|
| **TIMG 看门狗** | 直连 | `s3wdt` / `s3wdt hang` |
| **ULP-RISC-V 协处理器** | 直连 + 中断 | `s3ulp` / `s3ulpw` |

### 无线

| 能力 | 状态 | 验证命令 |
| --- | --- | --- |
| Wi-Fi STA 扫描 | ✅ | `s3wifiscan` |
| Wi-Fi STA 连接 + DHCP | ✅ | `s3wifi <ssid> <pass>` |
| Wi-Fi 断线重连 | ✅ | `s3wifi drop` |
| TCP + DNS | ✅（走 lwIP 原生 socket） | `s3net example.com` |
| BLE 控制器 + NimBLE host | ✅ | `s3ble` |
| BLE advertising | ✅ | 手机可扫到 `s3-rtt` |
| BLE GATT（读 + 写 + 通知） | ✅ | 写 `0xFF02` 控制 LED |
| 标准 Device Information `0x180A` | ✅ | `s3ble info` 报 handle |
| Wi-Fi 5 GHz | ❌ 硬件不支持 | — |

### 存储

| 能力 | 状态 | 验证命令 |
| --- | --- | --- |
| FAT 文件系统（`storage` 分区，1 MB） | ✅ | `s3fs` |
| 目录操作（列 / 读 / 删） | ✅ | `s3fs ls` / `cat` / `rm` |
| 跨重启持久 | ✅ | `s3fs read` |
| 掉电保护（信任引导签名） | ✅ | 见 `docs/FILESYSTEM.md` |
| 性能基准 | ✅ | `s3fsbench` |

### 未移植

I2S / SD-MMC / LCD / CAM / Touch / 温度传感器 / 以太网 / USB OTG / 深度睡眠 / 电源管理 / OTA。
见 [docs/ROADMAP.md](docs/ROADMAP.md)。

---

## 五个非显而易见的设计点

如果你只读一段，读这段。每一条都是**踩过坑之后**的结论，而不是事先的设计。

### 1. `rt_hw_interrupt_disable` 在 SMP 下是全局自旋锁

```c
/* include/rthw.h:146 */
#define rt_hw_interrupt_disable()   rt_cpus_lock()
```

IDF 的临界区如果用它，两个核会在 `_cpus_lock` 上互等，而其中一个核还关着中断——**死锁**。

所以 `board/critical.c` 用的是 `rt_hw_local_irq_disable()`。这是整个 SMP 移植里最后一个，
也是最难找的 bug：双核启动看起来像是"卡住"，实际上是两个核在争同一把锁。

### 2. flash 操作期间另一核必须完全静默

Xtensa 的 cache 和 flash 是共享的。一核写 flash 时要关自己的指令 cache，另一核如果还在
从 flash 取指，就是 `Cache error` 复位。

`board/board.c` 的 `spi_flash_other_core_stop/resume` 实现了一个握手：另一核在 tick 里
ack 并 park，同时用 `PS.INTLEVEL = 15` 屏蔽全部可屏蔽中断。

**为什么必须屏蔽全部中断**：IDF 用 `ESP_INTR_FLAG_IRAM` 注册的中断（BLE controller、
esp_timer alarm）在 cache 关闭时仍然会跑，而它们调用的是 flash 里的 shim 代码。
只屏蔽非 IRAM 中断不够——实测复现了四五次 `Cache error`。

这个握手还有三个必要条件，缺一个就死锁：
- **只有另一核 park**（`xt_rt_flash_cpu` 比较），做操作的核不能 park 自己
- **嵌套时不重复加锁**（depth 判断在 `rt_hw_spin_lock` 之前），`rt_hw_spin_lock` 不可重入
- **spinlock 跨核串行化**，防止两核同时做 flash 操作

### 3. 原子操作必须走硬件，且必须在 IRAM

Xtensa 的原子是 S32C1I（store-conditional），不是普通字上的 CAS。
软件回退实现用 `rt_hw_interrupt_disable()`——在 SMP 下就是上面那把全局锁，会递归。

`libcpu/xtensa/atomic_xtensa.c` 用 S32C1I 实现了全套 `rt_hw_atomic_*`，
并且**每个函数都带 `IRAM_ATTR`**：中断路径 `_rtt_int_enter → rt_interrupt_get_nest →
rt_atomic_load → rt_hw_atomic_load` 全程必须能在 cache 关闭时执行。

### 4. `main` 线程必须绑在 CPU0

不绑定的话，第一个空闲核会拿走它——在这个移植上通常是 CPU1。而 `main` 里跑的
`rt_components_init()` 会调 `nvs_flash_init()`，后者打开 flash 操作、要求另一核 park。
于是"另一核"成了 CPU0，而 CPU0 的 IPI 在这个移植上始终没打通：

```
IC i0=0 i1=27     ← core 1 的 IPI 工作，core 0 的从不触发
FL enter=1 sent=1 spins=17117 block=0
```

`src/components.c` 的补丁把它绑到 CPU0。

### 5. ULP 不是线程，但可以被线程等待

ULP 没有 TCB、没有优先级、不能调内核 API。它能做的是**发中断**。

所以"让 RT-Thread 调度 ULP"的实现是：ULP 完成工作 → `ulp_riscv_wakeup_main_processor()`
→ RTC ISR → 完成计数器 → 阻塞的线程观察到并恢复。

**ISR 里不能调 `rt_sem_release`**：它在 flash 里（`0x4200bc6c`），而 RTC ISR 注册时带了
`ESP_INTR_FLAG_IRAM`。这是一个潜伏的崩溃，不是"保守但可用"。所以 ISR 只递增计数器，
等待方每 tick 轮询。代价是最多 1 tick 延迟，实测 102–1053 µs。

---

## 已知限制

| 限制 | 原因 | 影响 |
| --- | --- | --- |
| **两个无线只能 BLE → Wi-Fi** | Wi-Fi → BLE 时 BLE controller 注册 `ETS_RWBT_NMI_SOURCE` 失败（`intc.c:222`，在预编译库里）。BLE → Wi-Fi 时 `s3wifi` 会先拆 BLE 栈再 init Wi-Fi | BLE → Wi-Fi 实测 5/5；Wi-Fi → BLE 不可用 |
| 长 flash 操作要先停无线 | 无线栈自己的 NVS 写与长擦除交错，IPC 握手不再完成；且 IRAM 中断在 cache 关闭时会调进 flash | `s3fs erase` / `s3fsbench` 已自动停无线，flash 窗口内两核都屏蔽全部中断 |
| **单核构建下 ULP 与 Wi-Fi 互斥** | Wi-Fi 初始化过之后（哪怕已 `stop`）ULP 中断到不了 RT-Thread | 需要 ULP 就必须双核；单核作为"无 ULP"降级配置 |
| ULP ISR 不释放信号量 | 上述 IRAM 契约 | 最多 1 tick 延迟 |
| 没有电源管理 | `CONFIG_PM_ENABLE` 关闭 | 功耗为 datasheet 最大值 |
| 没有 OTA | 分区表只有一个 app 槽 | 升级必须接线 |

详见 [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md)。

---

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/BUILD.md](docs/BUILD.md) | 从零搭建：上游准备、patch、构建、烧录 |
| [docs/PORTING.md](docs/PORTING.md) | 移植是怎么做的：架构层、SMP、中断、FreeRTOS shim |
| [docs/TUTORIAL.md](docs/TUTORIAL.md) | 手把手教程：跑通第一个程序，然后逐个验证功能 |
| [docs/FILESYSTEM.md](docs/FILESYSTEM.md) | FAT 移植、性能调优（实测数据）、掉电保护 |
| [docs/ULP.md](docs/ULP.md) | ULP-RISC-V：协议设计、为什么不能直接调度 |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | 症状 → 原因 → 修法 |
| [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md) | 已知限制与未移植项 |
| [docs/ROADMAP.md](docs/ROADMAP.md) | 下一步 |

---

## 硬件

在 **立创 ESP32S3R8N8** 上验证：

| 项 | 值 |
| --- | --- |
| 芯片 | ESP32-S3 (QFN56), rev v0.2 |
| 核心 | 双核 Xtensa LX7 @ 240 MHz |
| Flash | 8 MB (boya) |
| PSRAM | 8 MB octal @ 80 MHz |
| 晶振 | 40 MHz |
| MAC (Wi-Fi) | `3c:84:27:c7:4c:00` |
| MAC (BLE) | `3c:84:27:c7:4c:02` |
| 串口 | CH340K (UART0) + USB-Serial-JTAG |

其他 S3 板子应该也能跑，但引脚定义（LED = GPIO48、BOOT = GPIO0）需要按你的板子改
`bsp/ESP/ESP32_S3/board/`。

---

## 相关项目

- [yilives/esp32s31-rtthread](https://github.com/yilives/esp32s31-rtthread) —— 同一套方法移植到
  ESP32-S31（双核 RISC-V），那个移植的 SMP 完整度不如这个：S31 的 CPU1 启动未跑通
- [RT-Thread](https://github.com/RT-Thread/rt-thread)
- [ESP-IDF](https://github.com/espressif/esp-idf)

---

## License

Apache-2.0。见 [LICENSE](LICENSE)。

`rt-thread/bsp/` 和 `rt-thread/libcpu/` 下的代码遵循 RT-Thread 的 Apache-2.0；
`esp-idf/` 下的代码遵循 ESP-IDF 的 Apache-2.0。
