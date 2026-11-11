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

**顺序很重要。** 脚本在一次启动里按清单顺序跑完所有命令。BLE 必须排在
Wi-Fi 之前——Wi-Fi 起来后 BLE 拿不到那 56 KB 连续内部 DRAM
（第 12 条）。清单的注释里写了这件事，免得下次有人重排后看到两个
"失败"却其实是同一个已知约束。

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
