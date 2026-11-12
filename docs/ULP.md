# ULP-RISC-V 协处理器

---

## 先纠正一个前提

**ESP32-S3 没有 LP core。**

```c
/* components/soc/esp32s3/include/soc/soc_caps.h */
#define SOC_ULP_SUPPORTED               1
#define SOC_ULP_FSM_SUPPORTED           1
#define SOC_RISCV_COPROC_SUPPORTED      1
/* SOC_LP_CORE_SUPPORTED —— 未定义 */
```

`SOC_LP_CORE_SUPPORTED` 是 C6 / H2 / P4 的特性。S3 有的是 **ULP-RISC-V**：

| 项 | 值 |
| --- | --- |
| 类型 | RISC-V RV32，独立核 |
| 位置 | RTC 域 |
| 频率 | 20 MHz |
| 内存 | 8 KB RTC slow memory（`CONFIG_ULP_COPROC_RESERVE_MEM` 决定给程序多少） |
| 工具链 | `riscv32-esp-elf-gcc`（**不是** Xtensa） |
| 与主核通信 | RTC slow memory + 中断 |

---

## 为什么"移植"几乎不需要改代码

因为 ULP 程序**不跑在 RT-Thread 上**：

- 另一个 ISA（RV32 vs S3 的 Xtensa）
- 链接 `ulp_riscv.ld`，不是 RT-Thread 的链接脚本
- 由 IDF 的 `ulp_embed_binary()` 编译并嵌入主镜像
- `libcpu/xtensa`、调度器、FreeRTOS shim 全部不参与

主机侧 API 也没有 FreeRTOS 依赖：

| API | 实现 |
| --- | --- |
| `ulp_riscv_load_binary()` | `hal_memcpy` 到 `RTC_SLOW_MEM` |
| `ulp_riscv_run()` | 配置 `RTC_CNTL` 寄存器 |
| `ulp_set_wakeup_period()` | 同上 |
| `ulp_riscv_isr_register()` | `rtc_isr_register` → `esp_intr_alloc` |

所以真正要做的只有两件事：**接进构建系统**，**设计一套 RT-Thread 能用的接口**。

---

## 构建接入：两个坑

### 坑 1：riscv32 工具链不在 PATH

ULP 的构建是**嵌套的 CMake 项目**（`components/ulp/cmake/`），它用 `find_program`
找 `riscv32-esp-elf-gcc`：

```
CMake Error: The CMAKE_C_COMPILER:
  riscv32-esp-elf-gcc
is not a full path and was not found in the PATH.
```

`tools/build.ps1` 的 `$pathParts` 必须包含：

```powershell
"$tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin",
```

### 坑 2：`ulp` 必须是 `REQUIRES` 而不是 `PRIV_REQUIRES`

`main/CMakeLists.txt`：

```cmake
idf_component_register(... REQUIRES ulp PRIV_REQUIRES rtthread esp_rom ...)
```

**为什么**：`ulp_embed_binary()` 把本组件的 `INTERFACE_INCLUDE_DIRECTORIES` 传给嵌套构建：

```cmake
-DCOMPONENT_INCLUDES=$<TARGET_PROPERTY:${COMPONENT_TARGET},INTERFACE_INCLUDE_DIRECTORIES>
```

而 `PRIV_REQUIRES` **不填充这个属性**。放在 PRIV 里的结果是 ULP 程序找不到四个头文件：

```
ulp_riscv_uart.c:8:10: fatal error: ulp_riscv.h: No such file or directory
ulp_riscv_lock.c:6:10: fatal error: ulp_riscv_lock.h: No such file or directory
ulp_riscv_touch.c:11:10: fatal error: hal/touch_sens_types.h: No such file or directory
ulp_riscv_i2c.c:13:10: fatal error: hal/i2c_ll.h: No such file or directory
```

（这四个源文件是 `ulp_apply_default_sources()` 无条件全编的，即使你不用它们。）

### CMake 片段

```cmake
if(CONFIG_ULP_COPROC_ENABLED AND CONFIG_ULP_COPROC_TYPE_RISCV)
    set(ulp_app_name ulp_main)
    set(ulp_riscv_sources "ulp/s3ulp_main.c")
    set(ulp_exp_dep_srcs "s3ulp.c")
    ulp_embed_binary(${ulp_app_name} "${ulp_riscv_sources}" "${ulp_exp_dep_srcs}")
endif()
```

产物：

| 文件 | 内容 |
| --- | --- |
| `ulp_main.bin` | 262 字节，嵌入主镜像 |
| `ulp_main.h` | 导出 ULP 的全局变量（`extern uint32_t ulp_s3ulp_cmd;` 等） |
| `ulp_main.ld` / `ulp_riscv.ld` | 链接脚本 |

---

## 协议设计

### 为什么不能直接"调度" ULP

ULP 没有 TCB、没有优先级、没有 DRAM 栈、不能调内核 API。它不进就绪队列，
不能被抢占，不能阻塞在信号量上。

它能做的只有一件事：**给主核发中断**。

所以"让 RT-Thread 调度 ULP"的唯一实现是把它变成**异步事件源**：

```
ULP（RTC 域 20MHz）              主核（Xtensa + RT-Thread）
─────────────────               ─────────────────────────
poll s3ulp_seq
干活 -> s3ulp_result
s3ulp_ack++
ulp_riscv_wakeup_main_processor()
        ───────────────────────> RTC_CNTL_SW_CPU_INT
                                 rtc_isr (ETS_RTC_CORE_INTR_SOURCE)
                                 s3_ulp_isr
                                 完成计数器 ++
                                 阻塞的线程观察到 -> 恢复
```

### 共享变量

```c
/* ulp/s3ulp_main.c，通过 ulp_main.h 导出 */
volatile uint32_t s3ulp_magic;      /* 启动时写一次 */
volatile uint32_t s3ulp_cmd;        /* 主 -> ULP：做什么 */
volatile uint32_t s3ulp_seq;        /* 主 -> ULP：第几次请求（单调） */
volatile uint32_t s3ulp_ack;        /* ULP -> 主：完成次数 */
volatile uint32_t s3ulp_result;     /* ULP -> 主：结果 */
```

### 握手用序号，不用命令值

```c
/* 错误 */
if (cmd != handled) { ... }

/* 正确 */
if (seq != handled_seq) { ... }
```

**为什么**：比较命令值只在每个不同值上工作一次。同一个命令连发两次，第二次
`cmd == handled`，ULP 静默不响应。

实测：worker 线程循环发 `CMD_COUNT`，**第一次之后全部超时**。看起来像中断坏了。

序号还顺带修掉一个真实竞态：主核在 ULP 读完请求、还没记录之间更新请求，
用命令值比较无法区分这个转换和空闲状态。

### 发送顺序

```c
s_ulp_seq++;
ulp_s3ulp_cmd = cmd;      /* 先写命令 */
ulp_s3ulp_seq = s_ulp_seq; /* 后写序号 */
```

序号必须**最后**写，否则 ULP 可能看到新序号配旧命令。两个都是单个对齐 32 位写入，
在两侧各自原子。

---

## IRAM 约束 —— 最重要的设计决策

### 问题

`rtc_module.c:100`：

```c
err = esp_intr_alloc(ETS_RTC_CORE_INTR_SOURCE, ESP_INTR_FLAG_IRAM, &rtc_isr, NULL, ...);
```

`ESP_INTR_FLAG_IRAM` 意味着**这个 handler 可能在 flash cache 关闭时运行**。

而 `rt_sem_release` 在这个镜像里位于 flash：

```
$ nm s3_rtt_test.elf | grep " T rt_sem_release"
4200bc6c T rt_sem_release
```

在 ISR 里调它就是本项目已经修过四次的同一类 bug（`critical.c`、`atomic_xtensa.c`、
`freertos_api.c`、`ft_ipc.c`）。

### 我最初的错误

第一版写了"ISR 里直接 `rt_sem_release`，保守但可用"。

**这不是保守，是错的。** 它只是把崩溃推迟到某次 flash 写和 ULP 中断重叠的时候。

### 最终方案

ISR 只做最小且安全的事：

```c
static void IRAM_ATTR s3_ulp_isr(void *arg)
{
    s_ulp_completions = s_ulp_completions + 1;   /* 单个对齐 32 位写 */
    s_ulp_isr_count = s_ulp_isr_count + 1;
}
```

等待方每 tick 轮询一次：

```c
for (;;) {
    if (s_ulp_completions != seen) { ...return RT_EOK; }
    if (timeout) return -RT_ETIMEOUT;
    rt_thread_mdelay(1);
}
```

**代价**：轮询间隔是 1 tick（1000 Hz，即 1 ms），加上线程被重新调度的开销。

十次连续测量的实测值：

```
520  605  789  882  939  967  1173  1238  1339  1713   (µs)
```

中位数约 950 µs，最坏 1713 µs —— 比一个 tick 长，因为被唤醒的线程还要等
调度器把它排上。命令会把每次的实测值打印出来，方便观察。

> **一次更正。** 这里原本写"102–1053 µs"，那是一次测量的结果，不是区间。
> 十次采样里 8 次超出 1053。写成区间却没有说明样本量，读者会以为那是上界。

#### 双峰分布：还有一个 100–300 µs 的模式

如果 ULP 周期**已经在跑**（比如刚被 `s3ulpw` 连续 kick 过，或者紧接着上一条
`s3ulp`），延迟会变成 100–300 µs：

| 场景 | 延迟 |
| --- | --- |
| 典型单次调用（ULP 空闲，需要启动一个周期）| 524–1711 µs |
| 紧接 `s3ulpw`（ULP 周期已在运行）| 107 / 296 µs |

不是测量错误，是**测量语义**：`s3ulp_kick()` 测的是"从提交命令到观察到完成
计数变化"，而 ULP 的周期本身约 2 ms。命令提交时如果周期正在跑，观察点自然
靠前。

**判断标准**：两种模式都算 PASS。真正的问题是延迟**接近 timeout** 或
`isrs` 不增长——那说明中断没到。

**这是可测量的取舍，不是隐藏的开销。**

---

## 暴露的 API

```c
rt_err_t s3ulp_kick(rt_uint32_t cmd, rt_uint32_t *result, rt_int32_t timeout_ms);
```

线程调用 → 挂起 → ULP 在独立核上跑 → 中断到达 → 线程带结果恢复。

### 应用示例

```c
#include <rtthread.h>

extern rt_err_t s3ulp_kick(rt_uint32_t cmd, rt_uint32_t *result, rt_int32_t timeout_ms);

static void my_sensor_thread(void *arg)
{
    for (;;)
    {
        rt_uint32_t reading = 0;

        /* 让 ULP 采一次样，期间本线程挂起 */
        if (s3ulp_kick(1 /* CMD_COUNT */, &reading, 500) == RT_EOK)
        {
            rt_kprintf("sensor: 0x%08x\n", reading);
        }

        rt_thread_mdelay(1000);
    }
}
```

多线程可以同时调用——`s3ulp_kick` 内部没有加锁，但**当前只支持单请求串行**：
两次并发调用会互相覆盖 `ulp_s3ulp_cmd`。要支持并发需要给 ULP 侧加请求队列
（RTC 内存只有 8 KB，能放几十个条目）。

---

## 命令

| 命令 | 作用 |
| --- | --- |
| `s3ulp` | 提交两个命令并阻塞等待，验证完整往返 |
| `s3ulp start` | 显式加载并启动 |
| `s3ulp stop` | **验证**停止生效（计数器冻结） |
| `s3ulpw` | 启动观察用的 worker 线程 |
| `s3ulpw stop` | 停止并报告累计 kick 次数 |

### `s3ulp` 的期望输出

```
s3ulp: loading 262 bytes into RTC slow memory
s3ulp: started, wakeup 1000 us, ISR attached
s3ulp: submitting CMD_COUNT, blocking...
s3ulp: PASS, ULP completed in 102 us, result=0x2faeb9e0, isrs=1
s3ulp: PASS, CMD_DOUBLE returned 2 x CMD_COUNT (0x5f5d73c0)
```

三个关键点：

1. `ULP completed` —— 真的执行了并回了中断
2. `result` 非零 —— 结果从 RTC 内存读回来
3. `CMD_DOUBLE == 2 x CMD_COUNT` —— 结果确实来自 ULP 计算，不是固定缓冲

### `s3ulpw` 的验证证据

```
msh >s3ulpw
msh >list thread

ulpw   N/A  2  15  suspend  0x00000130  0x00000800  17%  0x0000000a  EINTRPT
                   ^^^^^^^                                  ^^^^^^^
                   suspend 状态                             被中断唤醒
```

- `suspend` —— 线程真的挂起了，不是忙等
- `EINTRPT` —— 被中断唤醒，不是超时
- 连续 **1675 次** kick 全部成功

---

## 代码位置

| 文件 | 内容 |
| --- | --- |
| `project/s3-rtt-test/main/ulp/s3ulp_main.c` | ULP 程序（RV32，5 KB） |
| `project/s3-rtt-test/main/s3ulp.c` | 主机侧（18 KB） |
| `project/s3-rtt-test/main/CMakeLists.txt` | `ulp_embed_binary()` 调用 |

---

## 已知限制

| 限制 | 原因 |
| --- | --- |
| 单请求串行 | 共享变量只有一组，并发调用会覆盖 |
| 无深睡唤醒 | `CONFIG_PM_ENABLE` 关闭，未实现 `esp_deep_sleep_start()` 路径 |
| ISR 不释放信号量 | IRAM 契约（见上） |
| 只有 timer 唤醒源 | GPIO 唤醒需要接外部按钮，BOOT 键已被 `s3rt key` 占用 |

### 想要真正的零延迟唤醒？

两条路：

1. **把 `rt_sem_release` 搬进 IRAM** —— 它深入对象系统，不现实
2. **验证 RTC ISR 触发时 flash cache 一定开着** —— RTC 中断不走 flash 访问路径，
   理论上安全。如果成立，可以在 ISR 里直接 `rt_sem_release`，延迟降到几微秒

第 2 条需要做实验：在 FAT 格式化期间高频触发 ULP 中断，看是否崩溃。
当前没有做。
