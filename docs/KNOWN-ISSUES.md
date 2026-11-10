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

`s3wifi` 在连接前会检查 BLE 是否在跑，在跑就先 `s3ble_shutdown_quiet()`
拆掉栈，连上之后用户可以再 `s3ble` 重开（不影响已连的 Wi-Fi）：

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

## 2. 文件系统只在开机挂载

### 现象

Wi-Fi + BLE 启动后再格式化 FAT 分区，会：

- 双核在 cache 握手里死锁（静默，无 panic），或者
- FAT 半写，下一次 `fopen` 返回 `EACCES`

### 原因

两个无线栈都会从自己的任务里写 NVS，而 flash 操作不能和它们共存。
即使 `board.c` 里加了 spinlock + depth + core-id + `INTLEVEL` 四重保护，
格式化这种长时间、多次嵌套的擦除仍然会与无线的 flash 访问冲突。

### 现在怎么办

`INIT_APP_EXPORT(s3_fs_boot_mount)` 在无线栈起来之前挂载。**运行中不要重新格式化。**

### 修它需要什么

让 NVS 的 flash 访问也走同一个跨核串行化机制。IDF 的 NVS 用的是
`esp_partition_*` API，那条路径上有自己的锁，但没有和 WL 层的 park 握手协同。

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

## 10. 单核路径的偶发 Guru

### 现象

单核构建下的完整测试，6 次里出现过 1 次 `Guru Meditation`（其余 5 次全绿）。

### 状态

**未定位。** SMP 路径 3/3 全绿，单核路径 5/6。

### 可能

目击次数太少，无法区分是竞态还是别的原因。如果需要单核长期运行，
建议先做冷启动 soak 测试确认。

---

## 汇总表

| # | 限制 | 严重度 | 可行的修法 |
| --- | --- | --- | --- |
| 1 | Wi-Fi 必须先于 BLE | 中 | 让 NimBLE 等 coex 就绪 |
| 2 | 文件系统只在开机挂载 | 中 | NVS 的 flash 访问走同一串行化 |
| 3 | ULP ISR 不释放信号量 | 低 | 验证 RTC 中断时的 cache 状态 |
| 4 | 无中断看门狗 | 中 | 接 level-4/5 定时器 |
| 5 | 无电源管理 | 高（电池场景） | 选定一套 PM 框架 |
| 6 | 无 OTA | 高（远程设备） | 双分区 + 回滚 |
| 7 | 无 DFS 设备节点 | 低 | 开 `RT_USING_DFS` |
| 8 | 无 SAL | 中 | SAL → lwIP 适配 |
| 9 | 无 POSIX | 中 | 开 `RT_USING_POSIX`，解决与 esp_libc 的冲突 |
| 10 | 单核偶发 Guru | 低 | soak 测试定位 |
