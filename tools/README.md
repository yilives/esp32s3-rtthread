# 应用本仓库的改动到上游

把 `rt-thread/` 和 `esp-idf/` 两块内容装进你本地的上游树。

```powershell
.\tools\fetch-upstream.ps1 -RtThreadPath I:\RT_thread\rt-thread -IdfPath I:\RT_thread\esp-idf-rtt
```

---

## 文件

| 文件 | 作用 |
| --- | --- |
| `env.ps1` | **共享的环境发现**：工具链、IDF 树、python venv、IDF 版本、RT-Thread 树、PATH |
| `build.ps1` | 构建（`-NoPsram` 出 QEMU 变体，`-Qemu` 构建后直接跑 QEMU）|
| `qemu.ps1` | 跑 QEMU 并做 PASS/FAIL 判定 |
| `qemu-gdb.ps1` | 起 QEMU 并冻结等 GDB 附加（`-Run` 则不冻结）|
| `gdb-bt.ps1` | attach GDB、halt、打印双核 backtrace（`-StartQemu` 先起 QEMU）|
| `fetch-upstream.ps1` | 把本仓库的改动应用到上游树 |
| `verify-tree.ps1` | **检查上游树是否只含本仓库的改动**（见下）|
| `check-docs.ps1` | **在板上跑文档里的命令，比对实际输出**（见下）|
| `verify-doc-commands.ps1` | **核对文档提到的命令是否真的导出**（见下）|
| `list-shim-api.ps1` | **列出 FreeRTOS 兼容层实际实现的 API**（见下）|
| `find-duplication.ps1` | **找出写了两遍的逻辑**（见下）|
| `soak-docs.ps1` | **单次启动内重复跑工作负载，看有无漂移**（见下）|
| `verify-doc-links.ps1` | **检查文档交叉引用是否还有效**（见下）|
| `verify-doc-facts.ps1` | **核对文档里的结构性数字**（见下）|
| `verify-doc-symbols.ps1` | **核对文档引用的符号名是否存在**（见下）|
| `expected-output.txt` | 上面那个脚本用的期望清单 |

前三者都 dot-source `env.ps1`。这不是洁癖——之前每个脚本各带一份环境代码，
它们漂移了：两份还固定着工具版本目录（`esp-15.2.0_20251204`、`cmake\4.0.3`、
`ninja\1.12.1`、`esp_develop_9.2.2_20260417`），IDF 升级一次就全废；其中一份
拼出的 PATH 少一层目录，报"编译器找不到"而它指的目录其实是存在的。

`env.ps1` 里没有任何版本号——它按**可执行文件名**搜索。

## `verify-tree.ps1`：为什么需要它

把每处未提交改动分成三类：

| 类别 | 含义 |
| --- | --- |
| `expected` | 本仓库发布或打 patch 的文件——正常 |
| `foreign` | 本仓库完全不知道的改动——要查 |
| （忽略）| 其他芯片的 port（`esp32s31` 等）、构建产物 |

**这不是洁癖。** 曾经有三个文件（`components/lwip/lwip/src/api/tcpip.c`、
`components/lwip/port/freertos/sys_arch.c`、
`components/esp_netif/lwip/esp_netif_lwip.c`）带着未提交的调试代码——
RTC_NOINIT 计数器和诊断打印，引用了 `c5tcp` 测试。它们改的内存布局足以
让测试行为变化，其中一个甚至编不过（用了 lwip 里不可见的 `rt_uint32_t`）。

我为排查一个"Guru"和一个"BLE assert"花了很长时间，而它们只存在于那棵树里。
清掉之后同一测试 6/6、Guru 0。

```powershell
.\tools\verify-tree.ps1          # 只报告
.\tools\verify-tree.ps1 -Fix     # 逐个询问是否回退
```

## `check-docs.ps1`：文档输出不能靠手抄

文档里的示例 transcript 会和固件脱节，而**抄错一个字段名就会让读者怀疑整份文档**。
上一轮手工比对发现问题（`kernel` vs `heap kernel`、`verdict` 的位置），
这一轮把它做成了脚本——**第一次跑就崩了两个真 bug**（见
[KNOWN-ISSUES](../docs/KNOWN-ISSUES.md) 第 14 条）。

```powershell
.\tools\check-docs.ps1 -List                       # 看有哪些节
.\tools\check-docs.ps1                              # 跑不需要网络的
.\tools\check-docs.ps1 -Only s3rt,s3smp             # 只跑这两节
.\tools\check-docs.ps1 -WifiSsid <ssid> -WifiPass <pass>
                                                     # 含网络节
```

**期望**：

```
  PASS  s3rt                 7 pattern(s)
  ...
RESULT: PASS - all 31 section(s) matched
```

覆盖范围包括 `s3rt` 的七个子命令（`tick`/`irq`/`psram`/`led`/`pwm`/`key`）、
`s3fs` 的四种状态转换、`s3fsbench` 的两种模式、`s3ulp` 与 `s3ulpw`、
以及 `s3wdt hang`。`led` 和 `pwm` 会点亮 LED，这是正常的副作用。

### `!` 前缀：会复位板子的节

清单里写成 `[!s3wdt hang]` 的节，其命令**会故意复位板子**。脚本对此的处理：

1. **排到最后跑**——复位之后板子是新启动的，前面各节建立的状态都没了
2. **等待 30 秒**而不是默认值（看门狗要先超时）
3. **跨复位继续捕获**——同一个串口会收到重启横幅，所以匹配式可以断言复位原因
4. **复位后重新唤出提示符**，再跑下一节

`[!s3wdt hang]` 断言的是 `rst:0x7 (TG0WDT_SYS_RST)` 和
`has been reset by WDT`。**只检查命令打印了"即将复位"是不够的**——
一个看门狗根本没接硬件的板子也能打印那句话。

清单在 `expected-output.txt`，每节是「命令名 + 若干正则」。规则写在文件头：
**只匹配该命令自己打印的行**，数值用形状不用具体数字。

### `~` 行：数值范围

正则只证明一个数字**存在**，证明不了它**合理**。`(\d+) KB/s` 在 242 和 57 上都通过：

```
s3fsbench: write  256 KB in (\d+) ms -> (\d+) KB/s
~ 1 in 700..2500
~ 2 in 100..400
```

- `~ N == V` —— 第 N 个捕获组必须**等于** V（确定性数值，如写次数）
- `~ N in LO..HI` —— 第 N 个捕获组必须在闭区间内（计时，SPI NOR 擦除时间同板抖动 40%）

区间放宽时**要说明原因**。`s3fsbench small` 的写次数区间是 145..200 而不是
固定的 152，因为它在"新格式化卷"上是 152、"用过的卷"上是 190，长序列里无法保证
是哪种——但两个都远低于调优前的 1460，所以范围仍能抓到真实回退。

**顺序很重要。** 脚本在一次启动里按清单顺序跑完所有命令。BLE 必须排在
Wi-Fi 之前——Wi-Fi 起来后 BLE 拿不到那 56 KB 连续内部 DRAM
（第 12 条）。清单的注释里写了这件事，免得下次有人重排后看到两个
"失败"却其实是同一个已知约束。

## `verify-doc-commands.ps1`：文档里的命令名要存在

不需要板子，几秒钟跑完。检查两件事：

1. 文档里每个 `msh >xxx` 对应的命令，代码里真的有 `MSH_CMD_EXPORT`
2. 代码导出的命令有没有在文档里完全没提（只报告，不算错误）

```powershell
.\tools\verify-doc-commands.ps1
```

**期望**：

```
  code exports 16 command(s)
  docs invoke  18 command(s) via 'msh >'
  Every command the docs tell a reader to type exists.
```

`free` / `list` 这类内核自带命令在白名单里，它们不来自本仓库的代码。

### 为什么需要它

`ROADMAP.md` 有一条"BLE 之后连 Wi-Fi 失败（进行中，未解决）"，在它已经被修好
**好几个版本之后**还留在那里。散文不会编译失败，没有东西会拦住它。

命令名是可以检查的那部分：如果文档让读者敲 `s3foo` 而没有这个命令，那是
脚本能抓到的事实错误。

## `list-shim-api.ps1`：兼容层到底实现了什么

不需要板子。从 `components/rtthread/port/freertos/*.c` 提取所有 FreeRTOS 风格
（`x*` / `v*`）的函数定义。

```powershell
.\tools\list-shim-api.ps1           # 按类别汇总
.\tools\list-shim-api.ps1 -All      # 全部 64 个，附文件名
.\tools\list-shim-api.ps1 -Missing  # 文档提到但没实现的
```

**实测架构**（脚本输出）：

```
freertos_api.c   933 行   64 个 FreeRTOS 函数   <- 薄的参数/类型适配层
ft_ipc.c        1231 行    0 个               <- 队列/信号量的真正实现
ft_task.c        988 行    0 个               <- 线程映射
```

`-Missing` 会把 `PORTING.md` 里提到的每个 `x*`/`v*` 名字查一遍，判定它是否
`#define` 成宏、或被 IDF 自己的 freertos 组件实现。`xTaskCreate` 是
`xTaskCreatePinnedToCore` 的宏，`xPortGetCoreID` 在 IDF 的 port 里——都不算缺失。

### 为什么需要它

`PORTING.md` 里 `FT_IRAM` 的代码块还写着 `#if defined(__XTENSA__)`，而实际
早就改成无条件了；文件行数写的是 3751，实际 4204。散文不会编译失败。

## `find-duplication.ps1`：重复逻辑在哪里

```powershell
.\tools\find-duplication.ps1                     # 两种视图都跑
.\tools\find-duplication.ps1 -Mode blocks        # 完全相同的连续代码
.\tools\find-duplication.ps1 -Mode calls         # 相同的 API 调用序列
```

两种视图互补：`blocks` 找复制粘贴，`calls` 找**改写过的**拷贝（变量名不同但
调用顺序相同）。两者都跳过 `rt_kprintf` 这类到处都是的调用。

### 为什么需要它

这个移植里有**四个 bug 形状相同**：一份逻辑写了两遍，保护只加了一份拷贝。

| bug | 一份有保护，另一份没有 |
| --- | --- |
| `s3wifi` 两次崩 | `s3wifiscan` 有 netif 检查，`s3wifi` 没有 |
| `s3net` 无 IP 崩 | 检查只覆盖两个分支中的一个 |
| `s3wifi` 重连超时 | 检查读的标志被自己的初始化清零 |
| `s3ble deinit` rc=30 | `shutdown_quiet` 有标志，`deinit` 的拷贝没有 |

工具**不找 bug**，它找的是让这些 bug 可能的重复。找到之后按"这两处应该合并"
处理，而不是"两处各修一遍"——后者正是下一次疏漏的来源。

## `soak-docs.ps1`：跑很久会不会变

```powershell
.\tools\soak-docs.ps1                        # 10 轮
.\tools\soak-docs.ps1 -Iterations 30
.\tools\soak-docs.ps1 -WifiSsid <ssid> -WifiPass <pass>
```

每轮跑一遍完整工作负载（`s3smp`、`s3heap`、`free`、`s3rt irqlat`、`s3rt psram`、
`i2cwatch`、`s3fs`、`s3fsbench small`、`s3fs read`、`s3ble`、`s3ulp`，带参数时
再加 `s3wifi` / `s3net`），然后抓一次内存快照。

**它判的不是命令输出**——那是 `check-docs.ps1` 的事。它回答的是另一个问题：
**做完全部这些 N 遍之后，板子还是不是同一个状态。**

观察三件事：

1. 内部 DRAM / PSRAM / 内核堆是否单调下降（泄漏）
2. UART 中断计数是否异常增长（中断风暴）
3. 是否出现 `Guru` / `rst:0x7` / `assert failed` / `Malloc failed`

### 实测 10 轮（约 65 分钟）

```
  iter  dram free psram free kern avail      irq     tick
  1         27348    8386120      87616      284   453147
  2         27324    8386120      87616      566   890276
  ...
  10        27324    8386120      87616     2822  4388197

  dram free    first 27348, last 27324, delta -24 bytes
  psram free   first 8386120, last 8386120, delta +0 bytes
  kernel avail first 87616, last 87616, delta +0 bytes
RESULT: PASS - 10 iteration(s), no fault markers
```

**-24 字节**出现在第 1 轮和第 2 轮之间，之后完全不动。那是首次使用时的一次性
分配（缓存或池），不是泄漏——泄漏会持续下降。

`irq` 每轮 +282 是键盘输入本身产生的中断，稳定即可。

`tick` 每轮 +393504 = 393 秒，与每轮实际耗时吻合。

## `verify-doc-links.ps1`：断链检查

```powershell
.\tools\verify-doc-links.ps1
```

**期望**：

```
  10 markdown file(s) under I:\RT_thread\esp32s3-rtthread

  KNOWN-ISSUES.md has 26 numbered entries, highest is 26

  Every cross-reference resolves.
```

检查三类引用：

| 类型 | 形式 | 判据 |
| --- | --- | --- |
| 文件链接 | `[文本](other.md)` | 目标文件必须存在 |
| 条目引用 | `第 N 条` | `KNOWN-ISSUES.md` 必须有第 N 条 |
| 章节引用 | `第 N 节` / `section N` | 被点名的那份文档必须有第 N 节 |

后两类对照**目标文档的实际标题**，而不是硬编码范围——这样目标重新编号后检查仍然准确。

### 为什么需要它

`docs/` 里现在有 26 条 `KNOWN-ISSUES`、九份文档、几十个交叉引用。每一轮都在新增、
重命名、重编号。**链接是第一个静默失效的东西**，因为除了点它的人，没有任何东西会读
一个 Markdown 链接。

### 一个 PowerShell 5.1 的坑

这个脚本的第一版**编译不过**：

```
At verify-doc-links.ps1:119 char:31
Missing ')' in method call.
```

原因不是语法写错，而是**脚本里有中文字符串字面量而文件没有 BOM**。Windows
PowerShell 5.1 会把无 BOM 的文件当 ANSI 读，UTF-8 的中文字节被拆成乱码，其中某个
字节恰好终止了字符串。

仓库禁止 BOM（它曾经破坏 `.gitattributes`），所以修法不是加 BOM，而是**脚本本身
保持纯 ASCII**：需要的中文字符用码点拼出来。

```powershell
$DI   = [char]0x7B2C   # 第
$TIAO = [char]0x6761   # 条
$JIE  = [char]0x8282   # 节
$EntryPattern = '{0}\s*(\d+)\s*{1}' -f $DI, $TIAO
```

其他脚本没有中文字面量，所以一直没撞上。这是第一个需要的，也是第一个撞上的。

## `verify-doc-facts.ps1`：文档里的数字

```powershell
.\tools\verify-doc-facts.ps1
```

**期望**：

```
  ok    compat layer, 4943 lines over 9 files 4943
  ok    freertos_api.c, 933 lines          933
  ...
  All 14 structural claim(s) hold.
```

检查的都是能机械验证的：某个文件或目录的**行数**、某个目录的**文件数**、
patch 文件触及的**路径数**。不试图验证散文、时序或需要开发板的数字——那些留在
散文里是有原因的。

### 为什么需要它

`PORTING.md` 说兼容层是 "4204 行，9 个文件"，实际是 **4943 行**（8 个 `.c` +
1 个 `.h`）。更早的版本写的是 3751。**两个数字都没人发现**，因为散文里的数字
不会编译失败。

新增数字时把它加进这个清单，这是代价也是目的。

### 写这个工具时我自己算错了两次

第一版 14 条报了 11 条 "WRONG"——全是我的计数方法错了，不是文档错了：

| 写法 | 问题 | 786 vs 933 |
| --- | --- | --- |
| `Get-Content x \| Measure-Object -Line` | **不数空行** | `freertos_api.c` 报 786，实际 933 |
| `Get-ChildItem -File` | **不递归** | `components/rtthread` 报 4，实际 22 |

改成按换行切分、并用 `-Recurse` 之后全部吻合。**一个误报 11 次的检查器比没有更糟**
——它会让人开始怀疑报告而不是代码。

## `verify-doc-symbols.ps1`：文档里引用的符号

```powershell
.\tools\verify-doc-symbols.ps1
```

提取文档里所有反引号包住的 `rt_*` / `heap_caps_*` / `esp_*` 标识符，逐个确认
它们在源码里存在。当前 42 个符号，搜 8013 个源文件（含 IDF 头文件），约 6 秒。

**期望**：

```
  42 quoted symbol(s) across 9 file(s)
  searched 8013 source file(s)

  Every quoted symbol exists in the sources.
```

### 抓到过什么

README 的支持情况表写 `| LEDC PWM | rt_pwm_device | s3rt pwm |`，而代码里是
`struct rt_device_pwm`。**同样两个词，顺序反了**——读起来完全正常，指向不存在
的东西。

`PORTING.md` 写 `port_intr.h` 提供 `rt_hw_intr_route` / `rt_hw_intr_is_enabled`，
实际第二个叫 `rt_hw_interrupt_is_masked`。

### 它不检查什么

**只证明名字在源码里出现过，不证明它是对的那一行。** PWM 那个错会被抓到；
一个"看着合理但配错了行"的名字不会被抓到。这一点写在脚本头部，免得高估它。

另外它会跳过出现在"删除/移除"语境里的符号——`PORTING.md` 说 "删除 `rt_xt_ip3`
调试探针"，那个符号**本来就不该存在**，报错是误报。

### 搜索范围

| 树 | 内容 |
| --- | --- |
| `rt-thread/bsp/ESP/ESP32_S3` | BSP |
| `rt-thread/libcpu/xtensa` | CPU port |
| `esp-idf/components/rtthread` | 兼容层 |
| `project/s3-rtt-test/main` | 应用 |
| `../rt-thread/src` + `include` | 上游内核 |
| `../esp-idf-rtt/components`（仅 `.h`）| IDF API |

最后一项只搜头文件：那棵树很大，而声明才是要找的东西。

## 参数

| 参数 | 必填 | 说明 |
| --- | --- | --- |
| `-RtThreadPath` | 是 | 你的 RT-Thread 源码树 |
| `-IdfPath` | 是 | 你的 ESP-IDF（需含 `CONFIG_IDF_RTOS_RTTHREAD`） |
| `-SkipPatches` | 否 | 只复制文件，不打 patch |
| `-KeepLocal` | 否 | 保留目标树里已修改的同名文件（默认是覆盖）|

---

## 它会做什么

1. **复制** `rt-thread/bsp/ESP/ESP32_S3/` → `<RtThreadPath>/bsp/ESP/ESP32_S3/`
2. **复制** `rt-thread/libcpu/xtensa/` → `<RtThreadPath>/libcpu/xtensa/`
3. **打补丁** `rt-thread/patches/0001-kernel-smp-hooks.patch`
4. **复制** `esp-idf/components/rtthread/` → `<IdfPath>/components/rtthread/`
5. **打补丁** `esp-idf/patches/0001-idf-core-smp-hooks.patch`
6. **打补丁** `esp-idf/patches/0002-freertos-shim-iram.patch`

---

## 安全措施

脚本会：

- **先检查**每个 patch 能否干净应用（`git apply --check`），有一个不行就整体中止
- 复制前确认目标目录存在，不存在就报错而不是创建一个可疑的路径
- **默认覆盖同名文件**。这些文件就是移植本身，覆盖是它的目的

关于最后一条：早期版本默认跳过"已存在且内容不同"的文件，只在加 `-Force` 时覆盖。
在干净检出上验证过——组件的 9 个文件（含 `ft_ipc.c`、`ft_core.h`）被静默跳过，
用户按 README 跑完会得到一个缺了核心修复的树。所以默认反过来了。

---

## 手工做

见 [docs/BUILD.md](../docs/BUILD.md) 第 2 节。

---

## 卸载

```powershell
cd <RtThreadPath>
git checkout -- src/
git clean -fd bsp/ESP/ESP32_S3 libcpu/xtensa

cd <IdfPath>
git checkout -- components/
git clean -fd components/rtthread
```

**注意**：`git clean -fd` 会删掉未跟踪的文件。执行前确认路径。
