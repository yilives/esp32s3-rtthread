# 排障

症状 → 原因 → 修法。

---

## 启动

### 串口没输出 / 输出乱码

| 检查 | 期望 |
| --- | --- |
| 波特率 | 115200 |
| 引脚 | UART0 = GPIO43(TX) / GPIO44(RX) |
| `dtr` / `rts` | 在 `open()` **之前**设为 `False` |
| 复位方式 | `rts=True; sleep(0.2); rts=False`（不是 `dtr`） |

某些 CH340 在 `dtr=True` 时会拉芯片进 bootloader，表现就是只有 `ESP-ROM:` 一行。

### 卡在 `cpu_start: Multicore app` 之后

**原因**：第二核启动失败或 `main` 线程调度出问题。

**看**：是否有 `Guru Meditation`。有的话看 PC 地址：

```powershell
xtensa-esp32s3-elf-addr2line -pfia -e build\s3_rtt_test.elf <PC>
```

**常见**：

| PC 位置 | 原因 |
| --- | --- |
| `rt_sched_tick_increase` | tick 在调度器启动前触发（内核补丁没打） |
| `rt_hw_atomic_load` | 原子操作在 flash 里（缺 `IRAM_ATTR`） |
| `rt_sem_trytake` | flash 与 ISR 的 IRAM 契约冲突 |

### 重启循环（每几秒重启）

**先看 `rst:` 后面的原因**：

| 值 | 含义 |
| --- | --- |
| `rst:0x1 (POWERON)` | 正常上电 |
| `rst:0x7 (TG0WDT_SYS_RST)` | **我们的硬件看门狗触发的** |
| `rst:0x10 (RTCWDT_RTC_RST)` | 中断看门狗或 panic 后复位 |
| `rst:0xc (RTC_SW_CPU_RST)` | 软件复位（panic 后 `esp_restart`） |

`TG0WDT_SYS_RST` 且你不想要它：

- 喂狗周期是 1250 ms，超时 5000 ms
- 如果某个操作阻塞调度器超过 5 秒（比如格式化 1 MB），需要 `s3wdt pause`
- 实测过的场景：格式化 FAT 时调度器被 `rt_enter_critical()` 挂起，
  喂狗线程跑不了

---

## SMP

### `s3smp` 显示 `CPUS_NR=1`

构建成了单核。检查：

```
rtthread/bsp/ESP/ESP32_S3/rtconfig.h:
  #define RT_USING_SMP
  #define RT_CPUS_NR  2

sdkconfig:
  CONFIG_FREERTOS_NUMBER_OF_CORES=2
  # CONFIG_FREERTOS_UNICORE is not set
  # CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE is not set
```

**四处必须一致**，缺一个就降级成单核或链接失败。

### `lock_nest` 或 `irq_nest` 不为 0

**硬故障**。要么有临界区没退出，要么有中断没返回。

**最可能的原因**：某个 IDF 临界区映射到了 `rt_hw_interrupt_disable()`
（SMP 下是全局 spinlock）。检查 `board/critical.c` 是否用的是
`rt_hw_local_irq_disable()`。

### 双核启动后"卡住"，无 panic 无复位

**最典型的症状**，通常是三个原因之一：

1. **`critical.c` 用了 `rt_hw_interrupt_disable()`** → 两核争全局锁，都关着中断
2. **flash park 握手缺条件** → 见 [PORTING.md](PORTING.md) §2.5 的四个必要条件
3. **`main` 绑到了 CPU1** → CPU0 的 IPI 不通，`nvs_flash_init()` 挂死

诊断：在可疑处加 `ets_printf()`（ROM 输出，IRAM，cache 关闭也能用）。
`rt_kprintf` 在 cache 关闭时会崩。

### `Guru ... Cache error` + PC 落在 `rt_*` / `ft_*` / `xQueue*`

**flash 与中断的 IRAM 契约冲突。**

修复清单（按重要性）：

1. `board/critical.c` 的 `xPortInIsrContext` / `xPortInterruptedFromISRContext` 加 `IRAM_ATTR`
2. `libcpu/xtensa/atomic_xtensa.c` 全部函数加 `IRAM_ATTR`
3. `ft_core.h` 的 `FT_IRAM` 标注 ISR 可达路径
4. `port_systick.c` 的 park 块用 `PS.INTLEVEL = 15` 屏蔽全部中断

---

## 无线

### `s3wifi` 报 `deauth reason=1`，连不上

**检查启动顺序**：

| 顺序 | 结果 |
| --- | --- |
| Wi-Fi → BLE | ✅ 4/4 |
| BLE → Wi-Fi | ❌ 0/4 |

**必须 Wi-Fi 先起。** 这是已知限制，见 [KNOWN-ISSUES.md](KNOWN-ISSUES.md)。

### `s3wifi` 报 `FAIL, no WIFI_EVENT_STA_START`

`WIFI_EVENT_STA_START` 每个 `esp_wifi_init` 只投递一次。如果之前跑过
`s3wifiscan`，这个事件已经用掉了。

代码已经处理（`s_wifi_started_once` 标志），如果还报错说明状态判断有漏。

### `s3net` 报 `thread_sem_init: unable to create semaphore`

**RT-Thread 堆不够。** lwIP 给每个线程建 per-thread 信号量，堆耗尽时失败。

检查：

```
msh >free
total    : 130976
used     : 63344
available: 67632     ← 这个数不能太小
```

Wi-Fi + BLE 起来后至少要有 **几 KB**。不够就调大
`CONFIG_IDF_RTTHREAD_HEAP_SIZE`（上限约 128 KB，再大 DRAM 放不下）。

### `s3net` 报 `assert failed: tcpip_send_msg_wait_sem (Invalid mbox)`

和上一条同源——堆耗尽导致 lwIP 的 mbox 创建失败。

### BLE 启动后 `s3ble: FAIL, host never synced`

**检查 `nimble_port_freertos_init()` 的参数**。它接受的是**函数指针**，不是参数：

```c
/* 正确 */
nimble_port_freertos_init(s3_ble_host_task);

/* 错误：传 NULL 会创建失败，host 线程不存在 */
nimble_port_freertos_init(NULL);
```

### 手机搜不到 `s3-rtt`

1. `s3ble info` 看 `advertising=1`
2. 如果 `advertising=0`，执行 `s3ble adv`
3. advertising 是从 `sync_cb` 里启动的，不是从命令里——检查 `s3_ble_on_sync` 是否被调用

---

## 文件系统

### `fopen` 返回 `EACCES`（errno 13）

**FAT 半写。** 通常是被中断的格式化留下的。

```
msh >s3fs unmount
msh >s3fs erase
（复位板子）
```

**预防**：不要让格式化被看门狗打断。`s3fs` 内部已经包了 `s3wdt_pause()`。

### `mount failed 0x105`

`ESP_ERR_NOT_FOUND` —— 分区表里没有 `storage`。

检查分区表是否烧录：

```
python -m esptool --chip esp32s3 -p COM19 read-flash 0x8000 0xC00 partition.bin
python esp-idf-rtt/components/partition_table/gen_esp32part.py partition.bin
```

### 每次开机都 `Formatting FATFS partition`

卷一直挂不上。**先看上一次是不是被中断了**。

如果反复发生，`s3fs erase` 后重启。

### 写小文件特别慢（100+ ms 一个）

**这是正常的**，见 [FILESYSTEM.md](FILESYSTEM.md)。4 KB 扇区 + IDF 的
`diskio_wl` 每次写前必擦，一个 64 字节文件消耗约 21 KB flash。

---

## 构建

### `xtensa_context.h: No such file or directory`

BSP 的 scons 脚本找不到 IDF 头文件。设：

```powershell
$env:RTT_IDF_PATH = "<IDF_PATH>"
```

### `riscv32-esp-elf-gcc ... not found`

ULP 的嵌套构建找不到 RISC-V 工具链。检查 `build.ps1` 的 `$pathParts` 是否包含：

```powershell
"$tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin",
```

### `ulp_riscv.h: No such file or directory`

`ulp` 在 `PRIV_REQUIRES` 里。改成 `REQUIRES`（见 [ULP.md](ULP.md)）。

### `region 'dram0_0_seg' overflowed`

RT-Thread 堆太大。`CONFIG_IDF_RTTHREAD_HEAP_SIZE` 上限约 **128 KB**
（160 KB 会 overflow 10672 字节）。

### `configNUMBER_OF_CORES must be defined`

`sdkconfig.h` shim 缺宏。删 `sdkconfig` 重新生成，或者补上
`CONFIG_FREERTOS_NUMBER_OF_CORES`。

### 改了 `sdkconfig.defaults` 但不生效

**`sdkconfig` 是持久文件，`defaults` 只影响首次生成。**

要改现有配置，直接改 `sdkconfig`，或者删掉它重新生成。

---

## QEMU

### `qemu.ps1` 报 PASS 但明显不对

**`qemu.ps1` 在构建失败时会静默跑上一次的镜像。**

每次都确认时间戳：

```powershell
Get-ChildItem project\s3-rtt-test\build\s3_rtt_test.elf | Select-Object LastWriteTime
```

### QEMU 里 PSRAM 相关命令失败

QEMU 不模拟 PSRAM。用 `-NoPsram` 构建，它会应用 `sdkconfig.nopsram`。

---

## 调试手段

### 定位崩溃地址

```powershell
$elf = "project\s3-rtt-test\build\s3_rtt_test.elf"
$a2l = "$env:USERPROFILE\.espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin\xtensa-esp32s3-elf-addr2line.exe"

& $a2l -pfia -e $elf 0x4204a122 0x4037d5ac
```

### 判断函数在 IRAM 还是 flash

```powershell
$nm = "...\xtensa-esp32s3-elf-nm.exe"
& $nm $elf | Select-String " T rt_sem_release$"
```

- `0x4037xxxx` / `0x4038xxxx` = **IRAM**（cache 关闭也能跑）
- `0x4200xxxx` = **flash**（cache 关闭时崩溃）

### cache 关闭时的输出

`rt_kprintf` 在 flash 里，cache 关闭时会崩。用 ROM 的 `ets_printf`：

```c
extern int ets_printf(const char *fmt, ...);
ets_printf("[probe] core=%d\n", (int)rt_hw_cpu_id());
```

它在 IRAM 里，可以在任何上下文中调用。

### 看线程状态

```
msh >list thread
```

关键字段：

| 字段 | 含义 |
| --- | --- |
| `suspend` / `ready` / `running` | 线程状态 |
| `EINTRPT` | 被中断唤醒（正常） |
| `ETIMEO` | 超时 |
| `lock_nest` / `irq_nest` | 必须为 0 |

### 看内存

```
msh >free
```

`available` 太小会导致各种奇怪的分配失败。
