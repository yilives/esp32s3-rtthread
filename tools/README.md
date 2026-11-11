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
