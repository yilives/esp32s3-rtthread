# 应用本仓库的改动到上游

把 `rt-thread/` 和 `esp-idf/` 两块内容装进你本地的上游树。

```powershell
.\tools\fetch-upstream.ps1 -RtThreadPath I:\RT_thread\rt-thread -IdfPath I:\RT_thread\esp-idf-rtt
```

---

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
