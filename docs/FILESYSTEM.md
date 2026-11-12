# 文件系统

FAT，跑在 ESP-IDF 的 VFS + FATFS + wear_levelling 之上。

---

## 为什么是 FAT 而不是 littlefs

两个代码树里**都没有 littlefs**：

- `esp-idf-rtt/components/` 只有 `fatfs`、`spiffs`、`vfs`
- IDF 的 `examples/storage/littlefs` 靠 `idf_component.yml` 从组件仓库拉
  `joltwallet/littlefs`
- RT-Thread 侧需要 `pkgs --update` 联网

而 `libfatfs.a` + `libwear_levelling.a` + `libvfs.a` 本来就在镜像里，39 KB `.text`，
零依赖。

**取舍**：

| | FAT | littlefs |
| --- | --- | --- |
| 掉电安全 | ❌ 无原子提交 | ✅ copy-on-write |
| 小文件效率 | ❌ 4 KB 簇 / 64 字节文件 | ✅ 按需分配 |
| PC 可读 | ✅ 直接读 | ❌ 需专用工具 |
| 依赖 | 零 | 需联网拉组件 |

**结论**：除非有强掉电安全需求，否则 FAT 更划算。见 README 的对比表。

---

## 分区

```
storage,  data, fat,  0x310000,  0x100000     # 1 MB
```

1 MB / 4 KB 扇区 = 256 簇 → **FAT12**（`ff.c:40` `MAX_FAT12 = 0xFF5 = 4085`）。

含义：**一个 100 字节的配置文件占 4 KB**，1 MB 分区最多 256 个文件。

---

## 用法

| 命令 | 作用 |
| --- | --- |
| `s3fs` | 挂载 + 写 511 字节 + 读回校验 |
| `s3fs read` | 只读（**这才是持久性证明**） |
| `s3fs erase` | 整片擦除（修复被中断的 format） |
| `s3fs format` | 显式格式化（需先 unmount） |
| `s3fs unmount` | 卸载 |
| `s3fsbench [write\|read\|small]` | 性能基准 |

### 为什么开机挂载

`INIT_APP_EXPORT` 里挂载，**在 Wi-Fi / BLE 起来之前**。

不是图方便——是唯一安全的顺序。两个无线栈都会从自己的任务里写 NVS，
而 flash 操作不能和它们共存：

- 运行时格式化 → 双核在 cache 握手里死锁，或
- FAT 半写 → 下一次 `fopen` 返回 `EACCES`（errno 13，看起来像权限问题）

---

## 掉电保护

### 原来的问题

`format_if_mount_failed = true` 是 IDF 的默认值。它的语义是"挂载失败就格式化"。

这个默认值**会静默销毁数据**：一次被看门狗打断的 format 会留下一个
"能挂载但每次 `fopen` 都失败"的卷，下次开机会被无提示地擦掉。

### 试错的方案

**方案 1（错误）：检测物理 offset 510 的 0x55AA 签名**

```c
esp_partition_read(part, 510, sig, 2);   // 读的是物理地址
return (sig[0] == 0x55 && sig[1] == 0xAA);
```

**为什么错**：offset 510 在分区内部，读到的是 **wear_levelling 的块头**，
不是 FAT 引导扇区。逻辑扇区 0 被 WL 映射到别处了。

实测：用 esptool 擦掉前 8 KB 后，卷仍然正常挂载工作——说明探测点找错了。

**方案 2（采用）：有状态判断**

```c
.format_if_mount_failed = !s_fs_mounted_once,
```

- 开机第一次挂载失败 → 允许格式化（空分区必须能格式化）
- 已经挂载成功过 → 失败就**报错**并提示 `s3fs erase`

代价：判断不了"这次开机第一次挂载失败，但其实有旧数据"。这是有意的取舍——
比静默擦掉好。

---

## 测量条件

本文所有时间数字都是在**这一套条件下**测的。改了任何一项，数字都会变，
需要重测：

| 条件 | 值 |
| --- | --- |
| 芯片 | ESP32-S3 rev v0.2，240 MHz |
| Flash | 8 MB，80 MHz，**DIO**（bootloader 随后切到 QIO） |
| PSRAM | 8 MB octal，80 MHz |
| **优化级别** | **`-Og`**（IDF `CONFIG_COMPILER_OPTIMIZATION_DEBUG`，RT-Thread 侧 `rtconfig.py` 显式 `-Og`） |
| tick | 1000 Hz |
| MMU 页 | 64 KB |
| 构建 | RT-Thread 5.4.0，ESP-IDF 6.1 |
| 测量时无线 | 已停（`s3fsbench` 会主动停） |

**优化级别是最容易被忽略的一项。** 当前是 `-Og`，不是 `-O2`：这是个调试构建，
所有断言都开着。如果换到 `-O2`，文件系统的时间数字需要重测。

> **优化级别的影响经过实测，两个方向都是小的。**

中断延迟：

| 构建 | 中断延迟 |
| --- | --- |
| `-Og` | 434..480 cycles |
| `-O2` | 423..471 cycles |

差 2.5%。ISR 入口是硬件跳转加固定代码，优化空间本来就小。

文件系统（两次都是**新格式化的卷**，同一块板子，同一套配置）：

| 指标 | `-Og` | `-O2` | 差 |
| --- | --- | --- | --- |
| 建 32 小文件 | 8766 ms | 8793 ms | +0.3% |
| 覆写一文件 8 次 | 2689 ms | 2714 ms | +0.9% |
| 删除 32 文件 | 4504 ms | 4388 ms | -2.6% |
| 写次数 | 152 | 152 | 0 |
| 擦除次数 | 136 | 136 | 0 |

**优化级别对这个负载几乎没有影响**，原因很直接：时间全花在 SPI NOR 擦除的物理
等待上，不是 CPU 计算。写次数和擦除次数完全一致，因为它们由 FAT 算法决定。

> **这里有一个我自己踩过的坑值得记。** 第一次在 `-O2` 下测到 8832 ms，我对比
> 文档里更早的 5281 ms，以为优化级别让文件系统变慢了 67%。实际上那两个数字来自
> **不同状态的卷**——5281 是文档早期一个尚未被后续实验改动的测量。做了受控对比
> 之后才知道两者相差不到 1%。**混在一起的变量会让结论完全反向。**

## 性能：实测数据与调优

### 基线（1 MB 分区，4 KB 扇区）

> **这些数字必须在新格式化的卷上测。** 同一个 `s3fsbench small` 在一个
> 已经用过一段时间的卷上是 190 writes / 170 erases / 6989 ms，在新卷上是
> 152 / 136 / 5374 ms —— 差 20% 以上。目录项分散程度影响 FAT 更新次数，
> 而 FAT 更新次数就是写放大。
>
> 所以复现时先 `s3fs unmount` + `s3fs erase` + `s3fs`，再跑基准。
> 这也解释了为什么不同时间测的数字会有出入：不是性能漂移，是卷的历史不同。
>
> **计数是确定性的，时间是波动的。** 四次全新卷测量：
>
> | 指标 | 四次结果 |
> | --- | --- |
> | writes / erases | 152 / 136（四次完全相同）|
> | create 32 小文件 | 5374 / 5458 / 7379 / 5281 ms |
> | overwrite 8x | 2341 / 2248 / … ms |
>
> 写次数完全可复现，因为它由 FAT 和 WL 的算法决定；时间是 SPI NOR 物理擦除
> 时间的波动，同一台设备上能差 40%。**看趋势用计数，看绝对性能才用时间。**


```
s3fsbench: write  256 KB in 1070 ms -> 239 KB/s
s3fsbench: read   256 KB in 30 ms -> 8533 KB/s
s3fsbench: create 32 x 64 B in 8539 ms (3 files/s, 267 ms each)
s3fsbench: delete 32 files in 6277 ms (196 ms each)
```

**340 倍写放大**：一个 64 字节文件消耗 21.8 KB flash。

> 这是**优化前**的数字。152 writes / 32 个文件 -> 4.75 次每文件，即 17 KB / 272 倍。两个数字都对，属于不同阶段：本节讲的是基线。
>
> `267 ms each` —— 这块原来写的是 `337 ms each`，与 `8539 / 32 = 267` 对不上：
> 337 乘 32 是 10784 ms，不是 8539。**错的是文档里的这个数，不是代码**——当前固件
> 自己算这一列是对得上的（实测 `8766 ms (274 ms each)`，`8766 / 32 = 274`）。

### 定位过程

**第 1 步：拆分 open / write / close**

```
bench: file  0 open=1 write=0 close=217 ms
bench: file  1 open=0 write=0 close=270 ms
```

**100% 开销在 `fclose`**。

**第 2 步：对照实验**

| 操作 | 时间 | 写 | 擦 |
| --- | --- | --- | --- |
| 覆写同文件 8 次（带 fclose） | 3586 ms | 446 | 59 |
| 同上但不 fclose | 991 ms | 145 | 16 |

**第 3 步：追到 IDF 源码**

`fatfs/diskio/diskio_wl.c:50`：

```c
esp_err_t err = wl_erase_range(wl_handle, ...);   // 先擦
err = wl_write(wl_handle, ...);                    // 再写
```

**每次 FAT 写扇区都先触发一次 `wl_erase_range`**，而它调用
`WL_Flash::erase_sector() → updateWL()` 触发整块搬迁。

这是 IDF 的设计，`CONFIG_FATFS_*` 里没有任何开关能改。

### 三个有效优化

#### 优化 1：WL 暂存缓冲 32 → 4096（写次数 -90%）

`WL_Flash::updateWL()` 搬迁 4 KB 块时要经过 `wl_temp_buff_size` 分段读写，
默认 32 字节 = **128 次读+写对**。

**不改 IDF 源码**——`WL_DEFAULT_TEMP_BUFF_SIZE` 有 `#ifndef` 保护：

```cmake
idf_component_get_property(wl_lib wear_levelling COMPONENT_LIB)
target_compile_definitions(${wl_lib} PRIVATE WL_DEFAULT_TEMP_BUFF_SIZE=4096)
```

A/B 实测（改一个编译宏，重编，烧写，各自空卷）：

| | `WL_DEFAULT_TEMP_BUFF_SIZE` |
| --- | --- |
| | **32**（IDF 默认）| **4096** |
| writes | 1168 | **152** |
| erases | 136 | 136 |
| create 32 小文件 | 5871 ms | 5791 ms |

写次数 **-87%**，时间几乎不变（差 1.4%）——因为瓶颈在擦不在写。

**这个结果值得想一想**：写次数少了 7.7 倍，时间只差 1.4%。如果目标是减少
flash 磨损，这个优化是决定性的；如果目标是速度，它什么也没带来。

#### 优化 2：FAT 缓冲从 PSRAM 移到内部 RAM（-19%）

`CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y` 时 FAT 缓冲放 PSRAM。Kconfig 原文：

> Disable this option if optimizing for performance.

A/B 实测：

| | FAT 缓冲位置 |
| --- | --- |
| | **PSRAM**（`=y`）| **内部 RAM**（未设）|
| writes | 152 | 152 |
| create 32 小文件 | 6943 ms | **5626 ms** |
| delete 32 files | 4458 ms | 3764 ms |
| overwrite 8x | 2706 ms | 2396 ms |

**-19%**，写次数不变——纯时间收益。和优化 1 正好相反：那个减写不减时，
这个减时不减写。两个都做，才有可能同时改善磨损和速度。

#### 优化 3：单份 FAT（-21%）

`use_one_fat = true`，省掉每次元数据更新时写第二份 FAT 的 4 KB：

8539 → 6740 ms，写次数 1460 → 1168。

**代价**：单份 FAT 撕裂后无法修复，只能 `s3fs erase`。但这里存的是应用数据不是固件，
且 WL 层本身有备用块和位置更新记录，双份 FAT 的边际价值很低。

### 三个优化各自改善什么

三个优化都做过 A/B（改一处配置、重编、烧写、各自空卷各测一次），它们改善的
不是同一件事：

| 优化 | 写次数 | 时间 | 性质 |
| --- | --- | --- | --- |
| 1. WL 暂存缓冲 32 → 4096 | **-87%** | -1.4% | 减磨损，不减时间 |
| 2. FAT 缓冲移出 PSRAM | 不变 | **-19%** | 减时间，不减磨损 |
| 3. 单份 FAT | **-20%** | **-24%** | 两者都减 |

这个区分很重要：如果只做了优化 1，会得到"写次数降了 87% 但一点没变快"的
结论，容易误以为优化没用；如果只做了优化 2，磨损完全没改善。

**优化 1 的写次数减少有实际价值**（SPI NOR 有擦写寿命），**优化 2 的时间收益
有实际价值**，两者不能互相替代。

### 试过但无收益（已回退）

| 尝试 | 结果 |
| --- | --- |
| 512 字节扇区 | 批量写 82351 ms（**71 倍变慢**）—— 4 KB 物理页要 RMW |
| 8 KB 簇 | 5316 vs 5297 ms，无收益（每文件仍占 1 扇区） |
| 关闭 LFN（仅 8.3） | 5461 vs 5483 ms，几乎无收益，代价太大 |
| max_files 5 → 8 | 无明显变化（保留） |

> **这一表的数字来自当时的配置，改配置才能复现。** 尤其是 512 字节扇区那一行
> —— 它需要把 `CONFIG_WL_SECTOR_SIZE` 改成 512 并重新烧写，不是运行期可切换的。
> 而这套配置已经被否定了，所以当前构建里**无法**重新验证 82351 ms。
>
> 保留它的理由：它解释了为什么默认用 4 KB 扇区（SPI NOR 的物理页是 4 KB，
> 512 字节逻辑扇区意味着每个 4 KB 页要做读-改-写，成本是线性的 8 倍加上
> 擦除，实测 71 倍）。**这个结论比具体数字重要**，数字只说明量级。

### 关于 152 和 190 这两个写次数

`s3fsbench small` 的写次数有两个值：

| `use_one_fat` | writes | erases | create 32 小文件 |
| --- | --- | --- | --- |
| `true`（当前） | **152** | **136** | 5506 ms |
| `false` | **190** | **170** | 7249 ms |

**这就是优化 3 的效果，A/B 实测**：改一行配置、重编、烧写、各自在空卷上测。

```
use_one_fat = true   -> 152 writes / 136 erases / 5506 ms
use_one_fat = false  -> 190 writes / 170 erases / 7249 ms
```

差 20% 的写次数和 24% 的时间。

> **一次更正（两次修订）。** 本文档曾经说 152 是"新格式化卷"的数字、190 是
> "用过的卷"的数字。那是错的——实测在 `use_one_fat=true` 下，四种卷状态
> （刚格式化 / 同卷再跑 / 跑过 bulk write / 跨重启有遗留文件）**全部是 152/136**。
>
> 190/170 的真正来源是**卷是怎么创建的**：
>
> | 创建方式 | writes | 原因 |
> | --- | --- | --- |
> | `s3fs erase` → `s3fs`（自动格式化）| **152** | 用 `s3_fs_mount()` 的 mkfs 参数 |
> | `s3fs erase` → `s3fs format` → `s3fs` | **190** | `s3fs format` 当时用 IDF 默认参数（双 FAT）|
>
> 后者是 bug，已修（见 [KNOWN-ISSUES.md](KNOWN-ISSUES.md) 第 21 条）。现在两条
> 路径都给 152。
>
> 而 `use_one_fat = false` 确实会给 190/170——A/B 实测过：
>
> | 配置 | writes | erases | create 32 小文件 |
> | --- | --- | --- | --- |
> | `use_one_fat = true` | 152 | 136 | 5506 ms |
> | `use_one_fat = false` | 190 | 170 | 7249 ms |
>
> **两个不同原因给出同一个数字**，这是第一次归因错的根源。

### 那么 bulk write 的 958 vs 2312 ms 呢

那个差异是**真的**，而且与写次数无关：

| 顺序 | bulk write |
| --- | --- |
| 新卷 → 直接 `s3fsbench write` | 958 ms / 267 KB/s |
| 新卷 → `s3fsbench small` → `s3fsbench write` | 2312 ms / 110 KB/s |

写次数两种情况都是 79/71。多出来的时间是分配器在碎片化的空闲链表里找块。

这个仍然是"两个基准会互相干扰"，但**影响的只有时间，不影响写次数**。

### 最终数据

| 指标 | 优化前 | 优化后 | 改善 |
| --- | --- | --- | --- |
| 建 32 小文件 | 8539 ms | 5281 ms | -38% |
| 覆写一文件 8 次 | 3586 ms | 2194 ms | -39% |
| 删除 32 文件 | 6277 ms | 3512 ms | -44% |
| 写操作次数（32 文件） | 1460 | 152 | **-90%** |
| 擦除次数（32 文件） | 170 | 136 | -20% |
| 批量写 256 KB | 1070 ms | 1064 ms | 持平 |

**擦除次数三次重复完全一致（136/136/136）、写次数完全一致（152/152/152）**。
时间有 5-7 秒抖动，来自 SPI NOR 擦除的物理时间波动。

> **"优化后"那一列的时间要打折扣看。** 5281 ms 是四次测量（5374 / 5458 / 7379 /
> 5281）里**最快的一次**，不是中位数。后来独立重测（新格式化卷、同一镜像）得到
> **8766 ms**，比 5281 慢 66%，接近"优化前"的 8539。
>
> 这不是优化失效——**写次数和擦除次数（152 / 136）在每次测量中都完全一致**，
> 那才是优化真正改变的东西，而且它有确定性。时间值的抖动来自 SPI NOR 擦除的
> 物理耗时，同一台设备上能差 40%。
>
> 所以这张表最值得信的是中间两行（次数），不是上下两行（时间）。

### 无线栈起来之后

`s3fsbench` 会自动停掉 Wi-Fi 和 BLE 再测，所以上面的数字是"无线空闲"的基线。
如果比较的是"无线在跑"的场景，实测：

| 场景 | 256 KB 批量写 |
| --- | --- |
| 无线空闲 | 1045 ms (244 KB/s) |
| Wi-Fi + BLE 曾经起来过 | 4379 ms (58 KB/s) |

慢的那一次并不是无线在竞争 CPU——测量期间无线已经停了。差别来自
`esp_wifi_stop()` 之后 PHY/coex 留下的状态让每次 flash 窗口的 IPC 握手变慢。
数字本身不矛盾，只是**测量前要先说清基线是哪个**。

顺带：如果把 `s3fsbench` 的 suspend 去掉，无线在跑时它会在 5 秒处被看门狗复位
（`rst:0x7`），一个数字都拿不到。原因见
[KNOWN-ISSUES.md](KNOWN-ISSUES.md) 第 2 条。

### 仍然存在的瓶颈

`fclose` 一次要 5 次扇区写：目录项 + FAT + 数据簇 + 空簇初始化 + 根目录扩展。
每次写带来一次 4 KB 物理擦除。**这是 IDF `diskio_wl` 的设计约束，不改 IDF 源码无法消除。**

要真正解决：

1. **应用层批量提交**——把多个小文件的 `fclose` 合并，减少 `f_sync` 次数。纯应用代码。
2. **换 littlefs**——copy-on-write 天然没有这个问题，但要引入依赖。

---

## 恢复坏掉的分区

```
msh >s3fs unmount
msh >s3fs erase
（复位板子，开机挂载会重新格式化）
```

**症状对照**：

| 症状 | 原因 |
| --- | --- |
| `fopen` 返回 `EACCES`（errno 13） | FAT 半写（format 被中断） |
| `mount failed 0x105` | 分区找不到，检查分区表 |
| `Storage` 显示但文件都在 | 正常 |
| 每次开机 `Formatting FATFS partition` | 卷一直挂不上，`s3fs erase` |

---

## 配置项一览

`project/s3-rtt-test/sdkconfig` 里和文件系统相关的：

```
CONFIG_FATFS_SECTOR_4096=y          # 逻辑扇区 4 KB
CONFIG_WL_SECTOR_SIZE=4096          # wear_levelling 物理页 4 KB
CONFIG_FATFS_PER_FILE_CACHE=y       # 每个打开的文件独占缓冲（耗 RAM 但快）
CONFIG_FATFS_LFN_HEAP=y             # 长文件名，缓冲在堆上
# CONFIG_FATFS_ALLOC_PREFER_EXTRAM is not set   # 缓冲放内部 RAM（快 19%）
```

`s3fs.c` 里：

```c
#define S3FS_MAX_FILES  8           // 同时打开的文件数（每个带 4 KB 缓冲）
.allocation_unit_size = CONFIG_WL_SECTOR_SIZE,   // 簇 = 1 扇区
.use_one_fat = true,                             // 单份 FAT
.format_if_mount_failed = !s_fs_mounted_once,    // 只在第一次挂载时格式化
```
