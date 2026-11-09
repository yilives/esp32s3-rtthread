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

## 性能：实测数据与调优

### 基线（1 MB 分区，4 KB 扇区）

```
s3fsbench: write  256 KB in 1070 ms -> 239 KB/s
s3fsbench: read   256 KB in 30 ms -> 8533 KB/s
s3fsbench: create 32 x 64 B in 8539 ms (3 files/s, 337 ms each)
s3fsbench: delete 32 files in 6277 ms (196 ms each)
```

**340 倍写放大**：一个 64 字节文件消耗 21.8 KB flash。

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

效果：写次数 1168 → 152（**-87%**），但**时间几乎没变**（5483 → 5297 ms），
因为瓶颈在擦不在写。

#### 优化 2：FAT 缓冲从 PSRAM 移到内部 RAM（-19%）

`CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y` 时 FAT 缓冲放 PSRAM。Kconfig 原文：

> Disable this option if optimizing for performance.

关闭后：6740 → 5483 ms，**写次数不变**。纯时间收益。

#### 优化 3：单份 FAT（-21%）

`use_one_fat = true`，省掉每次元数据更新时写第二份 FAT 的 4 KB：

8539 → 6740 ms，写次数 1460 → 1168。

**代价**：单份 FAT 撕裂后无法修复，只能 `s3fs erase`。但这里存的是应用数据不是固件，
且 WL 层本身有备用块和位置更新记录，双份 FAT 的边际价值很低。

### 试过但无收益（已回退）

| 尝试 | 结果 |
| --- | --- |
| 512 字节扇区 | 批量写 82351 ms（**71 倍变慢**）—— 4 KB 物理页要 RMW |
| 8 KB 簇 | 5316 vs 5297 ms，无收益（每文件仍占 1 扇区） |
| 关闭 LFN（仅 8.3） | 5461 vs 5483 ms，几乎无收益，代价太大 |
| max_files 5 → 8 | 无明显变化（保留） |

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
