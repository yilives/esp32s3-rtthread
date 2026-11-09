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
| `-Force` | 否 | 覆盖已存在的目标文件 |

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
- `-Force` 才会覆盖已存在的文件

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
