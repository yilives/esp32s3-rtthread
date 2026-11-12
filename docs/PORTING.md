# 移植说明

这份文档讲**怎么做出来的**，不是怎么用。想看用法去看 [TUTORIAL.md](TUTORIAL.md)。

---

## 整体结构

```
            ┌─────────────────────────────────────┐
            │        应用（s3-rtt-test）           │
            └─────────────────────────────────────┘
            ┌─────────────────────────────────────┐
            │      ESP-IDF 组件                    │
            │  Wi-Fi  BLE  lwIP  NVS  FATFS       │
            │  它们调用 FreeRTOS API               │
            └──────────────┬──────────────────────┘
                           │  FreeRTOS API
            ┌──────────────▼──────────────────────┐
            │  components/rtthread/port/freertos  │  ← 兼容层（4943 行）
            │  xQueueCreate → ft_ipc_create 等    │
            └──────────────┬──────────────────────┘
                           │  RT-Thread API
            ┌──────────────▼──────────────────────┐
            │         RT-Thread 5.4.0             │
            │  src/  components/  libcpu/xtensa/  │
            └──────────────┬──────────────────────┘
                           │
            ┌──────────────▼──────────────────────┐
            │   bsp/ESP/ESP32_S3/board/           │  ← 板级
            │   控制台 UART GPIO I2C SPI ADC      │
            │   SMP  临界区  中断                  │
            └─────────────────────────────────────┘
```

**为什么可以这样分层**：IDF 的组件并不直接调用 FreeRTOS 内核符号，而是通过
`freertos/FreeRTOS.h` 里的宏。只要这些宏指向别处，内核就能换掉。

---

## 一、架构层 `libcpu/xtensa/`

RT-Thread 上游没有 Xtensa 支持。这一层是新写的，7 个文件约 110 KB。

### 文件职责

| 文件 | 内容 |
| --- | --- |
| `cpuport.h` | `struct rt_xt_core`（每核状态）、偏移量常量、内联的 IRQ 操作 |
| `context_gcc.S` | 上下文切换、线程首次运行、中断进入/退出、协处理器处理 |
| `cpuport.c` | `rt_hw_stack_init`、spinlock（S32C1I）、`rt_hw_cpu_id`、FPU 策略 |
| `atomic_xtensa.c` | 全套 `rt_hw_atomic_*`，用 S32C1I 实现 |
| `backtrace.c` | 栈回溯（`RT_BACKTRACE_LEVEL_MAX_NR`） |
| `SConscript` | scons 构建（路线 B，未完成） |

### 1.1 窗口化 ABI —— 最容易错的地方

Xtensa 用**窗口化寄存器**：函数调用的参数和返回地址不固定在物理寄存器上，
而是按 `call8` / `call12` / `call0` 分配到不同的窗口。

**关键规则**：`call8` 时，被调用者的 `a2..a7` 就是调用者的 `a10..a15`。

第一次实现时踩了这个坑：`_rtt_resume` 把 TCB 指针放在 `a2`，而调用方用的是 `call8`，
所以内核看到的是 `a10`——两个寄存器位置不同，读出来是垃圾值。

```asm
/* 错误：call8 下 a2 不是参数寄存器 */
mov     a2, a13

/* 正确：TCB 必须放 a10 */
mov     a10, a13
```

症状是 `current_thread = 1`，然后在第一次切换时崩溃。

### 1.2 `_rtt_int_enter` / `_rtt_int_exit`

Xtensa 的中断入口是 `xtensa_vectors.S`（Espressif 提供，OS 无关），它通过五个宏回调：

| 宏 | 我们的实现 |
| --- | --- |
| `XT_RTOS_INT_ENTER` | `_rtt_int_enter` |
| `XT_RTOS_INT_EXIT` | `_rtt_int_exit` |
| `XT_RTOS_SYSCALL` | 未用（RT-Thread 不需要） |
| `XT_RTOS_START` | `_rtt_first_switch` |
| `XT_RTOS_END` | 未用 |

`_rtt_int_enter` 要做四件事：

1. 保存 `a12`/`a13`（`_xt_context_save` 的前提）
2. 调 `_xt_context_save`
3. **同时维护两个嵌套计数**（见下）
4. 判断是不是最外层中断——是的话保存 FPU 状态并切到 ISR 栈

**为什么两个计数器**：`rt_xt_core[cpu].nest` 是移植自己的，`rt_interrupt_nest` 是内核的。
`rt_schedule()` 读后者来决定走协作切换还是中断切换。只维护一个的话，
从 ISR 里调 `rt_schedule()` 会走错路径——留下一个 solicited frame 直接从矢量跳走，
外层帧永远不会展开。症状是间断性的 `InstrFetchProhibited, PC: 0x00000000`。

### 1.3 FPU

S3 有单精度 FPU（协处理器 0）。策略：

- 线程首次运行时开 `CPENABLE = CP0`
- 每个核**最外层**中断进入时保存 16 个 FP 寄存器（`ssi f0..f15`）
- 退出时恢复

不做"懒惰保存"是因为 RT-Thread 的上下文切换点不能触发协处理器异常。

### 1.4 原子操作

Xtensa 的原子是 **S32C1I**（store-conditional）：

```asm
wsr     exp, SCOMPARE1      ; 期望值
rsync
mov     got, new
s32c1i  got, adr, 0         ; 若 *adr == SCOMPARE1 则写，否则读回
```

注意 `S32C1I` **要求 16 字节对齐**。`atomic_xtensa.c` 里对未对齐的地址回退到关中断。

**每个原子函数都必须带 `IRAM_ATTR`**：中断路径是
`_rtt_int_enter → rt_interrupt_get_nest → rt_atomic_load → rt_hw_atomic_load`，
全程要在 cache 关闭时可执行。实测漏掉这个属性导致的崩溃：

```
Guru Meditation Error: Core 0 panic'ed (Cache error).
PC : 0x420ba2c6   rt_hw_atomic_load at libcpu/xtensa/atomic_xtensa.c:105
Backtrace: ... xPortInIsrContext -> global_interrupt_disable -> BLE ISR
```

---

## 二、SMP

### 2.1 RT-Thread 的 SMP 调度器

两级就绪队列：

- **全局** `rt_thread_priority_table[]`——所有核共享
- **每核** `struct rt_cpu.priority_table[]`——本核可跑的线程

`_scheduler_get_highest_priority_thread()`（`scheduler_mp.c:239`）比较两者取最优。
线程可以用 `rt_thread_control(RT_THREAD_CTRL_BIND_CPU)` 绑核。

### 2.2 每核 tick

`vSystimerSetup()`（IDF 的 `port_systick.c:69-86`）用
`ETS_SYSTIMER_TARGET0_INTR_SOURCE + cpuid` 给每个核配独立的 SYSTIMER alarm。

**这个在 `CONFIG_FREERTOS_UNICORE=y` 下不工作**：alarm 配置循环是
`for (cpuid = 0; cpuid < configNUM_CORES; ++cpuid)`，单核构建下 alarm 1 拿不到周期。
所以 SMP 必须同时打开 `CONFIG_FREERTOS_NUMBER_OF_CORES=2`。

### 2.3 IPI

`board/smp.c` 实现核间中断：

```c
#define XT_RT_IPI_SOURCE(cpu)   (SYS_CPU_INTR_FROM_CPU_0_SOURCE + (cpu))
```

**编号规则是"寄存器 n 投递到核 n"**。第一版用了 `FROM_CPU_2/3`，
那对应的是不存在的核 2/3——IPI 永远收不到。

另一个坑：`esp_intr_alloc()` 只注册不使能。必须在 `board_init` 里显式
`esp_intr_enable(handle)`，否则 `INTENABLE` 停在 `0x00000002`。

### 2.4 临界区 —— 最后一个 SMP bug

```c
/* include/rthw.h:146，RT_USING_SMP 下 */
#define rt_hw_interrupt_disable()   rt_cpus_lock()
```

IDF 的临界区 `portENTER_CRITICAL` 如果映射到 `rt_hw_interrupt_disable()`，
两个核会争同一个全局 spinlock，而其中一个核还关着中断——**确定性死锁**。

`board/critical.c` 必须用 `rt_hw_local_irq_disable()`：

```c
level = rt_hw_local_irq_disable();     /* 只关本核 */
/* ... */
rt_hw_local_irq_enable(level);
```

这个 bug 的表现是"双核启动后卡住无输出"——没有 panic、没有复位、没有任何线索，
因为两个核都在关中断的状态下自旋。

### 2.5 flash 操作的跨核握手

这一块踩的坑最多，因为涉及四条独立的条件。

**问题**：一核写 flash 要关自己的指令 cache。另一核如果还在从 flash 取指，
就是 `Cache error`。

**方案**（`board/board.c` 的 `spi_flash_other_core_stop/resume`）：

```
CPU0 做 flash                      CPU1 的 tick handler
─────────────────                  ────────────────────
xt_rt_flash_cpu = 0
xt_rt_flash_op  = 1
                        ────────>  看到 op=1 且 cpu != 自己
                                   rsil 15        (屏蔽全部中断)
                                   ack = 1
                                   自旋等 op 清零  <────┐
关自己的 cache                                          │
写/擦 flash                                            │
开 cache                                              │
op = 0  ─────────────────────────────────────────────┘
                                   退出自旋，恢复 INTLEVEL
```

**四个必要条件，缺一个就死锁或崩溃**：

1. **只有另一核 park**（`xt_rt_flash_cpu != xPortGetCoreID()`）
   —— tick handler 在两核上都跑，不比较的话做操作的核会 park 自己，
   然后等自己清 op

2. **嵌套时不重复加锁**（depth 判断在 `rt_hw_spin_lock` 之前）
   —— 格式化 FAT 是多次嵌套的擦除，`rt_hw_spin_lock` 不可重入，
   第二次加锁会永远自旋

3. **用 `PS.INTLEVEL = 15` 屏蔽全部中断**，不只是非 IRAM 中断
   —— IDF 用 `ESP_INTR_FLAG_IRAM` 注册的 handler（BLE controller、
   esp_timer alarm）在 cache 关闭时仍然会跑，而它们调用 flash 里的 shim 代码

4. **跨核 spinlock 串行化**（`xt_rt_flash_lock`）
   —— 防止两核同时做 flash 操作，互相覆盖 op/ack/cpu

缺第 1 条的症状：静默挂死，两核都在 park 循环里。
缺第 2 条的症状：format 卡在第一个扇区。
缺第 3 条的症状：`Cache error`，PC 落在 `rt_sem_trytake` / `ft_ipc_resolve` / `xQueueGiveFromISR`。
缺第 4 条的症状：只有无线栈起来后才挂。

### 2.6 `main` 线程必须绑 CPU0

`src/components.c` 的补丁：

```c
#ifdef RT_USING_SMP
    rt_thread_control(tid, RT_THREAD_CTRL_BIND_CPU, (void *)0);
#endif
```

不绑的话 `main` 落在 CPU1，而 `main` 里跑 `rt_components_init()`，
它会调 `nvs_flash_init()` → 打开 flash 操作 → 要求"另一核"park。
于是另一核是 CPU0——而 CPU0 的 IPI 在这个移植上始终没打通：

```
IC i0=0 i1=27                    ← core 1 的 IPI 工作，core 0 的从不触发
FL enter=1 sent=1 spins=17117 block=0
```

---

## 三、板级 `bsp/ESP/ESP32_S3/board/`

| 文件 | 内容 |
| --- | --- |
| `board.c` | `rt_hw_board_init`、heap、tick、flash park 握手、esp_timer 启动 |
| `console.c` | `rt_hw_console_output`，早期用 ROM 输出 |
| `serial.c` | UART0 `rt_serial` |
| `interrupt.c` | 中断注册（填 `_xt_interrupt_table`）、统计 |
| `smp.c` | 第二核启动、IPI |
| `critical.c` | FreeRTOS 临界区映射（用 local IRQ） |
| `gpio.c` / `pwm.c` / `i2c.c` / `spi.c` / `adc.c` | 各外设 |
| `port_intr.h` | `rt_hw_intr_route` / `rt_hw_interrupt_is_masked` 声明 |

### 3.1 `board.c` 里为什么有些东西"故意不做"

文件头有一段清单：

```
Not done here, on purpose:

  esp_int_wdt_init()       the interrupt watchdog. It is a genuine loss ...
                           but it needs the level-4/5 timer path wired up
                           first; it goes in after the kernel boots
```

也就是说，**中断看门狗没启用**。这是一个已知的损失，用 TIMG 任务看门狗补上了
（见 `s3wdt.c`）。

### 3.2 堆

`startup.c` 在 CORE 阶段调 `rt_system_heap_init(&__heap_start__, &__heap_end__)`，
这两个符号由 IDF 的 `sections.ld.in` 提供，大小由 `CONFIG_IDF_RTTHREAD_HEAP_SIZE`
控制。

**默认 40 KB 不够**。Wi-Fi + BLE 起来后只剩 2 KB，lwIP 给 msh 线程建 per-thread
信号量时失败，报：

```
E (113976) lwip_arch: thread_sem_init: unable to create semaphore 0 != 0x3fcdd01c
Guru Meditation Error: Core 1 panic'ed (LoadProhibited)
```

改成 128 KB。**上限是 DRAM 剩余空间**——160 KB 会链接失败
（`region 'dram0_0_seg' overflowed by 10672 bytes`）。

---

## 四、FreeRTOS API 兼容层

`esp-idf/components/rtthread/port/freertos/`，4943 行，9 个文件（8 个 `.c` + 1 个 `.h`）。

### 4.1 核心思想

IDF 的组件调用 `xQueueCreate` / `xSemaphoreTake` / `xTaskCreate` 等等。
兼容层把这些映射到 RT-Thread 的 IPC 和线程：

| FreeRTOS | RT-Thread |
| --- | --- |
| `QueueHandle_t` | `struct ft_ipc *`（内部持有 `rt_sem_t` + 环形缓冲） |
| `SemaphoreHandle_t` | 同上，item_size = 0 |
| `TaskHandle_t` | `rt_thread_t` |
| `xTaskNotifyGive` | `ft_notify_*`（用 `rt_sem_t` + 计数器） |
| `portENTER_CRITICAL` | `rt_hw_local_irq_disable`（**不是** `rt_hw_interrupt_disable`） |

**实际实现了 64 个 `x*`/`v*` 函数**，全部静态定义在 `freertos_api.c`
（933 行）里，用脚本从 `port/freertos/*.c` 提取：

```
freertos_api.c   933 行   64 个 FreeRTOS 函数   <- 薄包装层
ft_ipc.c        1231 行    0 个               <- 队列/信号量的真正实现
ft_task.c        988 行    0 个               <- 线程映射
ft_notify.c      414 行    0 个               <- 通知
ft_event.c       177 行    0 个               <- 事件组
ft_qset.c        273 行    0 个               <- queue set
ft_query.c       239 行    0 个               <- 状态查询
ft_probe.c       129 行    0 个               <- 诊断
```

**这一层薄包装是刻意的**：`freertos_api.c` 只做参数转换和类型适配，所有需要
状态的东西（`ft_ipc` 的生命周期、等待队列、静态控制块布局）都在 `ft_*.c` 里。
这样 FreeRTOS 的 API 形状变化只影响一个文件，而 RT-Thread 侧的语义只在一个
地方实现。

用脚本核对：

```powershell
.\tools\list-shim-api.ps1          # 按类别列出，验证上表
.\tools\list-shim-api.ps1 -All     # 全部 64 个
.\tools\list-shim-api.ps1 -Missing # 文档提到但没实现的
```

下列分类来自脚本输出：

| 类别 | 数量 | 代表 |
| --- | --- | --- |
| Queue / Semaphore | 27 | `xQueueGenericCreate`, `xQueueSemaphoreTake`, `xQueueGiveFromISR` |
| Task | 22 | `xTaskCreatePinnedToCore`, `vTaskDelay`, `xTaskGetCoreID` |
| Notify | 6 | `xTaskGenericNotify`, `vTaskGenericNotifyGiveFromISR` |
| Event group | 6 | `xEventGroupCreate`, `xEventGroupWaitBits` |
| 调度器 / 超时 | 3 | `vTaskSuspendAll`, `xTaskCheckForTimeOut` |

核对方法：

```powershell
# 从源码里提取 shim 实际定义的 FreeRTOS 风格函数
Get-ChildItem components/rtthread/port/freertos/*.c |
  Select-String -Pattern '^[A-Za-z_][\w \*]*\b([xv][A-Z]\w+)\s*\('
```

这不是完整清单——`xQueueCreate` 之类的宏在头文件里，落到
`xQueueGenericCreate`。但**上表每一类都有实际实现**，不是设想中的映射。

### 4.2 静态创建

FreeRTOS 允许调用者提供控制块（`xQueueCreateStatic`）。兼容层把 `ft_ipc` 放在
调用者的 `StaticQueue_t` 里，所以 handle 就是调用者传入的指针——和 FreeRTOS 的语义一致。

```c
_Static_assert(sizeof(StaticQueue_t) >= sizeof(struct ft_ipc_stub), "...");
```

这两个断言保证布局兼容。任一边增长，构建就停。

### 4.3 `FT_IRAM` —— 一个不得不加的宏

`ft_core.h` 里：

```c
#define FT_IRAM             __attribute__((section(".iram1")))
```

**无条件定义，不是只在 Xtensa 上。** 早先的版本按 `__XTENSA__` 条件编译，
理由是只有 Xtensa 有 flash cache。这个理由对 ESP32-C5（RISC-V）同样成立——
它也有同样的 cache，flash 里的被调用者一样会触发 cache 访问错误。

在 RT-Thread 里 `section(".iram1")` 对两个架构都有效，所以条件编译只是多余的
复杂度，而且会在 RISC-V 上留下一个真实缺陷。

**为什么需要**：BLE controller 的 ISR 会调
`semphr_give_from_isr_wrapper → xQueueGiveFromISR → ft_ipc_send_isr`。
如果这些在 flash 里，而另一核正在写 flash，就是 `Cache error`。

实测两次：

```
PC 0x4204a426  xQueueGiveFromISR     (flash)  ← BLE ISR, cache 关闭
PC 0x4200d1d2  ft_ipc_resolve        (flash)  ← 同样
```

**局限**：即使 `ft_ipc_*` 全在 IRAM，它内部调用的 `rt_sem_trytake`
（`src/ipc.c:681`）还在 flash 里。所以最后是靠 `PS.INTLEVEL = 15` 屏蔽全部中断
解决的，`FT_IRAM` 是第二道防线而不是第一道。

---

## 四点五、中断延迟

`s3rt irqlat` 用周期计数器测量从 GPIO 边沿到进入处理函数的时间。十次取
最小/最大，因为控制环关心的是尾巴而不是平均。

```
irqlat: 10/10 edges, 434..480 cycles = 1.81..2.00 us (mean 1.81 us) at 240 MHz
```

**这是上界。** 测量在 ISR 读到计数器时结束，所以引脚写入自身剩下的工作也被
算进去了。用 `rt_pin_write()` 时是 531..546 cycles；换成 `gpio_ll_set_level()`
（一次寄存器写）后降到 434..480。差的约 100 cycles 是调用链开销，不在中断
路径上——这也是为什么这里用更紧的那个数字。

434 cycles @ 240 MHz = 1.81 µs。Xtensa 的中断进入本身约 30–50 cycles，其余
是 GPIO 矩阵、中断控制器和 RT-Thread 的 `_rtt_int_enter`。

无线的 ISR 会让这个数字变差，但那不影响正确性——只影响最坏情况下的控制精度。

## 五、IDF 侧的 16 个改动

`patches/0001-idf-core-smp-hooks.patch`。

| 组件 | 改什么 | 为什么 |
| --- | --- | --- |
| `freertos/Kconfig` | 加 `IDF_RTOS_RTTHREAD` 开关 | 整个移植的入口 |
| `freertos/port_systick.c` | flash park 握手 + 每核 tick | 见 §2.5 |
| `esp_system/esp_ipc.c` | 删掉 `rt_xt_ip3` 调试探针 | 它引用的符号不存在 |
| `esp_partition/partition.c` | 预创建 `s_partition_list_lock` | newlib 惰性锁在 SMP 下挂死 |
| `esp_partition/partition_target.c` | 同上 | |
| `nvs_flash/src/*` (9 个) | 预创建 mutex + 单例改静态存储 | newlib 惰性锁 + `operator new` 不返回 |
| `esp_libc/src/locks.c` | 缩进修正 | |
| `heap/multi_heap.c` | 空行 | |
| `spi_flash/flash_mmap.c` | 缩进修正 | |

### 5.1 newlib 惰性锁的坑

`_lock_t`（newlib 的 per-file 锁）是**惰性创建**的：第一次 `_lock_acquire` 时才建
底层 mutex。在 SMP 下，两个核同时首次 acquire 同一个锁会死锁。

症状：

- NVS：`nvs_flash_init()` 中的 `Lock` 永不返回
- partition：`esp_partition_find_first()` 永不返回（`PL enter=1 loaded=0`）

**修法**：从 `rt_hw_board_init()`（单线程上下文）预创建。

### 5.2 NVS 单例的 `operator new`

`nv_partition_manager.cpp` 里有个 Meyers 单例：

```cpp
static NVSPartitionManager& instance() {
    static NVSPartitionManager s;   // ← 第一次调用时构造
    return s;
}
```

在这个移植上 `operator new` 永不返回。改成静态成员 + 访问器，去掉动态分配。

---

## 六、验证方法

**每个改动都要在真板上验证，不能只看编译通过。**

方法论：

1. **先测基线**（改之前的数据）
2. **单变量**（一次只改一个）
3. **看数字不看感觉**（写次数、擦次数、耗时、`switches`）
4. **三次重复**（区分确定性和抖动）

例子：FAT 性能优化时，`s3fsbench` 用 `--wrap` 统计 flash 操作次数，
发现 32 个 64 字节文件产生 **1460 次写 / 170 次擦**——340 倍写放大。
这个数字直接否定了"调 cluster 大小"的方案（实测无收益），
指向了真正的瓶颈。详见 [FILESYSTEM.md](FILESYSTEM.md)。
