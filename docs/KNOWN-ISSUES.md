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

双核没有这个问题。实测依据：

| 检查 | 结果 |
| --- | --- |
| `s3ulpw` 的 `list thread` 行 | `ulpw N/A bind=2` —— 绑定在 CPU1 |
| Wi-Fi 关联后连测 10 次 `s3ulp` | 962 / 631 / 1547 / 1711 / 956 / 1185 / 1026 / 1460 / 817 / 1249 µs |
| 与无 Wi-Fi 对比 | 524–1673 µs，同一区间 |
| BLE 也起来后 `s3ulp` | PASS，259 µs |

**是"绑定"，不是"正在跑"。** `list thread` 的 `cpu` 列显示 `N/A`，因为
`ulpw` 在两次 kick 之间处于 `suspend`——它没有占用任何核，只是每次被唤醒时
调度器把它放到 CPU1。这个区别有实际意义：它解释了为什么 Wi-Fi 的 ISR 在
CPU0 上忙不影响 ULP 的轮询线程被调度到 CPU1。

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

## 19. 152 与 190 写次数：一个被我归因错的数字（已更正）

### 曾经的说法

`FILESYSTEM.md` 里写：`s3fsbench small` 在新卷上是 152/136，在"用过的卷"上是
190/170，并建议复现时先格式化。

**这是错的。**

### 实测

`use_one_fat = true`（当前配置）下，四种卷状态各测一次：

| 卷的状态 | writes / erases |
| --- | --- |
| 刚 `s3fs erase` + 自动格式化 | 152 / 136 |
| 同一个卷再跑一次 | 152 / 136 |
| 跑过 `s3fsbench write`（256 KB 文件）后 | 152 / 136 |
| 跨重启、卷里有上次留下的文件 | 152 / 136 |

四种全部相同。

真正的原因是 `use_one_fat`，A/B 实测：

| 配置 | writes | erases | create 32 小文件 |
| --- | --- | --- | --- |
| `use_one_fat = true` | **152** | **136** | 5506 ms |
| `use_one_fat = false` | **190** | **170** | 7249 ms |

写次数由 FAT 副本数决定——双份 FAT 每次元数据更新要多写一份。与卷的历史无关。

### 错误归因是怎么产生的

早期 build 目录里缓存着 `use_one_fat=false` 时期的配置。测出 190/170 之后，
恰好那次的卷是先跑过别的测试的，于是"卷用过"成了一个看起来合理的解释，
而且它在**那一次**确实复现了（下次再测得也是 190，因为配置还是旧的）。

直到第 33 轮修 CMake 时删掉 `build/` 从零 configure，两个变量才分开：
`use_one_fat` 是 `true`，而卷的历史怎么变都是 152。

### 教训

**两个变量同时存在时，一次"复现"不算证据。** 我当时看到一个相关就停了，
没有做 A/B——而 A/B 只需要改一行、重编一次。

### 仍然成立的部分

`bulk write` 的 958 vs 2312 ms 是真的，但**只影响时间，不影响写次数**：
两种情况都是 79 writes / 71 erases。多出来的是分配器在碎片化的空闲链表里
找 256 KB 连续空间的时间。

---

## 20. 从零 configure 会失败（已修，两个 bug）

**这一条最严重**：只在没有 build 缓存时暴露，所以本地开发一直没撞上，
但任何第一次构建这个仓库的人都会。

### Bug 1：sibling 路径少上一级

`project/s3-rtt-test/CMakeLists.txt` 里找同级 RT-Thread 检出：

```cmake
get_filename_component(_s3_repo_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(RTTHREAD_ROOT "${_s3_repo_root}/../rt-thread")
```

`CMAKE_CURRENT_LIST_DIR` 是 `<repo>/project/s3-rtt-test`，上两级是
`<repo>/project`，再拼 `../rt-thread` 得到 `<repo>/project/rt-thread` —— 不存在。

注释说的是"找 repository 旁边的 `../rt-thread`"，代码算的是上两级。

**症状**：

```
RTTHREAD_ROOT is not set and no RT-Thread checkout was found at
'I:/RT_thread/../rt-thread'.
```

### Bug 2：顶层 `set()` 不写 CAMKE cache

修好 Bug 1 后继续失败：

```
CMake Error at components/rtthread/CMakeLists.txt:48:
  RTTHREAD_ROOT is not set.
```

而上一行刚打印了 `-- s3-rtt-test: RTTHREAD_ROOT=I:/RT_thread/rt-thread`。

原因：组件读的是

```cmake
if(DEFINED CACHE{RTTHREAD_ROOT}) ... else() $ENV{RTTHREAD_ROOT}
```

而顶层用的是普通 `set()`，组件在另一个 scope 里看不到。必须
`set(... CACHE PATH ... FORCE)`。

### 为什么一直没发现

因为 `build/` 一直在，cache 里早就有 `RTTHREAD_ROOT`。**只有删掉 build 目录
才会暴露。** 我这几轮的所有构建都复用了旧 cache。

### 修法

1. 改成探测多个候选路径，取第一个含 `src/scheduler_mp.c` 的：

```cmake
set(_s3_rtt_candidates
    "${CMAKE_CURRENT_LIST_DIR}/../../../rt-thread"   # 标准布局
    "${CMAKE_CURRENT_LIST_DIR}/../../rt-thread"     # 本仓库布局
    "${CMAKE_CURRENT_LIST_DIR}/../rt-thread")       # 扁平布局
```

2. 解析完写入 cache，并加注释说明为什么不能用普通 `set()`。

### 验证

```powershell
Remove-Item build -Recurse -Force
.\tools\build.ps1 -Project .\project\s3-rtt-test
```

从零到 `Project build complete` 成功，烧写后 `check-docs.ps1` 35/35 全绿。
`WL_DEFAULT_TEMP_BUFF_SIZE=4096` 仍在 9 个 wear_levelling 编译单元里生效。

---

## 21. `s3fs format` 创建的是双 FAT 卷（已修）

### 现象

同一个 `s3fsbench small`，同一次启动，取决于卷是怎么创建的：

| 创建方式 | writes / erases | create 32 小文件 |
| --- | --- | --- |
| `s3fs erase` → `s3fs`（自动格式化）| **152 / 136** | 5506 ms |
| `s3fs erase` → `s3fs format` → `s3fs` | **190 / 170** | 7249 ms |

两条路都得到"已挂载的空卷"，`s3fs` 看不出区别。差 20% 写次数、24% 时间。

### 根因

`s3fs format` 调的是：

```c
esp_vfs_fat_spiflash_format_rw_wl(S3FS_MOUNT_POINT, S3FS_PARTITION);
```

它内部把 NULL 转给 `esp_vfs_fat_spiflash_format_cfg_rw_wl()`，后者替换成

```c
{ .max_files = 1, .format_if_mount_failed = true }
```

于是用 **FAT 默认布局（两份 FAT）** 格式化。而 `s3_fs_mount()` 的配置里写的是
`use_one_fat = true`。**同一个命令，两条路径创出不同的卷。**

### 修法

1. 把 mkfs 参数提成一个共享的 `s3_fat_cfg`，mount 和 format 都用它
2. `s3fs format` 改用 `esp_vfs_fat_spiflash_format_cfg_rw_wl()`，传该配置

### 修的过程中撞到的第二个坑

改完之后 `s3fs format` 直接失败：

```
E vfs_fat_spiflash: f_mount failed with error: "No filesystem detected" [13]
E vfs_fat_spiflash: esp_vfs_fat_spiflash_format_cfg_rw_wl(277): Failed to mount
s3fs: format failed 0xffffffff (ESP_FAIL)
```

因为 `_cfg_` 版本**不直接格式化**：它先 mount，只有 mount 失败才格式化。而共享
配置里 `format_if_mount_failed = false`（mount 路径按启动状态设这个字段），
于是它卡在 mount 失败上，什么都不做。

所以 format 时要用一个副本，把这个字段强制成 `true`，其余字段（`use_one_fat`、
`allocation_unit_size`）保持共享。

### 验证

```
erase -> s3fs (auto-format)  -> small : 152/136
erase -> s3fs format -> mount -> small : 152/136
```

两条路一致。`check-docs.ps1` 35/35 全绿。

### 这个 bug 是怎么被发现的

不是靠读代码。是第 32 轮加 `~` 范围断言后，`152` 被写成精确值，完整序列跑出
`190` 才暴露的。**一个"两个数字都合理"的差异，只有在断言了具体值之后才会叫。**

---

## 22. 三个"调用两次"才暴露的 bug（已修）

第 14 条修了 `s3wifiscan` 的两处 netif 顺序错误之后，同样的模式在别的命令里还
存在。这一轮逐个试"把同一个命令跑两次"，找到三处。

### 22.1 `s3wifi` 第二次调用崩

```
E esp_netif_lwip: esp_netif_new_api: Failed to configure netif
assert failed: esp_netif_create_default_wifi_sta wifi_default.c:437 (netif)
```

和第 14 条完全相同的 bug：`esp_netif_create_default_wifi_sta()` 不能调两次。
`s3wifiscan` 当时加了 `esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")` 检查，
`s3wifi` 没加。

修法相同。修完后第二次调用打印 `ok (already created)`。

### 22.2 `s3wifi` 第二次调用等 15 秒后失败

修完 22.1 之后第二次调用不再崩，但变成了：

```
s3wifi: FAIL after 15000 ms: connected=0, retries=0, last reason=0
```

**链路一直是通的。** 原因是 `esp_wifi_connect()` 在已连接状态下返回错误，
没有 DISCONNECT 事件，于是命令等满超时。

加了早退检查——但位置错了一次，放在了等待循环**之后**，等于没加。移到
`esp_wifi_connect()` 之前才对。

**然后仍然失败**，因为 `cmd_wifi()` 开头无条件 `s_wifi_got_ip = 0`，把自己的
检查依据清掉了。这个字段必须保留；其它状态字段（`s_wifi_started` 等）继续清零。

修完后第二次调用：

```
s3wifi: already connected to "..."，nothing to do
s3wifi: PASS (already connected)
```

### 22.3 `s3net` 在 Wi-Fi 未启动时崩溃

```
s3net example.com
assert failed: tcpip_send_msg_wait_sem tcpip.c:454 (Invalid mbox)
```

命令里**有**保护，但只覆盖无参数路径（连网关）：

```c
if (host == RT_NULL) {
    if (netif == RT_NULL || ... ip.gw.addr == 0) {
        rt_kprintf("s3net: no gateway yet ...");
```

带参数时直接走 `gethostbyname()`，在 lwIP 未初始化时断言。

把检查提到所有网络操作之前，判据从"有网关"改成"有 IP"：

```
s3net: no IP yet - run `s3wifi <ssid> <pass>` first
```

### 22.4 `s3ble deinit` 复制的拆栈逻辑漏了保护

拆栈代码有**两份**：`s3ble_shutdown_quiet()` 和 `deinit` 子命令内联的一份。
第 13 轮给前者加了 `s_ble_shutting_down` 标志（阻止 gap 回调在拆栈时重启广播），
后者没加，于是：

```
s3ble deinit
s3ble: adv_set_fields failed rc=30
s3ble deinit: PASS, stack down
```

改成一个实现：`deinit` 调 `s3ble_shutdown_quiet()`。

**这是同一模式的第四例**：保护加在一处，另一处复制了逻辑没复制保护。
前三例是 netif 创建（`s3wifiscan` 修了 `s3wifi` 没修）、`s3net` 的网关检查
（只覆盖一个分支）、`s3wifi` 的重连检查（被自己的初始化清零）。

### 怎么找到的

不是读代码 —— 是先**静态检查哪些命令读状态**，再对可疑的几个**在错误状态下
试一遍**。

四例的发现方式：

| 例 | 怎么发现的 |
| --- | --- |
| 22.1 s3wifi netif | 连续跑两次 |
| 22.2 s3wifi 重连 | 修完 22.1 后重跑 |
| 22.3 s3net 无 IP | 在无网络时跑 |
| 22.4 s3ble deinit | 静态找重复逻辑，然后跑 `deinit` |

静态分析本身没有指出 bug——它只是缩小了范围。**每一例都还是跑出来才确认的。**

现在 `check-docs.ps1` 覆盖了它们（38 节）：

```
[s3net example.com #without ip]
[s3wifi <ssid> <pass> #second call]
```

节名里的 `#` 后缀是为此加的：一节只能是一条命令，而"同一命令跑两次"需要两个
节。`#` 后面只用于区分，不发送给板子。

---

## 23. ADC 的百分比措辞和"三次读数"注释都会误导（已修）

`s3adc` 报告的是 **12 位原始码，没有校准**（driver 头部的注释写明，`get_vref()`
返回 0）。两处输出/注释没有贯彻这一点。

### 23.1 "% of full scale" 读起来像电压

```
s3adc: adc1 ch3 (GPIO4) = 2671 / 4095 (65.2% of full scale)
```

`full scale` 在 ADC 语境里几乎总是意味着电压量程。这里它只是 `4095`，
而这个驱动**不做电压换算**。衰减固定 12 dB，读者会自然算出 `65.2% × 3.3 V`，
那个结果是错的——错多少取决于这颗芯片的 VREF。

改成 `% of code range`，并在代码里说明为什么。

### 23.2 "三次读数证明通道真的变了" 只在驱动的输入上成立

注释原本说连读三次、稳定就说明通道切换成功。实测裸板十个引脚全悬空：

```
ch0 GPIO1  3068 3014 2967      <- 单调下降，每次差约 100
ch1 GPIO2  2821 2777 2741
ch2 GPIO3  2839 2793 2750
ch3 GPIO4  2664 2621 2587
```

**不是随机噪声，是系统性衰减。** 采样保持电容没有驱动源，每次转换分享上一次
剩下的电荷。被驱动的引脚就稳定：GPIO8/9（I2C 带上拉）两次都读 4095。

所以"三次相同"是**驱动输入的证据**，不是要求。注释现在写清了这一点，并附上
实测数据。命令本来就打印全部三次——这个设计正好让差异可见，而不是被平均掉。

### 23.3 顺带确认了一个好设计

`s3adc 1 7`（GPIO8）拒绝读取：

```
s3adc: GPIO8 is the i2c0 SDA pad.
Reading it measures a driven digital line, not an analog input.
Pass 'force' if that peripheral is idle.
```

`force` 时才读，读到 4095（上拉电压）。这个保护是对的——不加的话，用户会
以为自己在测一个模拟输入。

---

## 24. PWM 只测了一个 50% 点，且读回的 timer 寄存器是 0（已修）

### 24.1 一个点证明不了调制

`s3rt pwm` 原来只设一次 50%，读回 duty 寄存器，然后打印"LED should be dimmed"。
那是**指令**，不是测量。一个 50% 点无法区分"duty 跟随请求"和"某个值被写进去
一次"。

现在扫 25% / 50% / 75% 三点，各自和请求比例对比：

```
pwm   : ch0 on GPIO48, 1000000 ns period (about 1000 Hz)
 25%: duty 65536, expected ~65525 ok
 50%: duty 131072, expected ~131050 ok
 75%: duty 196608, expected ~196575 ok
```

比值精确到 1/4、1/2、3/4——这是这块板子在**没有示波器**的情况下唯一能测的
PWM 属性（GPIO 采样跟不上 1 kHz 方波）。

### 24.2 第一次改进反而报错了

第一版用 `LEDC.timer_group[0].timer[n].conf.duty_resolution` 和 `conf.clock_divider`
推满量程，两个都读回 **0**：

```
pwm   : ch0 on GPIO48, 1000000 ns period (about 1000 Hz), res 0 (1 steps)
 25%: duty 65536, expected ~0 OFF
```

duty 本身是对的（65536 = 2^18 × 1/4），但 `res` 算出的满量程是 1，于是期望值
是 0，判据报了三个 OFF。

**timer 寄存器路径和这个驱动配置的字段对不上**，duty 字段对得上。改成直接从
duty 字段推满量程（2^18），并把这个陷阱写进注释——否则下一个人会再犯。

### 24.3 我的期望清单又错了一次

三个 `~` 约束行全写在了最后一个 pattern 之后，于是全部约束到 75% 那一行：

```
missing: 75%: duty (\d+), ... (group 1 is 196608, outside 60000..70000)
```

工具的行为是"~ 约束它上面的那个 pattern"，我写成了"三个一起约束最后一个"。
每个 `~` 必须紧跟它约束的行。

---

## 25. I2C 的"0 device"什么都证明不了（已加 i2cwatch）

### 问题

`i2cscan` 在裸板上报 `0 device(s)`。**一个从来没驱动过线的驱动也会报同样的
结果。** NACK 测试无法区分"波形发出去了、没人应答"和"根本没发"。

### 解决：数 SCL 边沿

`i2cwatch` 在一次传输期间对 SCL pad 挂下降沿中断，数边沿。pad 由 I2C 外设
经 GPIO 矩阵驱动，但它的输入路径仍然采样真实引脚，所以 GPIO 中断看到的是
真实波形。一次地址写是 8 数据位 + 1 ACK 槽 = 9 个时钟。

```
i2cwatch: i2c0 addr 0x50 at 10000 Hz -> rc=-8 (NACK), SCL(GPIO9) edges=9
i2cwatch: PASS - the bus is being clocked (9 edges; 8 data bits + 1 ACK slot)
```

**现在 "0 device" 是有意义的**：波形确实发出去了，没有器件应答。

### 两个坑

**坑 1：100 kHz 下中断来不及。** 半周期 5 µs，ISR 进出装不下。实测同一命令
跑四次：

| 频率 | 四次边沿数 |
| --- | --- |
| 100 kHz | 10, 10, **1**, 10 |
| 10 kHz | **1**, 9, 9, 9 |
| 1 kHz | 9, 9, 9, 9 |

不是总线问题，是观测手段不够快。默认改成 10 kHz。

**坑 2：改频率后第一次传输总是 1 个边沿。** 新的分频值在那一笔交易中途才
生效。这让 `i2cscan`（它会恢复 100 kHz）之后的第一次 `i2cwatch` 必然失败，
而单独跑 `i2cwatch` 通过。

修法：改频率后先做一次丢弃传输，再开始计数。地址无关紧要，只要让时钟切换
落地。

### 这个坑的形状值得记

和前面几轮的模式一样：**单独跑通过、在序列里失败**。区别是这次不是状态泄漏，
是"设置生效需要时间"——更隐蔽，因为没有任何状态变量能显示它。

### 从"时钟在动"到"每一位都对"

SCL 边沿数只证明总线在被驱动。要证明**数据位正确**，SDA 也要数。

一次地址写 = START + 8 位地址（含 R/W）+ ACK 槽 + STOP。地址帧里相邻位之间的
每一次变化都是一个 SDA 边沿，所以边沿数取决于地址值：

```
SDA 边沿 = 位转换数 + 2 + (帧 != 0 ? 1 : 0)
```

实测（10 kHz，NACK，即从机不拉低 SDA）：

| 地址 | 帧 | 位转换 | 模型 | 实测 |
| --- | --- | --- | --- | --- |
| 0x00 | 0x00 | 0 | 2 | **2** |
| 0x50 | 0xA0 | 3 | 6 | **6** |
| 0x55 | 0xAA | 7 | 10 | **10** |
| 0x77 | 0xEE | 3 | 6 | **6** |

四个全部吻合。**这是位级正确性的证据**——SDA 上的波形严格对应发出去的地址位，
不是"时钟可能动了"。

`i2cwatch` 现在会把这个模型打印出来，读者可以自己核对，不必相信一个孤立的数字：

```
i2cwatch: i2c0 addr 0x55 at 10000 Hz -> rc=-8 (NACK), SCL edges=9, SDA edges=10
i2cwatch: address frame 0xaa has 7 bit transitions, so ~10 SDA edges
```

> **数据字节没有被验证。** 地址 NACK 之后主机就中止了传输，`i2cwatch ... <byte>`
> 里的字节根本发不出去。要验证数据段需要真正的从机应答，那是另一件事。

---

## 26. 浸泡测试：10 轮无内存漂移、无累积故障

前面所有验证都是单次运行。`KNOWN-ISSUES` 里最难查的几个问题（`errno=113`、
偶发 Guru）只在长序列里出现过一次，单次检查抓不到。

`tools/soak-docs.ps1` 在**一次启动内**重复跑完整工作负载，每轮抓内存快照。

### 结果

| 指标 | 第 1 轮 | 第 10 轮 | 变化 |
| --- | --- | --- | --- |
| 内部 DRAM 空闲 | 27348 | 27324 | **-24 字节** |
| PSRAM 空闲 | 8386120 | 8386120 | **0** |
| 内核堆可用 | 87616 | 87616 | **0** |
| UART 中断计数 | 284 | 2822 | +282/轮 |
| tick | 453147 | 4388197 | +393504/轮 |

```
RESULT: PASS - 10 iteration(s), no fault markers
```

总运行时间约 65 分钟（tick 显示 4388 秒自上次复位）。

### 三个数字怎么读

**-24 字节出现在第 1 轮到第 2 轮之间，之后完全不动。** 泄漏会持续下降；
一次性分配不会。这是缓存或池的首次使用。

**UART 中断 +282/轮** 是键盘输入本身产生的中断——脚本每轮发约 15 条命令，
每条约 19 个字符。稳定即可，绝对值无意义。

**tick +393504/轮 = 393 秒/轮**，与每轮实际耗时吻合，说明 tick 没有跑快或跑慢。

### 这证明了什么，没证明什么

**证明了**：10 轮完整工作负载（WiFi、BLE、ULP、文件系统、SPI、I2C、ADC、
中断）之后，三个内存池零漂移，没有累积性故障。

**没有证明**：24 小时以上的稳定性。10 轮 65 分钟是一个起点，不是结论。慢速
泄漏（每秒几字节）在这个时间尺度上会被 -24 的噪声掩盖。

### 顺带修了脚本自己的 bug

第一版把 `uart0 irq` 的抓取模式套在 `s3rt` 的输出上，但那一行来自
`s3rt irq`（另一个命令），于是 irq 列全程显示 `?`。跑了两轮才发现。

---

## 27. 性能数字没有标注测量条件，而实际是 `-Og` 构建

### 问题

文档里 141 处时间数字，**没有一处说明构建条件**。查了 `sdkconfig` 才发现：

```
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y      -> -Og
rtconfig.py: CFLAGS += ' -gdwarf-4 -ggdb -Og'
```

**两边都是 `-Og`。** 这是个调试构建，断言全开。所有文件系统性能数字都是在这个
前提下测的，而文档一个字都没提。

### 修法

`FILESYSTEM.md` 加了一节"测量条件"，列出芯片、Flash 模式、PSRAM、优化级别、
tick、MMU 页大小、构建版本、无线状态。**优化级别单独强调**，因为它是唯一一项
改了就必然要重测、且最容易被忘掉的。

### 顺带做了一次受控对比

改到 `-O2` 重测中断延迟：

| 构建 | 中断延迟 |
| --- | --- |
| `-Og` | 434..480 cycles |
| `-O2` | 423..471 cycles |

**只差 2.5%。** 这是有意义的：中断延迟主要是硬件跳转加固定代码，编译器能优化
的地方很少。所以这个数字对构建配置不敏感，可以放心引用。

文件系统的时间当时没有受控对比，因为两次测量的卷新旧不同。**后来补做了**，
两次都用新格式化的卷：

| 指标 | `-Og` | `-O2` | 差 |
| --- | --- | --- | --- |
| 建 32 小文件 | 8766 ms | 8793 ms | +0.3% |
| 覆写一文件 8 次 | 2689 ms | 2714 ms | +0.9% |
| 删除 32 文件 | 4504 ms | 4388 ms | -2.6% |
| 写次数 | 152 | 152 | 0 |
| 擦除次数 | 136 | 136 | 0 |

**几乎没有影响**，因为时间全花在 SPI NOR 擦除的物理等待上，不是 CPU 计算。

这个补测纠正了我自己的一个错误结论。第一次在 `-O2` 下测到 8832 ms，我对比文档
里更早的 5281 ms，以为优化级别让文件系统慢了 67%。实际两个数字来自不同状态的
卷，真实差距不到 1%。**混在一起的变量会让结论完全反向。**

### 顺带完成了 `-O2` 全套验证

改到 `-O2` 后完整跑 50 节：

```
RESULT: PASS - all 50 section(s) matched
```

内核、SMP、外设、文件系统、Wi-Fi、BLE、ULP、看门狗全部在优化构建下正常。这是
一个比 `-Og` 强得多的保证——`-O2` 会暴露 `-Og` 看不见的未定义行为，第 28 条就是
一个例子。

---

## 28. `-O2` 编译暴露了一个真实的严格别名 bug（已修）

### 发现

为了做上面的对比改到 `-O2`，构建直接失败：

```
s3net.c:141:31: error: dereferencing type-punned pointer will break
                strict-aliasing rules [-Werror=strict-aliasing]
```

```c
addr.sin_addr.s_addr = ip.gw.addr;
rt_kprintf("s3net: target gateway " IPSTR ":%d\n",
           IP2STR((esp_ip4_addr_t *)&addr.sin_addr), port);
```

`struct in_addr` 和 `esp_ip4_addr_t` 形状相同但**是不同的类型**，通过指向一个的
指针读另一个是未定义行为。`-Og` 不说，`-O2` 拒绝——**编译器是对的，这不是挑剔**。

### 修法

不转指针，用本地变量：

```c
esp_ip4_addr_t gw_addr = { .addr = 0 };
...
gw_addr.addr = ip.gw.addr;
rt_kprintf(..., IP2STR(&gw_addr), port);
```

### 值得记的地方

**这个问题在 `-Og` 下永远不会暴露**，而项目一直用 `-Og`。也就是说，如果不做这轮
"标注测量条件"的文档工作，就不会去改优化级别，也就不会发现它。

一个纯文档任务捞出了一个未定义行为。计划外的收获，但它是真的。

---

## 29. 排障文档里有三条指引已经过时，其中一条是完全反的

### 问题

`TROUBLESHOOTING.md` 是从实际踩坑里总结的，但坑修好之后没人回头改它。通读一遍
发现三处：

| 位置 | 原来写的 | 实际 |
| --- | --- | --- |
| 无线顺序表 | Wi-Fi → BLE ✅ 4/4，**必须 Wi-Fi 先起** | **完全相反**：必须 BLE → Wi-Fi |
| 写小文件 | "100+ ms 一个，约 21 KB flash" | 273 ms 一个，约 17 KB |
| RISC-V 工具链 | "检查 `build.ps1` 的 `$pathParts` 含 `esp-15.2.0_20251204`" | 工具链现在是自动发现任意版本 |

### 第一条最糟

无线顺序表与 `KNOWN-ISSUES.md` 第 12 条**完全相反**。读者照着排障文档做，会得到
`Malloc failed`，然后以为自己操作错了——而正确做法就写在另一份文档里。

修复方式是把它改对，并加一句说明这张表曾经是反的。**留着这句话不是为了自我批评，
是为了让下次有人发现类似矛盾时知道该怀疑哪一边。**

### 为什么会发生

`KNOWN-ISSUES.md` 每修一个 bug 就加一条，编号一直涨到 29 条。`TROUBLESHOOTING.md`
是另一条维护线，改动时没人对照前者。**两份文档描述同一批现象，却没有机制保证一致。**

### 能机械检查的部分已经检查了

`verify-doc-links.ps1` 会确认 `TROUBLESHOOTING.md` 里"第 N 条"指向的
`KNOWN-ISSUES` 条目真实存在——引用不会指向不存在的编号。

但**"两份文档说的是不是同一件事"没法机械检查**。这次的三处都是人工读出来的。
所以给 `TROUBLESHOOTING.md` 加了一段开头说明，讲清哪些内容是"现在还会遇到"
哪些是"历史记录"，并明确写了：**发现对不上就报 issue，过时的指引比没有更糟。**

---

## 30. 同一事实抄在多处，"优化后"那一列取了四次里最快的一次

### 三处发现

**1. "优化后 5281 ms" 是四次测量里最快的一次。** `FILESYSTEM.md` 自己列着
5374 / 5458 / 7379 / 5281，但最终数据表拿 5281 当"优化后"的代表值。跨度 40%，
却只报了最好看的那次。

后来独立重测（新格式化卷、同一镜像）是 **8766 ms**，比 5281 慢 66%，接近"优化前"
的 8539。

**这不是优化失效。** 写次数和擦除次数（152 / 136）在每次测量中完全一致——那才是
优化真正改变的东西，而且它有确定性。时间值不稳定，因为大头是 SPI NOR 的物理擦除。

**2. `8539 ms (3 files/s, 337 ms each)` 算术不自洽。** 8539 / 32 = 267，不是 337。
337 × 32 = 10784，与 8539 差 2200 ms。当前固件自己算这一列是对的
（实测 `8766 ms (274 ms each)`，8766 / 32 = 273.9），所以错在文档。

**3. TUTORIAL 抄了 FILESYSTEM 的性能数字和内存数字。** 内存那处用了约数
（`used 27000, maximum 31000`），实测是 28816 / 32640——那个 31000 从没真正出现过。

### 修法：不是改数字，是取消重复

改数字只能救这一次。真正的修法是**一份事实一个来源**：

- `FILESYSTEM.md` 是性能数据的权威来源。`TUTORIAL.md` 只保留"输出长什么样"，
  加一句：**看形状就行，别对着数字抠**，并说明真正有诊断价值的是写次数和擦除次数。
- 内存数字统一用实测值，不用约数。

### 为什么约数也是问题

`27000 / 31000` 看起来"诚实地表示这是个约数"，但它掩盖了两件事：真实值是多少，
以及那个 31000 从来没出现过。**约数不是免责声明，它只是把不准确藏起来了。**

---

## 31. 全新克隆审计：能构建，但 README 少了一步

### 做了什么

在 `C:\s3clone` 里 `git clone` 这个仓库，只按 README 的步骤走。第一次连上游都没
准备，构建直接报：

```
IDF_PATH is not set and no ESP-IDF checkout was found next to this repository.
```

### 问题 1：README 没说要先把上游放成**同级目录**

`tools/env.ps1` 会去 `..\rt-thread` 和 `..\esp-idf-rtt` 找上游——这个设计是好的
（不用设环境变量），但**README 从没说过**。它直接给出

```powershell
.\tools\fetch-upstream.ps1 -RtThreadPath I:\RT_thread\rt-thread -IdfPath I:\RT_thread\esp-idf-rtt
```

那是我自己的路径。新读者会照抄，然后失败。

已加一段目录结构示意，并说明也可以用 `IDF_PATH` / `RTTHREAD_ROOT` 环境变量指向
任意位置。

### 问题 2：首次构建会打印 `fatal:`，但构建是成功的

把上游放好后重新构建：

```
fatal: no submodule mapping found in .gitmodules for path 'docs/开发板资料/docs'
fatal: not a git repository: .../.git/modules/components/openthread/...
Project build complete. To flash, run:
```

**同一段输出里既有 `fatal:` 又有 `build complete`。** 这是最容易让人放弃的那种输出。

来源：CMake 配置阶段会调 `git submodule` 查询 IDF 树的状态。那棵 IDF 树是打过补丁
的本地副本，git 索引里留着**没有 `.gitmodules` 映射的 submodule 记录**——实测
`git ls-files -s` 显示那条路径是 `160000` 模式（子模块）但没有对应配置。

### 更麻烦的是退出码

```
powershell -File build.ps1 ... | Select-Object -Last 12
   -> [exit code: 1]      <- 看起来失败

powershell -File build.ps1 ... *> build.log
   "exit code: $LASTEXITCODE"
   -> 0                   <- 实际成功
```

**通过管道时 PowerShell 会记一条 native stderr 错误，于是报 exit 1。** 重定向到
文件就不会。这个差异本身就会误导人。

### 三处修改

1. `README.md`：加了目录结构示意，说明"上游要放成同级目录"。
2. `docs/BUILD.md` 新增「3a. 第一次构建的 git 噪音」，讲清来源、怎么确认成功、
   以及怎么去掉那条野记录。
3. `tools/build.ps1`：**首次配置成功后主动打印一段解释**，不靠读者先去读文档。

第 3 条是关键——文档要人去找，而提示就在他正在看的输出里。

### 验证

改完后在 `C:\s3clone` 里删掉 `build/` 重跑：

```
exit code (fresh): 0
Project build complete. To flash, run:
  note: if the output above contained
          fatal: no submodule mapping found in .gitmodules for path ...
        that came from CMake querying the IDF tree's git state. The build
        still succeeded ...
```

**而且产出的二进制与开发机上字节数完全一致**（1129360 字节）。哈希不同是正常的
（构建时间戳会嵌进去），大小相同说明代码路径一样。

---

## 32. `check-docs.ps1` 把不需要凭据的 `s3wifiscan` 也跳过了

### 问题

检查器有 `needsWifi` 这个列表，决定"没有 `-WifiSsid` 时跳过哪些节"：

```powershell
$needsWifi = @('s3wifiscan', 's3net example.com',
               's3wifi <ssid> <pass>', 's3wifi <ssid> <pass> #second call')
$needsCreds = @{ 's3net example.com' = $true }     # <- 定义了，从没用过
```

**`s3wifiscan` 根本不需要凭据。** 它不接参数（源码里是
`RT_UNUSED(argc)`），只是扫描一遍报告看到什么。它唯一的要求是**附近有个
2.4 GHz AP**——而这恰恰是"我有一个板子但没有 WiFi 环境"的人想验证的东西。

结果那个人跑 `check-docs.ps1`，`s3wifiscan` 被**静默跳过**，只在 `-List` 里有个
`(needs -WifiSsid)` 的标注——而那个标注是错误的。

### 更糟的细节

`$needsCreds` 这个更精确的表**在第 127 行的判断里从未被使用**。它被定义、被命名，
然后没人用。跳过判断用的是 `$needsWifi`。

### 修法

拆成两个概念：

| 变量 | 含义 | 内容 |
| --- | --- | --- |
| `$needsRadio` | 需要附近有 2.4 GHz AP | `s3wifiscan` |
| `$needsCreds` | 需要输入 SSID / 密码，或需要出网 | `s3net example.com`、两个 `s3wifi` |

跳过判断改用 `$needsCreds`——因为**只有凭据是能从命令行提供的，AP 不是**。附近
有没有 AP 是环境事实，跳过它并不能让检查通过，只会让人看不见结果。

### 验证

```
带凭据  : RESULT: PASS - all 50 section(s) matched
不带凭据: RESULT: PASS - all 47 section(s) matched     <- 含 s3wifiscan
```

差的三节是 `s3net example.com` 和两个 `s3wifi`。**`s3wifiscan` 在不带凭据时也会跑
并报告它扫到的 AP 数。**

---

## 33. 顺序约束只写在注释里，违反要跑 40 分钟才发现

### 问题

`expected-output.txt` 的头部列了四条顺序规则，但**只有第四条（reboot 节最后）
在代码里强制**。其余三条是散文，靠人记住。

这个错误犯过两次，每次都是完整跑一遍才暴露：

| 犯的错 | 症状 | 代价 |
| --- | --- | --- |
| `s3fs rm s3fs.txt` 排在 `s3fs read` 之前 | 后者报 `not found - run s3fs first` | 一次完整运行 |
| `spiloop #external` 排在 `spiloop internal` 之前 | 外部那次的结论没有意义 | 一次完整运行 |

而这类"失败"根本不是 bug——是顺序错了。

### 修法：声明式约束 + 解析时校验

在节头上加一行：

```
[s3fs rm s3fs.txt]
# scope: B - ...
# order: after s3fs read
```

`check-docs.ps1` 在**打开板子之前**校验全部约束：

```
  ORDER VIOLATIONS in tools/expected-output.txt
    [s3fs rm s3fs.txt] (position 29) says 'after s3fs read' (position 30)

  These are declared in the file, so the fix is to move a section or to
  correct a declaration - not to run the suite and see what breaks.
```

负向测试：故意把那节挪到 `read` 之前，`-List` 立刻报出两个位置，退出码 1，
板子完全没被碰过。

### 现在有 10 条

其中两条是"计时的节必须在任何无线起来之前"（`s3fsbench small` / `write` 在
`s3ble` 之前）。这一条的理由是量化的：Wi-Fi 关联之后同一个 benchmark 从 1055 ms
变成 4567 ms，4 倍差距会让任何范围断言失去意义。

### 为什么值得做成工具而不是记住

**记不住的东西不该靠记。** 十条约束、50 个节、每轮都在增删——人和工具在这一项上
的可靠性差距不是一点半点。而检查成本从"40 分钟一次运行"降到"读一遍文件"。

---

## 34. 声明式元数据没有完整性校验，写错时静默降级

### 问题

`expected-output.txt` 里现在有两种被代码读取的声明：`# scope:` 和 `# order:`。
但在本轮之前，**它们写错时没有任何东西会报错**：

| 写错的方式 | 之前的表现 |
| --- | --- |
| 某节漏了 `# scope:` | `-List` 里显示 `[?]`，其它一切正常 |
| `# scope: X`（非法值） | 原样传下去，不报错 |
| `# order: after 不存在的节` | 依赖后续的重复检查碰巧报错 |
| `# scpoe: U` | 被当成普通注释忽略 |

第一条是上轮真实发生过的：我给 50 节加 scope 时漏了一节，是靠**盯着 `-List` 输出**
发现的。**工具应该主动报错，不该指望人看输出。**

### 修法

打开板子之前校验四件事：每节必须有 scope、值必须是 U/B/E、order 目标必须存在且方向
合法、头部出现未知键就报错。四条都做了负向测试，全部 exit 1 并报出具体位置。

### 一个真误报，暴露了元数据的定义问题

第一版校验把这段散文当成了元数据：

```
# apart: 8 data bits plus the ACK slot is 9 falling edges.
```

键名被解析成 `apart`，报"未知的键"。它和 `# scope: U - ...` **在语法上完全一样**。

结构上的区别只有一个：**元数据出现在节头之后、第一个 pattern 之前**。校验改成按
位置判断后，散文怎么写都不会误报。

这件事值得记，因为它说明"给注释加语义"本身有代价：**一旦某些注释被代码读取，
就必须有一条无歧义的规则区分它们和普通注释**。靠语法（形状）是不够的，得靠位置。

---

## 已修条目的回归覆盖

读者会问："你说修了，我怎么知道？"这张表回答它。

| 条目 | 怎么防止复发 |
| --- | --- |
| 11 | `i2cscan` 断言 flaky / timeout 计数 |
| 14 | `s3wifiscan` 断言 `N AP(s)` |
| 15 | `s3fs after erase` 断言重新格式化并挂载 |
| 17 | `s3rt key pulse` 断言 delta 和 total 同时出现（本轮补上） |
| 21 | `s3fsbench small` 断言写次数 **152**、擦除 **136**（精确值，不是范围） |
| 22 | `s3ble deinit` 断言 `PASS, stack down`；22.x 的另外三处各有对应命令 |
| 23 | `s3adc` 断言 `% of code range` 措辞 |
| 24 | `s3rt pwm` 断言 25/50/75 三点的 duty 都在范围内 |
| 25 | `i2cwatch` 断言 SCL/SDA 边沿数 |
| 28 | **没有命令级断言**——它靠 `-O2` 构建通过来验证 |
| 29 | **没有命令级断言**——它靠 `verify-doc-links.ps1` 和人工通读 |
| 30 | 部分：`verify-doc-facts.ps1` 检查结构数字；性能数字靠人工 |
| 20 | **没有命令级断言**——它靠"删掉 build/ 从零配置"这个流程 |

**三条没有检查器覆盖的（20、28、29），验证方式是流程而不是断言。** 写在表里，
是为了不用假装它们有覆盖。

### 补上 #17 时发现的事

`[s3rt key pulse]` 这一节一直在，但只断言了 `key : ... counted` 那一行——**而
#17 修的正是下一行的措辞**。修复在代码里，检查器却看不见它。如果有人改回旧格式，
50 节依然全绿。

现在断言了完整那行，包括 `new interrupt(s)` 和 `total` 两个数。改回旧格式会失败。

---

## 汇总表

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
| 19 | 152 vs 190 写次数归因错误 | 中 | 已更正：是 `use_one_fat`，不是卷的历史；A/B 实测 |
| 20 | 从零 configure 失败 | **高** | 已修：sibling 路径 + RTTHREAD_ROOT 未写 cache |
| 21 | `s3fs format` 创建双 FAT 卷 | **中** | 已修：共享 mkfs 配置，两条路径一致 |
| 22 | 三个"调用两次"才暴露的 bug | **中** | 已修：s3wifi netif/重连、s3net 无 IP 保护 |
| 23 | ADC 百分比措辞与三次读数注释 | 低 | 已修：改 "% of code range"，注释附实测数据 |
| 24 | PWM 只测一个点，timer 寄存器读回为 0 | 低 | 已修：三点扫描，从 duty 字段推满量程 |
| 25 | I2C "0 device" 无法区分没发/没器件 | 中 | 已加 `i2cwatch` 数 SCL 边沿；10 kHz 默认 + 预热传输 |
| 26 | 单次检查抓不到累积性问题 | 信息 | 已加 `soak-docs.ps1`；10 轮 65 分钟零漂移 |
| 27 | 141 处性能数字无测量条件，实际是 `-Og` | 中 | 已加"测量条件"章节；-O2 对比中断延迟只差 2.5% |
| 28 | `-O2` 暴露严格别名 UB | 中 | 已修：改用本地 `esp_ip4_addr_t` 而非指针强转 |
| 29 | 排障文档三条指引过时，一条完全相反 | **高** | 已修：无线顺序、写小文件数字、工具链查找方式 |
| 30 | "优化后"取了最快值；文档间重复且算术不自洽 | 中 | 已修：标注时间不可靠、修正算术、取消重复 |
| 31 | README 缺"上游要放同级目录"；首次构建有误导性 fatal | 中 | 已修：README/BUILD 补说明，build.ps1 主动提示 |
| 32 | 无凭据时 `s3wifiscan` 被错误跳过；`$needsCreds` 定义了没用 | 中 | 已修：拆 needsRadio / needsCreds，跳过只看凭据 |
| 33 | 顺序约束只在注释里，违反要跑 40 分钟才发现 | 中 | 已修：`# order:` 声明 + 解析时校验，10 条约束 |
| 34 | 声明式元数据写错时静默降级 | 中 | 已修：四项完整性校验，四项负向测试 |
