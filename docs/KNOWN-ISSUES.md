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

**实测 6/6，Guru 0**：BLE 广播成功 6/6，Wi-Fi 连接成功 6/6
（`advertising as "s3-rtt"` → `unloading first` → `s3wifi: PASS`）。

> **一次更正。** 这个数字先前记作"6/6，但其中 1 次出现 Guru"。那是本地工作树
> 的状态，不是这个仓库的状态：`components/lwip/lwip/src/api/tcpip.c`、
> `components/lwip/port/freertos/sys_arch.c` 和
> `components/esp_netif/lwip/esp_netif_lwip.c` 里各有一段未提交的调试代码
> （RTC_NOINIT 计数器和诊断打印，引用 `c5tcp` 测试）。清掉之后同一测试 6/6、
> Guru 0。这些文件**从未进入本仓库的 patch** —— patch 只含 17 个文件，
> 不含 lwip 和 esp_netif。

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

### Wi-Fi → BLE 为什么还不行（未解决，但量化了）

新增的 `s3heap` 命令（IDF 堆，不是 `free` 那个 RT-Thread 堆）给出了答案：

```
                     内部 DRAM 空闲    最大连续块
开机              :      93512           53248
Wi-Fi 起来后      :      59364           31744
s3ble 尝试之后    :       2568            2304
```

BLE controller 一次性要 **> 56 KB 的连续内部 DRAM**。Wi-Fi 占掉后只剩
31 KB 的最大块，不够。

清理掉本地调试残留之后，4 次测试全部是 `Malloc failed`，形态统一了。
之前见过的两次 `BLE assert intc.c 222, param 00000008` 没有再出现——
那很可能也是残留代码改变了内存布局的后果，但样本太少，不下结论。

| 现象 | 出现情况 |
| --- | --- |
| `BLE_INIT: Malloc failed` / `rc=519` | 干净树上 4/4，确定性 |
| `BLE assert intc.c 222, param 00000008` | 只在有残留的树上出现过 |

**这是内存问题，不是中断分配器的问题。** 我一开始以为是后者（`board/interrupt.c`
拒绝高于 `XCHAL_EXCM_LEVEL` 的线），但那个拒绝会打印
`intr: line N needs an assembly handler` —— 实测从未出现。

### 试过但无效的（继续记录，避免重复）

| 尝试 | 结果 |
| --- | --- |
| `BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`（PSRAM） | 4/6，仍会失败 |
| WiFi 静态 RX 10→6、动态 32→16 | 5/6，重复测 4/8 |
| WiFi 静态 RX 6→4、动态 16→8 | 2/6，更差 |
| NimBLE 连接数 3→1、MTU 256→128、事件队列 30→12 | **0/5，更差** —— 这些参数不控制 controller 的 buffer |
| `BT_CTRL_BLE_MAX_ACT` 6→2 | 腾出 3.7 KB（`afterble` 从 2568/2304 变成 6240/6144），**仍 `Malloc failed`** |

### 下一步

BLE controller 的 buffer 不是在 NimBLE 的 Kconfig 里配的，而是在预编译的
`libbtdm_app.a` 里按 `CONFIG_BT_CTRL_BLE_MAX_ACT` 等参数静态分配的。真正能减的是：

1. ~~`CONFIG_BT_CTRL_BLE_MAX_ACT` 6 → 2~~ —— 已试，只腾出 3.7 KB，不够
2. 查清 BLE controller 那 56 KB 到底花在哪。`libbtdm_app.a` 里的分配是按
   `BT_CTRL_BLE_MAX_ACT` 算的，但 6→2 只省 3.7 KB，说明大头不在 activity 上，
   而在固定的 controller 数据结构
3. 反过来压 Wi-Fi：31 KB 的最大块是碎片问题，不是总量问题（还有 59 KB 空闲）。
   如果能减少 Wi-Fi 的中等块分配，最大块可能变大
4. 或者接受这个限制。两个无线同时在线对这块板子本来就紧张 ——
   内部 DRAM 只有 512 KB，Wi-Fi + BLE + RT-Thread + 应用要一起分

前两条都需要读 `libbtdm_app.a` 的分配行为，那是预编译的，只能靠实测推断。

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

## 13. `s3net` 偶发 `errno=113`（ECONNABORTED）

### 现象

```
msh >s3net example.com
s3net: resolving example.com ...
  gethostbyname          ok -> 172.66.147.243
  socket                 ok (56)
  connect                FAILED errno=113 (Software caused connection abort)
s3net: command failed -1.
```

DNS 和 socket 都成功，`connect` 被中止。

### 复现情况

| 场景 | 结果 |
| --- | --- |
| `s3wifiscan` → `s3ble` → `s3wifi` → `s3ble` → `s3net` | 出现过 1 次 |
| `s3ble` → `s3wifi` → `s3net`，连续 4 次 | **4/4 PASS** |

只在那次较长的序列里见过一次。之后 4 次直接测试全部通过。

### 状态

**未定位，样本不足。** 记在这里是因为它出现在一次真实的运行里，而不是因为
已经知道原因。下次复现时应该抓：

1. `connect` 返回前是否有 `WIFI_EVENT_STA_DISCONNECTED`
2. 是否与 `s3ble` 重新初始化（`nimble_port_freertos_init` 会重建 host 任务）
   有关 —— 那次失败前刚做过一次 `s3ble`

`ECONNABORTED` 在 lwIP 里常见于连接建立过程中底层接口被拆除。

---

## 14. `s3wifiscan` 的两处 netif 顺序错误（已修）

`s3wifiscan` 是唯一一个最近几轮没有实际跑过的命令。为文档输出写检查脚本时，
它连续暴露了两个崩溃。

### 崩溃 1：netif 建在 `esp_wifi_init()` 之前

冷启动直接跑 `s3wifiscan`，每次复位：

```
ESP_ERROR_CHECK failed: esp_err_t 0x103 (ESP_ERR_INVALID_STATE)
func: esp_netif_create_default_wifi_sta
expression: esp_wifi_set_default_wifi_sta_handlers()
```

`esp_netif_create_default_wifi_sta()` 内部要调
`esp_wifi_set_default_wifi_sta_handlers()`，后者要求 Wi-Fi 驱动已初始化。
而代码把它放在了 `esp_wifi_init()` 之前——注释还写着"和 `s3wifi` 一样的顺序"，
实际 `s3wifi` 是 init 之后才建 netif。

对照实验：冷启动 `s3wifi` 0 次 `ESP_ERROR_CHECK`、PASS；`s3wifiscan` 每次崩。

### 崩溃 2：netif 重复创建

修好 1 之后，`s3wifi` 之后再 `s3wifiscan` 又崩：

```
E esp_netif_lwip: esp_netif_new_api: Failed to configure netif
assert failed: esp_netif_create_default_wifi_sta wifi_default.c:437 (netif)
```

`esp_netif_create_default_wifi_sta()` **不能调两次**。改成先查
`esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")`，不存在才创建。

### 验证

| 场景 | 修复前 | 修复后 |
| --- | --- | --- |
| 冷启动 `s3wifiscan` | 每次复位 | PASS，扫到 22 个 AP |
| `s3wifi` → `s3wifiscan` | 复位 | PASS |
| 紧接 `s3net` | `tcpip.c:454 Invalid mbox` 复位 | PASS，512 bytes |

### 怎么发现的

不是靠手工测试，是靠**给文档输出写检查脚本**（`tools/check-docs.ps1`）。
第一次跑就崩了。这两个 bug 存在了好几轮，因为 `s3wifiscan` 一直不在回归清单里。

---

## 15. `s3fs erase` 之后无法挂载（已修）

### 现象

```
msh >s3fs unmount
msh >s3fs erase
s3fs: erased; next `s3fs` will format and mount

msh >s3fs
E vfs_fat_spiflash: f_mount failed with error: "No filesystem detected" [13]
s3fs: mount failed 0xffffffff (ESP_FAIL)
s3fs: is the partition table flashed? `storage` starts at 0x310000 and is 1 MB
```

**第二次 `s3fs` 也一样失败。** 只有重启才能恢复，因为 `s_fs_mounted_once`
是静态变量，重启才会清零。

也就是说 `erase` 打印的那句"next `s3fs` will format and mount"**是假的**。

### 根因

`s3_fs_mount()` 里：

```c
.format_if_mount_failed = !s_fs_mounted_once,
```

这个标志是有意加的：`format_if_mount_failed = true` 会在任何挂载失败时
静默格式化，把用户数据擦掉（见第 2 条）。但 `erase` 是**主动**擦除，
之后本该允许格式化——标志没复位，格式化就永远被禁止。

### 修法

`erase` 成功后重置 `s_fs_mounted_once = RT_FALSE`。

### 验证

```
s3fs unmount
s3fs erase
s3fs
  W vfs_fat_spiflash: FatFS mount failed [13]
  I vfs_fat_spiflash: Formatting FATFS partition, allocation size=4096
  I vfs_fat_spiflash: Mounting again
  s3fs: PASS, mounted at /storage, wrote and read back 511 bytes
```

### 顺带：`s3fs format` 现在也 suspend Wi-Fi

`erase` 会先停掉 Wi-Fi 再擦（第 2 条），`format` 没有——两个做同类事情的
命令，一个有保护一个没有，而且没有注释解释原因。

实测 Wi-Fi 关联时 `format` 3/3 成功，所以这**不是**在修一个观察到的故障，
而是补上同一个窗口：站点从自己的任务里写 NVS，和长时间 flash 操作不能共存。
`erase` 曾经在第四个窗口静默挂死；`format` 做的工作少、目前侥幸躲过，
这不等于安全。

---

## 16. 文档里的性能数字需要注明前提（已修三处）

性能数字和命令输出一样会过时，而且更难察觉——一个"152 writes"看起来
完全合理，直到有人在新机器上测出 190。

这一轮核对了 `s3fsbench`、`s3ulp`、`s3heap` 的数字，三处都需要修正。

### 1. `s3fsbench` 的数字取决于卷的历史

| 卷状态 | writes / erases | create 32 小文件 |
| --- | --- | --- |
| 全新格式化 | **152 / 136** | 5281–7379 ms |
| 用过一段时间 | **190 / 170** | 6989–8913 ms |

差 25%。目录项分散 → 新建文件要更新更多 FAT 扇区 → 每次 FAT 扇区写都是一次擦除。

`FILESYSTEM.md` 写的是 152/136，那是新卷的数字，但**没说前提**。
现在说清了，`s3fsbench` 自己也会打印一行提示。

### 2. 计数确定，时间不确定

四次全新卷测量：

| 指标 | 四次结果 |
| --- | --- |
| writes / erases | 152 / 136（完全相同）|
| create 32 小文件 | 5374 / 5458 / 7379 / 5281 ms |

写次数由算法决定；时间由 SPI NOR 物理擦除决定，同一台设备上抖动 40%。
**看趋势用计数，看绝对性能才用时间。**

### 3. ULP 延迟：一次测量被写成了区间

`ULP.md` 原文写"实测 102–1053 µs"。十次采样：

```
520  605  789  882  939  967  1173  1238  1339  1713   (µs)
```

8 次超出 1053。原文那个"区间"其实是一次测量的结果。现在给出十次样本和
中位数（约 950 µs）、最坏值（1713 µs），并说明为什么超过一个 tick——
线程被唤醒后还要等调度器排上。

### 4. 堆的余量：文档写的是中间状态

`TROUBLESHOOTING.md` 写 `available: 67632`。实测（同一镜像）：

| 阶段 | used | available |
| --- | --- | --- |
| 刚开机 | 28816 | 102160 |
| Wi-Fi + BLE 都在跑 | 52144 | 78832 |

67632 是某个中间状态，不是最坏情况。现在列出三个阶段。

---

## 17. `s3rt key pulse` 的输出看起来像"脉冲未被计数"（已修）

### 现象

```
msh >s3rt key pulse
key   : GPIO0 idle (high), 0 press(es) counted
        pulsed 5 falling edges -> 8 interrupt(s) (PASS - the edge reached the ISR)
```

第一行说 0 次按键，第二行说 8 次中断。看起来像自触发脉冲没被算进按键计数。

### 实际没问题

只有**一个**计数器 `key_presses`，由 `key_isr` 递增。第一行在脉冲**之前**
读取，第二行是脉冲前后的差值。同一变量，读的时刻不同。

### 但输出确实会误导

我上一轮也误判成"有两个计数器，一个走消抖一个不走"。既然连维护者都会看错，
读者更会。

改成同时报新总数：

```
key   : GPIO0 idle (high), 0 press(es) counted
        pulsed 5 falling edges -> 8 new interrupt(s), 8 total (PASS - the edge reached the ISR)
        (8 for 5 edges - contact bounce, expected)
```

多出来的那行只在 `fired > 5` 时打印，解释触点抖动。

### 为什么不加消抖

这个命令验证的是**边沿能不能到达 ISR**。加消抖会掩盖它该暴露的中断风暴。
8 > 5 是 GPIO 外设在 5 ms 间隔里latch 到了抖动，属于预期。

---

## 18. PSRAM 的三个数字不一样，但不是泄漏（已加验证）

### 现象

```
psram : 8388608 bytes detected
heap pool 8388608 total, 8386120 free, 8257536 largest
```

三个数都不同：total 是 8192 KB，free 少 2488 字节，largest 少了整整 128 KB。

### 都不是 bug

| 差值 | 原因 |
| --- | --- |
| free 比 total 少 **2488 字节** | region 注册时 TLSF 拿走做控制结构 |
| largest 比 total 少 **128 KB** | PSRAM 按 **64 KB MMU 页**映射，堆分成多个 region，每个有自己的 TLSF 控制结构，最大那个装不下全部 8 MB |
| BLE 之后 free 不变 | 控制器不从 PSRAM 分配（`BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL`）|

实测：`CONFIG_MMU_PAGE_SIZE=64KB`，128 KB 正好两页。

### 但"largest 限制单次分配"这个说法需要验证

文档原本只是说"看 largest 别看 free"。这是个断言，不是证据。现在
`s3rt psram` 真的去申请：

```
malloc(largest=8257536) ok (free was 8386120, 128584 more than that)
malloc(largest+1) refused, as it should be
```

申请 `largest` 成功、申请 `largest + 1` 被拒——这才证明那个数字对分配确实
有约束力。三次测量 largest 稳定在 8257536。

### 实践含义

**一次分配 8 MB 会失败**，尽管堆里还有 8 MB 空闲。需要大块连续内存时
（比如整个 framebuffer），要么分块申请，要么把 PSRAM 的映射配置改成单段。

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
| 12 | 两个无线只能 BLE→Wi-Fi | 中 | BLE→Wi-Fi 已 6/6；Wi-Fi→BLE 确定性 `Malloc failed` |
| 13 | `s3net` 偶发 `errno=113` | 低 | 长序列里见过 1 次，直接测 4/4 通过；样本不足 |
| 14 | `s3wifiscan` netif 顺序错误 | 高 | 已修：init 之后建 netif，且不重复创建 |
| 15 | `s3fs erase` 后无法挂载 | 高 | 已修：erase 成功后复位 `s_fs_mounted_once`；format 也停 Wi-Fi |
| 16 | 文档性能数字缺前提 | 中 | 已修：注明卷状态、样本量、确定性 vs 波动 |
| 17 | `s3rt key pulse` 输出误导 | 低 | 已修：同时报新总数，并说明抖动 |
| 18 | PSRAM 三个数字不一致 | 信息 | 不是 bug；已加 largest/largest+1 的实测验证 |
