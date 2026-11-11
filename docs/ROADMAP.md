# 路线图

按"投入产出比"排序，不是按重要性。

**这份文档会过时。** 做完一项就把它从"计划"移到 [KNOWN-ISSUES.md](KNOWN-ISSUES.md)
的"已修"或 README 的"支持情况"，否则读者会按过时的前提做决定。第 1 条就曾经
停留在"未解决"好几个版本，而它其实已经修了。

---

## 短期（1-3 天每项）

### 1. 让 Wi-Fi 起来之后还能再起 BLE（**未解决，投入产出比低**）

**已修的部分**：BLE → Wi-Fi 曾经是 0/4，现在是 6/6。根因不是射频，是
`s3wifi` 里拆 BLE 的代码排在了 `esp_wifi_init()` **之后**——Wi-Fi 要一次性分配
10 个 1600 字节的静态 RX buffer，BLE 还占着内部 DRAM 就失败了。移到前面即可。
详见 [KNOWN-ISSUES.md](KNOWN-ISSUES.md) 第 12 条。

**剩下的**：Wi-Fi 已经占住内部 DRAM 之后，BLE controller 拿不到它需要的
> 56 KB 连续内部 DRAM。实测：

```
                   内部 DRAM 空闲    最大连续块
开机              :    93512           53248
Wi-Fi 起来后      :    59364           31744
s3ble 尝试之后    :     2568            2304
```

失败形态有两种，取决于碎片形状：多数是 `BLE_INIT: Malloc failed`，
少数是注册 `ETS_RWBT_NMI_SOURCE` 时断言（`intc.c:222`）。

**试过无效的**（全部已回退，详见第 12 条）：PSRAM 分配模式、Wi-Fi 静态/动态
buffer 数量、`BT_CTRL_BLE_MAX_ACT` 6→2（只腾出 3.7 KB）、NimBLE 连接数/MTU。

**下一步唯一有意义的**：`libbtdm_app.a` 是预编译的，只能靠实测推断那 56 KB
花在哪。MAX_ACT 减到三分之一只省 3.7 KB，说明大头是固定结构而非
per-activity 缓冲。压 activity 这条路走不通。

**风险**：高，收益低。两个无线同时在线对这块板子的 512 KB 内部 DRAM 本来就
紧张。**建议按已知限制接受**，用 BLE → Wi-Fi 的顺序。

---

### 2. BLE 加一个真实的 GATT service

现在的 service 是演示用的（一个只读计数器 + 一个 LED 开关）。

**做法**：加一个电池服务（`0x180F`）或设备信息服务（`0x180A`），
让手机端的通用 BLE 工具能识别出这是什么设备。

**验证**：nRF Connect 能看到标准的 service 名称。

**风险**：低。

---

### 3. `s3fs` 加目录操作

现在只能写固定的 `s3fs.txt`。

**做法**：加 `s3fs ls` / `s3fs cat <file>` / `s3fs rm <file>`，用 `opendir` /
`readdir`（IDF VFS 提供）。

**验证**：连续创建 10 个文件，`ls` 能列出来。

**风险**：低。

---

## 中期（1-2 周每项）

### 4. 接 RT-Thread 的 SAL

**问题**：socket 直接走 lwIP，`ifconfig` / `ping` / `netstat` 都没有，
RT-Thread 的网络组件用不了。

**做法**：
1. 开 `RT_USING_SAL` / `RT_USING_SOCKET` / `RT_USING_NETDEV`
2. 实现一个 `sal_proto_family` 把 SAL 的 socket 调用转到 lwIP
3. 把 `esp_netif` 的接口注册到 netdev

**验证**：`ifconfig` 能列出接口和 IP，`ping` 能通。

**风险**：中。SAL 和 lwIP 的 socket 语义要仔细对齐（超时、非阻塞、错误码）。

---

### 5. 开 DFS + 设备节点

**做法**：
1. 开 `RT_USING_DFS`（选 v1，v2 的 API 还在变）
2. 把 `/storage` 作为一个 DFS 挂载点（或者反过来，把 DFS 挂在 IDF VFS 上）
3. 加 `devfs` 提供 `/dev/uart0` 之类的节点

**验证**：`ls /`、`cd /storage`、`cat /storage/s3fs.txt` 能工作。

**风险**：中。DFS 和 IDF VFS 是两套 VFS，需要决定谁在上层。

**注意**：`RT_USING_DFS` 会引入 `dfs_v1` 和 `dfs_v2` 的选择，选错了后续改很麻烦。

---

### 6. 中断看门狗

**问题**：只有任务看门狗，卡在关中断状态的 handler 检测不到。

**做法**：
1. 把 Xtensa 的 level-4/5 定时器中断接进 `_xt_interrupt_table`
2. 调 IDF 的 `esp_int_wdt_init()`
3. 在 handler 里打印当前 PC 并复位

**验证**：写一个故意关中断死循环的命令，确认能复位且打印出卡住的地址。

**风险**：中。level-4/5 中断的进入/退出路径和 level-1 不同，要和
`_rtt_int_enter/_exit` 的嵌套计数对齐。

---

## 长期（1 个月+）

### 7. 电源管理

**问题**：功耗是 datasheet 最大值。

**决策点**：用 RT-Thread 的 PM 还是 IDF 的 `esp_pm`？

| 选项 | 优点 | 缺点 |
| --- | --- | --- |
| RT-Thread PM | 和内核集成好 | 要重写所有 `esp_pm_*` 调用点 |
| IDF `esp_pm` | IDF 组件原生支持 | 要和 RT-Thread 的 idle 线程协同 |

SMP 下还要额外考虑两核的空闲协同——一核 idle 不代表另一核也 idle。

**验证**：`esp_pm` 的 `light_sleep` 能进，Wi-Fi 的 DTIM 唤醒正常，功耗仪读到下降。

---

### 8. OTA

**问题**：分区表只有一个 app 槽。

**做法**：
1. 改分区表：`ota_0` / `ota_1` 各 3 MB（8 MB flash 够，但只剩 2 MB 给文件系统）
2. 接 `esp_ota_begin` / `esp_ota_write` / `esp_ota_end` / `esp_ota_set_boot_partition`
3. 开 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`，在新固件启动后调
   `esp_ota_mark_app_valid_cancel_rollback()`
4. 提供 HTTP 或 BLE 的传输通道

**验证**：刷一个会崩溃的固件，确认能自动回滚。

**风险**：高。回滚逻辑写错会让设备变砖。

---

### 9. 深度睡眠 + ULP 唤醒

**问题**：`CONFIG_PM_ENABLE` 关闭，ULP 只能轮询不能被主核"用起来"。

**做法**：
1. 开 `CONFIG_PM_ENABLE`
2. ULP 程序改成：采样 → 超阈值时 `ulp_riscv_wakeup_main_processor()` → `ulp_riscv_halt()`
3. 主核 `esp_deep_sleep_start()`，醒来后读 ULP 留下的数据
4. 加一个 `s3sleep` 命令演示

**验证**：电流从 mA 级降到 µA 级，且 ULP 能按预期唤醒。

**风险**：高。深睡后 RTC 内存保留、时钟源切换、唤醒源配置都容易错。

---

## 不打算做的

| 项 | 原因 |
| --- | --- |
| littlefs | 需要引入外部依赖；FAT 在当前用途下够用，见 [FILESYSTEM.md](FILESYSTEM.md) |
| RT-Thread 的 `RT_USING_LIBC` | 和 IDF 的 `esp_libc` 冲突 |
| 路线 B（纯 scons 构建） | 需要重造整个 IDF 运行时，见 README 的对比 |
| Xtensa 的 `call0` ABI | S3 用窗口化 ABI（`call8`），`call0` 只在 ESP32 的老核上 |

> **`bsp/ESP/ESP32_S3/SConscript` 是 RT-Thread BSP 的标准模板**，不是路线 B 的
> 实现。它只做一件事：收集子目录的 `SConscript`。而 `board/` 目录里没有
> `SConscript`，所以它什么也不收集，`scons` 构建这条路仍然是空的。
>
> 保留它是因为上游所有 BSP 都有这个文件，删掉反而不符合惯例。但看到它存在
> 就以为"也许 scons 能编"是个合理的误判——所以在这里说清楚。

---

## 怎么参与

每个事项都应该是：

1. **先写测试**（现有的命令都是这个思路：`s3smp`、`s3fs read`、`s3ulp`）
2. **记录实测数据**（不是"感觉快了"）
3. **更新对应文档**（`docs/` 下的文件）
4. **保持"已知限制"诚实**——做不到的要写进去

现有的模式可以参考 `docs/FILESYSTEM.md` 的性能调优那一节：先测基线，
再定位，再改，再测，最后把无效的尝试也记下来。

