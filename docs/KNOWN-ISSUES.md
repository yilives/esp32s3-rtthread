# 已知限制

这份文档列的是**现在做不到的**，不是待办清单（那个是 [ROADMAP.md](ROADMAP.md)）。

---

## 1. Wi-Fi 在 BLE 之后连接：已用工作区规避（根因未定位）

### 现象

| 顺序 | 结果 |
| --- | --- |
| Wi-Fi → BLE | 4/4 成功 |
| BLE → Wi-Fi（原样） | 0/4 失败 |
| BLE → Wi-Fi（现在，自动处理） | **5/5 成功** |

失败时 `assoc` 会成功，然后在 **6 ms 内**被主动断开：

```
I wifi:state: assoc -> run (0x10)
I wifi:state: run -> init (0x100)
I wifi:Coexist: Wi-Fi connect fail, apply reconnect coex policy
W wifi:m f deauth reason:1
```

关联成功说明 SSID、密码、信道、射频链路全部正常。失败在之后的
**4-way 握手**。

### 这一轮新增的关键实验

上一轮排除了射频竞争、coex 偏好、时间不足、CPU 抢占。这一轮把范围缩到了
"NimBLE 加载时留下的状态"：

| 实验 | 结果 | 说明 |
| --- | --- | --- |
| `s3ble off`（只 disable controller，host 仍加载）→ 连 WiFi | **FAIL** | 不是 controller 在发射 |
| `s3ble deinit`（整个栈拆掉）→ 连 WiFi | **PASS 3/3** | 拆掉就好了 |
| `s3ble deinit` → 连 WiFi → `s3ble` 重开 | **PASS 3/3** | 重开不破坏链路 |

所以：**不是射频在竞争，也不是 BT controller 存在本身，而是 NimBLE host /
HCI 层加载时留下的某种状态。** 拆掉整个栈就恢复，重开也不影响已建立的
Wi-Fi 链路。

### 现在的行为

`s3wifi` 在 **`esp_wifi_init()` 之前**检查 BLE 是否在跑，在跑就先
`s3ble_shutdown_quiet()` 拆掉栈，连上之后用户可以再 `s3ble` 重开
（不影响已连的 Wi-Fi）。

> 这个检查原本排在 `esp_wifi_init()` **之后**，导致 `s3ble` → `s3wifi`
> 反而在 Wi-Fi 初始化时就 `ESP_ERR_NO_MEM` 失败。位置已修正，
> 实测 5/5，见第 12 条。



```
msh >s3ble
msh >s3wifi 你的SSID 你的密码
  BT up: unloading first     ok (run `s3ble` again afterwards)
s3wifi: PASS, connected ...
s3ble                      # 重开，不影响链路
```

实测 5/5 通过，`Guru 0`。

代价：BLE 会被断开一次。对"先起 BLE 调试、再连 Wi-Fi"的工作流可接受，
对"BLE 长期在线 + Wi-Fi 长期在线"不够——那个场景仍然只能用
Wi-Fi 先起的顺序。

### 根因还没找到

下一步应该看 NimBLE host 加载时改了什么全局状态：HCI 的 buffer 池、
`esp_coex` 的仲裁表项、还是 `esp_phy` 的 modem 标志（`phy_get_modem_flag()`）。
`esp_phy_enable()` 在 `s_is_phy_calibrated` 之后走的是 `phy_wakeup_init()`
分支而不是重新校准，这条路径在 BT 先启用时被走过一次。

---

## 2. 长时间 flash 操作要求无线栈先停（已自动化，含 IRAM 契约）

### 现象

三件事，同一根因：

1. Wi-Fi 关联后 `s3fs erase`（擦 1 MB）停在三四个 flash 窗口处永不返回
2. Wi-Fi + BLE 都开着时 `s3fsbench write` 5 秒后 `rst:0x7 (TG0WDT_SYS_RST)`
3. 修了 1 和 2 之后，偶发 `Guru: Cache error. Cache disabled but cached
   memory region accessed`，PC 落在 `rt_sem_trytake`

### 三个独立的缺陷

**缺陷 A：无线栈自己的 NVS 写与长擦除交错。**

`spi_flash_disable_interrupts_caches_and_other_cpu()` 每处理一个扇区都要
`vTaskSuspendAll()` + `esp_ipc_call_nonblocking(另一核)` + 忙等握手。Wi-Fi 从
自己的任务周期性写 NVS，每次都是一次这样的窗口。1 MB 擦除要 24 个窗口，
与无线的窗口交错后，某次握手再也没完成。

修法：`s3fs erase` 和 `s3fsbench` 自动停无线，做完再恢复。

**缺陷 B：两核的 IRAM 中断在 cache 关闭时都仍然会来。**

`esp_intr_noniram_disable()` 只屏蔽非 IRAM 中断，它的契约是"带
`ESP_INTR_FLAG_IRAM` 的 handler 继续跑，但它们只碰 IRAM"。**本移植违反了这个契约
的后半句**——IDF 的 IRAM handler 调进 FreeRTOS 兼容层，那是普通 flash 代码：

```
Guru Meditation Error: Core 0 panic'ed (Cache error).
Cache disabled but cached memory region accessed
PC 0x4200bcbe  rt_sem_trytake   (flash)
  <- ft_ipc_give_isr <- xQueueGiveFromISR
  <- semphr_give_from_isr_wrapper <- BLE controller
```

S3 的 flash cache 是**共享**的（`SOC_IDCACHE_PER_CORE` 未定义，只有 ESP32
有每核 cache），所以做 flash 的核一关 cache，另一核也没 cache 了。
这意味着**两边都必须聋**：

- 做 flash 的核：`spi_flash_disable_interrupts_caches_and_other_cpu()` 里
  关 cache 前 `rsil 15`，开 cache 后恢复 `PS`
- 被 park 的核：`spi_flash_op_block_func()` 里同样 `rsil 15`，
  等 `s_flash_op_complete` 之后、cache 恢复完再恢复 `PS`

只修一边时崩溃变稀有但没消失——这是第二次迭代才发现的，第一次只改了做 flash 的核。

把这些函数搬进 IRAM 是错的方向——它一路通到 `rt_sem_take`、
`rt_susp_list_dequeue`、`rt_thread_resume`，等于搬半个内核。**要关的是窗口，不是调用图。**

**缺陷 C：`board.c` 的 park 握手有两个 bug。**

虽然这条路径在 SMP 下不生效（见下），单核构建会走到：

- `xt_rt_flash_depth` 是全局变量但语义是每核的。两核同时做 flash 时第二个核
  会误判为"嵌套"直接返回，既不加锁也不 park 对方；resume 时计数错乱，
  第二个核**解开一把不属于自己的锁**，spinlock owner 损坏后永久死锁。
  已改成 `xt_rt_flash_depth[RT_CPUS_NR]`。
- ack 是"清零再等置位"，有丢失窗口。已改成单调 generation：
  doing core 递增 `xt_rt_flash_gen`，parked core 同步 `xt_rt_flash_ack_gen`。

### 实测数据

| 场景 | 修复前 | 修复后 |
| --- | --- | --- |
| `s3fs erase` + Wi-Fi 关联 | 静默卡死 | **PASS** |
| `s3fsbench write` + 双无线 | WDT 复位 | **PASS，4379 ms** |
| 擦除 + Wi-Fi 重连 soak | 3 次 1 次 Guru | **6/6 无 Guru** |
| `s3fsbench write` 无无线 | 1045 ms | 1045 ms（未变） |

**有无线时 4379 ms vs 无无线 1045 ms** —— 停无线本身有开销（`esp_wifi_stop`
要拆掉 802.11 状态），但能完成。

### 一个重要的认识

**`board.c` 里那套 park 握手在 SMP 构建下是死代码。** `cache_utils.c` 里
`spi_flash_other_core_stop()` 只在 `#else // CONFIG_FREERTOS_UNICORE` 分支被调用，
而 `CONFIG_FREERTOS_UNICORE` 没设，编的是 `#ifndef` 分支——那里用 IDF 自己的
`esp_ipc_call_nonblocking`。实测 `op=0 gen=0 ack_gen=0` 在所有 flash 操作后恒为 0，
证实了这一点。

所以 SMP 下实际依赖 IDF 的 IPC 机制；`board.c` 那套只在单核构建（`CPUS_NR=1`）
生效，那里它仍然必需，因为单核分支没有 IPC 可用。

### 还没覆盖的

格式化（而非擦除）同样要求无线先停，但 `s3fs` 的格式化走
`esp_vfs_fat_spiflash_format_rw_wl`，目前没有插 suspend 钩子。实际影响小，
因为格式化只在开机挂载失败时发生，那时无线还没起来。

---

## 2b. 单核构建能用，但 ULP + Wi-Fi 不能共存

### 构建

单核是支持的，需要三处同时改，缺一个都编不过或者跑不起来：

```
sdkconfig:      CONFIG_FREERTOS_UNICORE=y
                CONFIG_FREERTOS_NUMBER_OF_CORES=1
rtconfig.h:     /* #define RT_USING_SMP */     <- 必须注释掉，不能只改 CPUS_NR
                #define RT_CPUS_NR          1
```

**`RT_USING_SMP` 不会跟着 `RT_CPUS_NR` 自动关。** 只把 `RT_CPUS_NR` 改成 1
而留着 `RT_USING_SMP`，会编进 `scheduler_mp.c` 但 `struct rt_thread` 里没有
`cpus_lock_nest`（它由 `RT_USING_SMP` 控制），结果是 `cpu_mp.c` 编译失败。

还有一个坑：`components/rtthread/CMakeLists.txt` 在 **configure 时**读
`rtconfig.h` 决定编 `cpu_mp.c` 还是 `cpu_up.c`。只改 `rtconfig.h` 不会触发
reconfigure，会继续用缓存的选择。改完要 `idf.py reconfigure`：

```
-- rtthread: RT_USING_SMP=FALSE (from .../rtconfig.h)
```

### 能力差异（实测）

单核构建下：

| 场景 | 结果 |
| --- | --- |
| 启动 + `s3rt` 自检 | PASS |
| 文件系统挂载 / 读 / 基准 | PASS（基准还更快：868 ms vs 4379 ms） |
| Wi-Fi 连接 + TCP | PASS |
| BLE 广播 + GATT | PASS |
| **Wi-Fi 起来后 `s3ulp`** | **WDT 复位** |
| **`s3wifi stop` 之后 `s3ulp`** | **超时，no completion** |
| **Wi-Fi 从未初始化时 `s3ulp`** | **PASS，968 µs** |

也就是说：**ULP 和 Wi-Fi 在单核上互斥**，而且是单向的——Wi-Fi 初始化过
（哪怕已经 stop）之后，ULP 的中断就再也到不了 RT-Thread。

挂死时 `Saved PC: 0x4037eea9 wDev_ProcessFiq`，即 Wi-Fi 的 ISR 在处理。
单核下 ULP 的 RTC 中断和 Wi-Fi 的 ISR 在同一个核上竞争，而 ULP 的等待方
是按 tick 轮询的——Wi-Fi ISR 占满 CPU 时 tick 涨不起来，轮询永远看不到
`completions` 变化，最终被看门狗复位。

双核没有这个问题，因为 ULP 的等待方在 CPU1、Wi-Fi 在 CPU0。

### 结论

单核构建可以作为"没有 ULP 需求"的降级配置使用。需要 ULP 就必须双核。
这不是回归——`docs/` 从头到尾没说过单核支持 ULP。

---

## 3. ULP ISR 不释放信号量

### 现象

`s3ulp_kick()` 的延迟是 102–1053 µs（1 tick 轮询），不是几微秒。

### 原因

`rtc_module.c:100` 用 `ESP_INTR_FLAG_IRAM` 注册 RTC ISR，而 `rt_sem_release`
在 flash 里（`0x4200bc6c`）。在 ISR 里调它是本项目修过四次的同类 bug。

### 现在怎么办

ISR 只递增计数器，等待方每 tick 轮询。延迟是可测量的，命令会报出来。

### 修它需要什么

两条路，都需要实验验证：

1. 把 `rt_sem_release` 搬进 IRAM——它深入对象系统，不现实
2. 验证 RTC 中断触发时 flash cache 一定开着——理论上安全（RTC 中断不走 flash
   访问路径），但需要做压力测试：FAT 格式化期间高频触发 ULP 中断

---

## 4. 没有中断看门狗

### 现象

只有任务看门狗（`s3wdt`，TIMG MWDT），没有中断看门狗。

### 原因

`board.c` 的注释：

> `esp_int_wdt_init()` — the interrupt watchdog. **It is a genuine loss**
> (the S31 port starts it) but it needs the level-4/5 timer path wired up
> first; it goes in after the kernel boots, not before.

本移植只调度 level-1 中断。中断看门狗需要 level-4/5，那条路径还没接。

### 影响

一个卡在关中断状态的中断 handler 不会被检测到。任务看门狗只检测"喂狗线程不跑"。

### 修它需要什么

把 Xtensa 的 level-4/5 定时器中断接进 `_xt_interrupt_table`，
并实现对应的 handler。

---

## 5. 没有电源管理

### 现象

功耗是 datasheet 最大值。`CONFIG_PM_ENABLE is not set`。

### 原因

RT-Thread 的 PM 框架和 IDF 的 `esp_pm` 是两套。SMP 下还要考虑两核的空闲协同。

### 修它需要什么

先决定用哪一套。用 IDF 的 `esp_pm` 需要它和 RT-Thread 的 idle 线程协同；
用 RT-Thread 的 PM 需要重写 `esp_pm_*` 的调用点。两条都不小。

---

## 6. 没有 OTA

### 现象

分区表只有一个 `factory` 槽：

```
factory,  app,  factory,  0x10000,  0x300000
```

升级必须接线烧录。

### 修它需要什么

改分区表加 `ota_0` / `ota_1`（应用目前 3 MB，两块需要 6 MB，8 MB flash 够但紧张），
接 `esp_ota_begin/write/end`，加回滚检查。

---

## 7. 没有文件系统之外的用户存储

### 现象

只有 `/storage`（1 MB FAT）。没有：

- `stdout` / `stdin` 的设备节点（`RT_USING_DFS` 关闭）
- ymodem 传文件
- `/dev` 目录

### 原因

`RT_USING_DFS` 关闭，用的是 IDF 的 VFS。

### 影响

`ls` / `cd` / `cat` 这些 RT-Thread 的 shell 命令不存在。文件操作用标准的
`fopen` / `fread` / `fwrite`（POSIX 风格，走 IDF VFS）。

---

## 8. socket 走 lwIP 原生，不走 RT-Thread SAL

### 现象

- `ifconfig`、`ping`、`netstat` 命令**不存在**
- 代码里必须 `#include "lwip/sockets.h"`，不能写 `#include <sys/socket.h>`

### 原因

`RT_USING_SAL` / `RT_USING_SOCKET` / `RT_USING_NETDEV` 都关闭。

### 影响

RT-Thread 生态里的网络组件（`webclient`、`mqtt`、`netutils`）用不了。

### 修它需要什么

把 RT-Thread 的 SAL 接到 lwIP 上，提供一个 `sal_proto_family` 注册。

---

## 9. 没有 POSIX / pthread

### 现象

`RT_USING_POSIX` / `RT_USING_PTHREADS` 关闭。

### 影响

`pthread_create`、`select`、`poll` 这些用不了。移植 Linux 代码会卡。

### 注意

`RT_USING_LIBC` 也是关的——用的是 IDF 的 `esp_libc` + newlib。
两者同时开会冲突，这是当初有意避开的。

---

## 10. 单核路径的 ULP + Wi-Fi 挂死

原来的记录是"单核偶发 Guru，未定位"。现在定位了，见 [2b](#2b-单核构建能用但-ulp--wi-fi-不能共存)：
不是偶发，是**必现**——Wi-Fi 一初始化过，ULP 的中断就再也到不了 RT-Thread，
表现是被看门狗复位（Wi-Fi 在跑）或轮询超时（Wi-Fi 已 stop）。

双核不受影响。

---

## 11. I2C 把超时和仲裁丢失误判成 ACK（已修）

### 现象

裸板上 `i2cscan` 报若干 `flaky` 地址，每次数量和地址都不同：

```
i2cscan: 0 device(s) on i2c0, 7 flaky, 0 timeout(s)
i2cscan: 0 device(s) on i2c0, 9 flaky, 0 timeout(s)
i2cscan: 0 device(s) on i2c0, 13 flaky, 0 timeout(s)
```

`flaky` 的定义是"两次探测中 ACK 了一次"，而板上没有任何 I2C 设备。

### 根因

`rt_s3_i2c_xfer_msg()` 的等待循环在三个错误位中的**任意一个**置位时 `break`：

```c
if (hw->int_raw.nack_int_raw || hw->int_raw.time_out_int_raw ||
    hw->int_raw.arbitration_lost_int_raw)
{
    break;
}
```

但循环之后判断成功与否时，只读了 `nack_int_raw`：

```c
nack = (hw->int_raw.nack_int_raw != 0);   /* 只看了一个 */
```

于是由**超时或仲裁丢失**引起的 break，`nack` 为 0，函数返回 `RT_EOK`，
调用方把它当成 ACK。

新增的 `i2cprobe` 命令证实了这一点——同一个地址连续探测：

```
i2cprobe: 0x50 x8 on i2c0
  [ 0] rc=-8 NACK
  [ 1] rc=1 ACK        <- 同一个地址，同一份代码
  [ 2] rc=-8 NACK
  [ 4] rc=1 ACK
  ...
```

真设备每次都会 ACK；随机 ACK 只能是错误标志被忽略。

### 修法

三个错误位都算失败，且在 `i2c_ll_master_fsm_rst()` 清掉它们**之前**读取
（那个函数会重新初始化寄存器块，见函数开头的注释）：

```c
nack = (hw->int_raw.nack_int_raw != 0) ||
       (hw->int_raw.time_out_int_raw != 0) ||
       (hw->int_raw.arbitration_lost_int_raw != 0);
```

### 验证

| | 修复前 | 修复后 |
| --- | --- | --- |
| `i2cprobe 0x50 8` | 8 次里 2 次 ACK | **8/8 NACK** |
| `i2cprobe 0x08 8` | 8/8 NACK | 8/8 NACK |
| `i2cscan` 三次 | 7 / 9 / 13 flaky | **0 / 0 / 0** |

### 顺带

文档里写的 `s3rt i2c` / `s3rt spi` / `s3rt adc` 从来不存在，实际命令是
`i2cscan` / `spiloop` / `s3adc`。已更正 README 和 TUTORIAL。

---

## 12. 两个无线只能按 BLE → Wi-Fi 的顺序起（Wi-Fi → BLE 仍失败）

### 可用顺序

```
msh >s3ble                    # 1. 先起 BLE
msh >s3wifi <ssid> <pass>     # 2. 连 Wi-Fi（会自动拆掉 BLE）
msh >s3net example.com        # 3.
```

**实测 5/5 通过**（`advertising as "s3-rtt"` → `unloading first` → `s3wifi: PASS`
→ `s3net: PASS`，无 Guru、无 NO_MEM）。

### 不可用顺序

```
msh >s3wifi <ssid> <pass>
msh >s3ble                    # 失败
```

```
BLE assert intc.c 222, param 00000008      # = ETS_RWBT_NMI_SOURCE
```

### 本轮修掉的那个 bug

`s3wifi` 里拆 BLE 的那段代码**位置错了**：它排在 `esp_wifi_init()` 之后。

```
行 444: esp_wifi_init()        <- 内存不足，失败
行 511: s3_wifi_ble_is_up()    <- 之后才拆，太晚
```

Wi-Fi 驱动要一次性分配 10 个 1600 字节的静态 RX buffer，BLE 还占着内部 DRAM：

```
W wifi:malloc buffer fail
E wifi:Expected to init 10 rx buffer, actual is 9
esp_wifi_init  FAILED 0x101 (ESP_ERR_NO_MEM)
```

拆 BLE 对已经失败的分配没有帮助。移到 `esp_wifi_init()` **之前**后，这条路
从 0/5 变成 5/5。

### Wi-Fi → BLE 为什么还不行（未解决）

`param 00000008` 是 `ETS_RWBT_NMI_SOURCE` —— BLE controller 初始化时要注册
RWBT 的 NMI 中断，而此时 Wi-Fi 已经占用了中断控制器里的资源。断言在预编译的
`libbtdm_app.a` 里（`intc.c:222`），改不动。

### 试过但无效的方案

| 尝试 | 结果 |
| --- | --- |
| `BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`（PSRAM） | 4/6，仍会 `intc.c:222`，且出现过 TLSF 堆损坏的 Guru |
| WiFi 静态 RX 10→6、动态 32→16 | 5/6，重复测 4/8 —— 波动太大，不算修好 |
| WiFi 静态 RX 6→4、动态 16→8 | 2/6，更差 |
| `linker.lf` 把 `ipc`/`interrupt` 搬进 IRAM | 与这个问题无关（但见下） |

**都已回退。** 现在的配置是默认值 + BLE 先起。

### 一个仍然有效的独立改进

`linker.lf` 里的 `ipc (noflash)` / `object (noflash)` / `interrupt (noflash)`
把 ISR 路径搬进了 IRAM，与无线顺序问题无关，保留。

---

## 汇总表## 汇总表## 汇总表

| # | 限制 | 严重度 | 可行的修法 |
| --- | --- | --- | --- |
| 1 | Wi-Fi 必须先于 BLE | 中 | 让 NimBLE 等 coex 就绪 |
| 2 | 长 flash 操作要先停无线 | 中 | 已自动化：`s3fs erase` / `s3fsbench` 停无线，flash 窗口两核都屏蔽全部中断 |
| 3 | ULP ISR 不释放信号量 | 低 | 验证 RTC 中断时的 cache 状态 |
| 4 | 无中断看门狗 | 中 | 接 level-4/5 定时器 |
| 5 | 无电源管理 | 高（电池场景） | 选定一套 PM 框架 |
| 6 | 无 OTA | 高（远程设备） | 双分区 + 回滚 |
| 7 | 无 DFS 设备节点 | 低 | 开 `RT_USING_DFS` |
| 8 | 无 SAL | 中 | SAL → lwIP 适配 |
| 9 | 无 POSIX | 中 | 开 `RT_USING_POSIX`，解决与 esp_libc 的冲突 |
| 2b | 单核下 ULP 与 Wi-Fi 互斥 | 中 | 需要 ULP 就用双核；这是单核的固有限制 |
| 10 | 单核 ULP+Wi-Fi 挂死 | 中 | 同 2b，已定位，非偶发 |
| 11 | I2C 把超时/仲裁误判为 ACK | 中 | 已修：三个错误位都算失败 |
| 12 | 两个无线只能 BLE→Wi-Fi | 中 | BLE→Wi-Fi 已 5/5；Wi-Fi→BLE 仍 `intc.c:222`，在预编译库里改不动 |
