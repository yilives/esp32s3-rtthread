/*
 * FreeRTOS API surface, mapped onto the ft_* layer.
 *
 * This file only exists when CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM is on, i.e.
 * when components/freertos no longer compiles queue.c / tasks.c / timers.c /
 * event_groups.c / stream_buffer.c. Defining xQueueCreate while the real kernel
 * is still in the link would be a lottery: both archives would define it and
 * the winner would depend on archive order.
 *
 * Everything here is a one or two line delegation. If something in this file
 * starts containing logic, the logic belongs in ft_*.c instead.
 *
 * Two deviations from FreeRTOS that are deliberate:
 *
 * 1. Static allocation keeps the caller's storage but heap-allocates the
 *    object. ft_ipc is about 170 bytes and StaticQueue_t is about 120, so a
 *    caller's control block can never hold the object. Overflowing it would
 *    corrupt whatever follows, and failing would break esp_libc's lock
 *    initialisation, so ft_ipc_create_static_ram() uses the caller's storage as
 *    the ring buffer, remembers its control block verbatim for
 *    xQueueGenericGetStaticBuffers() to hand back, and keeps only the object on
 *    the heap. Deletion frees the object; the caller frees its own two buffers,
 *    which is exactly what vQueueDeleteWithCaps() does.
 *
 * 2. vTaskSwitchContext() is a trap. It is the function the FreeRTOS port calls
 *    to pick the next task, and it must never run under RT-Thread: if it does,
 *    the FreeRTOS scheduler is being entered from somewhere and the image is
 *    already in a state nobody designed for.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"

/* IRAM_ATTR: spi_flash calls into the scheduler API with the flash cache off. */
#include "esp_attr.h"

/* IRAM_ATTR: a few of the functions below are reachable with the flash cache
 * off (spi_flash's cache windows call into the scheduler API), and FreeRTOS's
 * own linker fragment puts the corresponding tasks.c functions in IRAM for
 * exactly that reason. See xTaskGetSchedulerState() below. */

#include "ft_core.h"

/* The stack unit constant in ft_core.h is derived from this port's
 * StackType_t. If a future port makes it a 32-bit type, every task stack in the
 * system would silently be a quarter of what the caller asked for, so fail the
 * build instead. */
_Static_assert(sizeof(StackType_t) == FT_STACK_UNIT_BYTES,
               "ft_core.h FT_STACK_UNIT_BYTES no longer matches StackType_t");

/* Referenced by stack_macros.h, which some port code inlines even though it
 * never runs under RT-Thread. Nothing is scheduled by FreeRTOS here, so the
 * array only has to exist for the linker. */
void *pxCurrentTCBs[configNUM_CORES] = { NULL };

/*
 * The static creation path places an ft_ipc inside the caller's StaticQueue_t,
 * so the two sizes have to stay compatible. struct ft_ipc is opaque here (its
 * definition belongs to ft_ipc.c), so the check is split in two and joined by a
 * constant:
 *
 *   ft_ipc.c        asserts sizeof(struct ft_ipc) <= FT_IPC_STATIC_BUDGET
 *   here            asserts sizeof(StaticQueue_t)  >= FT_IPC_STATIC_BUDGET
 *
 * Together they mean the placement is sound, and if either side grows the build
 * stops instead of the static path quietly falling back to the heap.
 */
_Static_assert(sizeof(StaticQueue_t) >= sizeof(struct ft_ipc_stub),
               "StaticQueue_t cannot hold the port's stub, so a static handle "
               "could not equal the caller's control block");
_Static_assert(sizeof(StaticSemaphore_t) >= sizeof(struct ft_ipc_stub),
               "StaticSemaphore_t cannot hold the port's stub");

static BaseType_t ft_result(int rc)
{
    return (rc >= 0) ? pdTRUE : pdFALSE;
}

static void ft_warn_once(rt_uint32_t *flag, const char *what)
{
    if (*flag == 0)
    {
        *flag = 1;
        rt_kprintf("freertos-shim: %s\n", what);
    }
}

/* ------------------------------------------------------------------ queue -- */

QueueHandle_t xQueueGenericCreate(const UBaseType_t uxQueueLength,
                                  const UBaseType_t uxItemSize,
                                  const uint8_t ucQueueType)
{
    switch (ucQueueType)
    {
    case queueQUEUE_TYPE_MUTEX:
        return (QueueHandle_t)ft_ipc_create_mutex_ex(0);

    case queueQUEUE_TYPE_RECURSIVE_MUTEX:
        return (QueueHandle_t)ft_ipc_create_mutex_ex(1);

    case queueQUEUE_TYPE_COUNTING_SEMAPHORE:
        /* The initial count is applied by xQueueCreateCountingSemaphore. */
        return (QueueHandle_t)ft_ipc_create(uxQueueLength ? uxQueueLength : 1, 0);

    default:
        return (QueueHandle_t)ft_ipc_create(uxQueueLength, uxItemSize);
    }
}

QueueHandle_t xQueueCreateMutex(const uint8_t ucQueueType)
{
    return (QueueHandle_t)ft_ipc_create_mutex_ex(ucQueueType == queueQUEUE_TYPE_RECURSIVE_MUTEX);
}

QueueHandle_t xQueueCreateMutexStatic(const uint8_t ucQueueType,
                                      StaticQueue_t *pxStaticQueue)
{
    /* The handle is the caller's own block, which is what FreeRTOS promises and
     * esp_libc asserts (locks.c:422). The block holds a stub; the object itself
     * is on the heap because it would not fit. */
    return (QueueHandle_t)ft_ipc_create_mutex_stub(
        pxStaticQueue, sizeof(*pxStaticQueue),
        ucQueueType == queueQUEUE_TYPE_RECURSIVE_MUTEX);
}

QueueHandle_t xQueueCreateCountingSemaphore(const UBaseType_t uxMaxCount,
                                             const UBaseType_t uxInitialCount)
{
    return (QueueHandle_t)ft_ipc_create_sem(uxMaxCount, uxInitialCount);
}

BaseType_t xQueueGenericSend(QueueHandle_t xQueue,
                             const void *const pvItemToQueue,
                             TickType_t xTicksToWait,
                             const BaseType_t xCopyPosition)
{
    /* queueOVERWRITE now has real semantics rather than a warning: it replaces
     * the single item instead of appending. queueSEND_TO_FRONT still appends,
     * because the ring has no way to prepend - and unlike overwrite, a wrong
     * answer there changes ordering rather than failing loudly, so it warns. */
    if (xCopyPosition == queueOVERWRITE)
    {
        return ft_result(ft_ipc_overwrite(ft_ipc_resolve(xQueue), pvItemToQueue));
    }

    if (xCopyPosition == queueSEND_TO_FRONT)
    {
        static rt_uint32_t warned;

        ft_warn_once(&warned,
                     "xQueueGenericSend: queueSEND_TO_FRONT is not implemented, "
                     "the item was appended");
    }

    return ft_result(ft_ipc_send(ft_ipc_resolve(xQueue), pvItemToQueue, xTicksToWait));
}

BaseType_t IRAM_ATTR xQueueGenericSendFromISR(QueueHandle_t xQueue,
                                    const void *const pvItemToQueue,
                                    BaseType_t *const pxHigherPriorityTaskWoken,
                                    const BaseType_t xCopyPosition)
{
    if (xCopyPosition == queueOVERWRITE)
    {
        return ft_result(ft_ipc_overwrite_isr(ft_ipc_resolve(xQueue), pvItemToQueue,
                                              (ft_base_t *)pxHigherPriorityTaskWoken));
    }

    if (xCopyPosition == queueSEND_TO_FRONT)
    {
        static rt_uint32_t warned;

        ft_warn_once(&warned,
                     "xQueueGenericSendFromISR: queueSEND_TO_FRONT is not "
                     "implemented, the item was appended");
    }

    return ft_result(ft_ipc_send_isr(ft_ipc_resolve(xQueue), pvItemToQueue,
                                     (ft_base_t *)pxHigherPriorityTaskWoken));
}

BaseType_t xQueueReceive(QueueHandle_t xQueue, void *const pvBuffer,
                         TickType_t xTicksToWait)
{
    return ft_result(ft_ipc_receive(ft_ipc_resolve(xQueue), pvBuffer, xTicksToWait));
}

BaseType_t IRAM_ATTR xQueueReceiveFromISR(QueueHandle_t xQueue, void *const pvBuffer,
                                BaseType_t *const pxHigherPriorityTaskWoken)
{
    return ft_result(ft_ipc_receive_isr(ft_ipc_resolve(xQueue), pvBuffer,
                                        (ft_base_t *)pxHigherPriorityTaskWoken));
}

BaseType_t IRAM_ATTR xQueueIsQueueEmptyFromISR(const QueueHandle_t xQueue)
{
    return ft_ipc_is_empty(ft_ipc_resolve(xQueue));
}

BaseType_t xQueueSemaphoreTake(QueueHandle_t xQueue, TickType_t xTicksToWait)
{
    return ft_result(ft_ipc_take(ft_ipc_resolve(xQueue), xTicksToWait));
}

BaseType_t IRAM_ATTR xQueueGiveFromISR(QueueHandle_t xQueue,
                             BaseType_t *const pxHigherPriorityTaskWoken)
{
    return ft_result(ft_ipc_give_isr(ft_ipc_resolve(xQueue),
                                     (ft_base_t *)pxHigherPriorityTaskWoken));
}

UBaseType_t uxQueueMessagesWaiting(const QueueHandle_t xQueue)
{
    return (UBaseType_t)ft_ipc_messages_waiting(ft_ipc_resolve(xQueue));
}

UBaseType_t IRAM_ATTR uxQueueMessagesWaitingFromISR(const QueueHandle_t xQueue)
{
    /* The count is read under an interrupt lock in ft_ipc_messages_waiting's
     * callers' world, but it is a plain word here: an ISR that shadows a pending
     * send by one item is not a problem the API contracts against, and taking a
     * lock in an ISR to make a diagnostic exact would be worse. */
    return (UBaseType_t)ft_ipc_messages_waiting(ft_ipc_resolve(xQueue));
}

BaseType_t IRAM_ATTR xQueueIsQueueFullFromISR(const QueueHandle_t xQueue)
{
    return (BaseType_t)(ft_ipc_spaces_available(ft_ipc_resolve(xQueue)) == 0);
}

UBaseType_t uxQueueSpacesAvailable(const QueueHandle_t xQueue)
{
    return (UBaseType_t)ft_ipc_spaces_available(ft_ipc_resolve(xQueue));
}

/* xQueuePeek: read the front item without removing it.
 *
 * Not used by anything in this image, but it is in queue.h unconditionally and
 * the implementation is three lines: copy the slot, leave the ring alone. The
 * index and item_size are read without a lock, which is the same exposure
 * uxQueueMessagesWaiting has and acceptable for a peek, since the alternative is
 * a lock in a path that may be called from an ISR. */
BaseType_t xQueuePeek(QueueHandle_t xQueue, void *const pvBuffer,
                      TickType_t xTicksToWait)
{
    struct ft_ipc *ipc = ft_ipc_resolve(xQueue);
    int rc;

    if (ipc == NULL)
    {
        return pdFALSE;
    }

    /* Block until something is there, using a zero-length receive into a
     * throwaway buffer is not possible - that would consume the item. So this
     * uses the same "wait for availability, then look" shape as a receive, but
     * with ft_ipc_peek doing the copy. */
    rc = ft_ipc_peek(ipc, pvBuffer, xTicksToWait);

    return ft_result(rc);
}

/* xQueueGenericReset: empty the queue and restore its initial state. */
BaseType_t xQueueGenericReset(QueueHandle_t xQueue, BaseType_t xNewQueue)
{
    RT_UNUSED(xNewQueue);       /* RT-Thread's ring starts at zero either way */

    return ft_result(ft_ipc_reset(ft_ipc_resolve(xQueue)));
}

/* ------------------------------------------------------------ queue set -- */

/* Only esp_ble_mesh and esp_ble_iso use queue sets, and neither is in this
 * image. The implementation is real rather than a stub because a stub would turn
 * "BLE does not work yet" into "BLE hangs at startup with nothing to go on". */

QueueSetHandle_t xQueueCreateSet(const UBaseType_t uxEventQueueLength)
{
    return (QueueSetHandle_t)ft_qset_create(uxEventQueueLength);
}

BaseType_t xQueueAddToSet(QueueSetMemberHandle_t xQueueOrSemaphore,
                          QueueSetHandle_t xQueueSet)
{
    return (BaseType_t)ft_qset_add((void *)xQueueSet,
                                   ft_ipc_resolve(xQueueOrSemaphore));
}

BaseType_t xQueueRemoveFromSet(QueueSetMemberHandle_t xQueueOrSemaphore,
                               QueueSetHandle_t xQueueSet)
{
    return (BaseType_t)ft_qset_remove((void *)xQueueSet,
                                      ft_ipc_resolve(xQueueOrSemaphore));
}

QueueSetMemberHandle_t xQueueSelectFromSet(QueueSetHandle_t xQueueSet,
                                           const TickType_t xTicksToWait)
{
    return (QueueSetMemberHandle_t)ft_qset_select((void *)xQueueSet,
                                                  xTicksToWait, NULL);
}

QueueSetMemberHandle_t IRAM_ATTR xQueueSelectFromSetFromISR(QueueSetHandle_t xQueueSet)
{
    return (QueueSetMemberHandle_t)ft_qset_select_isr((void *)xQueueSet, NULL);
}

void vQueueDelete(QueueHandle_t xQueue)
{
    ft_ipc_delete(ft_ipc_resolve(xQueue));
}

BaseType_t xQueueTakeMutexRecursive(QueueHandle_t xMutex, TickType_t xTicksToWait)
{
    return ft_result(ft_ipc_take(ft_ipc_resolve(xMutex), xTicksToWait));
}

BaseType_t xQueueGiveMutexRecursive(QueueHandle_t xMutex)
{
    return ft_result(ft_ipc_give(ft_ipc_resolve(xMutex)));
}

TaskHandle_t xQueueGetMutexHolder(QueueHandle_t xSemaphore)
{
    return (TaskHandle_t)ft_ipc_mutex_holder(ft_ipc_resolve(xSemaphore));
}

/* ------------------------------------------------------------------- task -- */

/* xTaskCreate() is a static inline in task.h that forwards here, so it must not
 * be defined again: the header already provides it in every translation unit. */
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t pxTaskCode,
                                   const char *const pcName,
                                   const configSTACK_DEPTH_TYPE usStackDepth,
                                   void *const pvParameters,
                                   UBaseType_t uxPriority,
                                   TaskHandle_t *const pxCreatedTask,
                                   const BaseType_t xCoreID)
{
    static rt_uint32_t warned;

    /*
     * The pinning is honoured now. It used to be folded onto core 0 with a
     * warning, from the days when only one core was alive. With both cores
     * running that lie is observable: esp_ipc's worker asserts
     * `cpuid == xPortGetCoreID()` (esp_ipc.c:56), and its core-1 worker is
     * created with xTaskCreatePinnedToCore(..., 1).
     *
     * The core is passed into ft_task_create rather than applied here, because
     * the interesting tasks (esp_ipc's two workers among them) are created from
     * constructors, before there is a scheduler to bind in. ft_task_create
     * records the request alongside the deferred thread and applies it in
     * ft_task_start_pending().
     */
    void *thread = ft_task_create(pcName, pxTaskCode, pvParameters,
                                  (ft_ubase_t)usStackDepth, (ft_ubase_t)uxPriority,
                                  (void **)pxCreatedTask,
                                  (xCoreID == tskNO_AFFINITY) ? -1 : (int)xCoreID);

    if (thread == RT_NULL)
    {
        return pdFAIL;
    }

    RT_UNUSED(warned);

    return pdPASS;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return (TaskHandle_t)ft_task_self();
}

/*
 * xTaskGetCoreID() - which core a task is running on.
 *
 * Newly required by the dual-core build. esp_hw_support/intr_alloc.c:933 calls
 * it from esp_intr_free() to decide whether the handle belongs to this core,
 * and that file is compiled on every target. With one core the comparison folds
 * away at compile time, which is why UP never needed the symbol - measured as an
 * "undefined reference to `xTaskGetCoreID'" the moment FREERTOS_UNICORE was
 * cleared.
 *
 * FreeRTOS's contract is "the core the given task is running on, or the calling
 * core when xTask is NULL". This port really does run tasks on both cores, so
 * answering with the hardware core id is the honest answer rather than a stub:
 * intr_alloc's question ("do I need to ask the other core to free this?") is
 * answered correctly exactly when this is told the truth.
 */
BaseType_t xTaskGetCoreID(TaskHandle_t xTask)
{
    RT_UNUSED(xTask);

    return (BaseType_t)rt_hw_cpu_id();
}

/* The per-core variants. Single core here, so both collapse to the plain ones -
 * but they have to exist, because task.h declares them and a driver written for
 * a dual-core part will call them. */
TaskHandle_t xTaskGetCurrentTaskHandleForCore(BaseType_t xCoreID)
{
    RT_UNUSED(xCoreID);

    return (TaskHandle_t)ft_task_self();
}

TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t xCoreID)
{
    RT_UNUSED(xCoreID);

    return (TaskHandle_t)ft_task_idle_handle();
}

void vTaskResume(TaskHandle_t xTaskToResume)
{
    /* Resume is not the same as aborting a delay: FreeRTOS has both, and this is
     * the one that takes a suspended task back to ready without touching
     * anything the task was waiting on. rt_thread_resume() is exactly that. */
    (void)ft_task_resume_isr(xTaskToResume, NULL);
}

/*
 * xTaskCreateStaticPinnedToCore.
 *
 * The caller supplies both the stack and the control block, which means FreeRTOS
 * expects them to be used in place: StaticTask_t is a 60-byte placeholder whose
 * only purpose is to be big enough for the real TCB, and rt_thread is larger.
 *
 * Writing an rt_thread into a 60-byte StaticTask_t corrupts whatever follows it,
 * so the static path here refuses unless the caller's block is documented to be
 * large enough. esp_ble_mesh is the only user and it passes a StaticTask_t, so
 * this returns NULL and the component reports a create failure instead of
 * corrupting the heap. Every other static task path in IDF goes through
 * xTaskCreateStaticPinnedToCoreWithCaps, which allocates its own control block.
 *
 * Refusing is not a regression: before this function existed the call did not
 * link, so the component could not be built at all.
 */
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t pxTaskCode,
                                           const char *const pcName,
                                           const uint32_t ulStackDepth,
                                           void *const pvParameters,
                                           UBaseType_t uxPriority,
                                           StackType_t *const puxStackBuffer,
                                           StaticTask_t *const pxTaskBuffer,
                                           const BaseType_t xCoreID)
{
    RT_UNUSED(pxTaskCode);
    RT_UNUSED(pcName);
    RT_UNUSED(ulStackDepth);
    RT_UNUSED(pvParameters);
    RT_UNUSED(uxPriority);
    RT_UNUSED(puxStackBuffer);
    RT_UNUSED(pxTaskBuffer);
    RT_UNUSED(xCoreID);

    _Static_assert(sizeof(StaticTask_t) < sizeof(struct rt_thread),
                   "this port assumes StaticTask_t cannot hold an rt_thread");

    {
        static rt_uint32_t warned;

        ft_warn_once(&warned,
                     "xTaskCreateStaticPinnedToCore: a StaticTask_t is too "
                     "small for an rt_thread; use the WithCaps variant");
    }

    return NULL;
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)ft_task_tick_count();
}

TickType_t IRAM_ATTR xTaskGetTickCountFromISR(void)
{
    return (TickType_t)ft_task_tick_count_isr();
}

/*
 * xTaskGetSchedulerState() - and it must live in IRAM.
 *
 * spi_flash/cache_utils.c calls this from
 * spi_flash_enable_interrupts_caches_and_other_cpu(), i.e. at the exact moment
 * the flash cache is being turned back on. FreeRTOS's own copy is placed in IRAM
 * for that reason (components/freertos/linker.lf:90
 * `tasks:xTaskGetSchedulerState (noflash_text)`); the shim's copy was in flash,
 * so the first flash operation of the boot died with
 *
 *   Guru Meditation Error: Core 0 panic'ed (Cache error).
 *   Cache disabled but cached memory region accessed
 *   PC : 0x42041172   (0x42... = flash, 0x403... = IRAM)
 *      <- cache_enable (spi_flash_os_func_app.c:81)
 *      <- spi_flash_enable_interrupts_caches_and_other_cpu (cache_utils.c:208)
 *      <- esp_flash_init_default_chip
 *
 * The call is made again at cache_utils.c:220 and :234, so this is a window
 * entered repeatedly, not a one-off. It only started failing in the dual-core
 * build because with CONFIG_FREERTOS_UNICORE the assert at :208 folds away.
 */
BaseType_t IRAM_ATTR xTaskGetSchedulerState(void)
{
    return (BaseType_t)ft_task_scheduler_state();
}

void vTaskDelay(const TickType_t xTicksToDelay)
{
    ft_task_delay(xTicksToDelay);
}

void vTaskDelete(TaskHandle_t xTaskToDelete)
{
    ft_task_delete(xTaskToDelete);
}

/*
 * Stack high water mark.
 *
 * INCLUDE_uxTaskGetStackHighWaterMark is 1 in this configuration, so task.h
 * declares this function to every caller, and tasks.c - the only definition -
 * is not compiled when the shim is on. The first caller would have hit an
 * undefined reference at link time. That is the ordinary reason this exists.
 *
 * The interesting part is that it can be implemented at all. FreeRTOS measures
 * the mark by filling a new stack with a known pattern and counting what is left
 * untouched; RT-Thread does exactly the same with '#' - src/thread.c:210,
 * rt_memset(thread->stack_addr, '#', thread->stack_size) - so the information is
 * already there and needs no extra bookkeeping in the port.
 *
 * The stack grows downward, so the survivors are at the bottom of the buffer and
 * the mark is the offset of the first byte that is no longer '#'.
 */
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t xTask)
{
    rt_thread_t thread = xTask ? (rt_thread_t)xTask : rt_thread_self();
    const rt_uint8_t *base;
    rt_uint32_t       size;
    rt_uint32_t       free_bytes = 0;

    if (thread == RT_NULL)
    {
        return 0;
    }

    base = (const rt_uint8_t *)thread->stack_addr;
    size = thread->stack_size;

    if (base == RT_NULL || size == 0)
    {
        return 0;
    }

    while (free_bytes < size && base[free_bytes] == '#')
    {
        free_bytes++;
    }

    /* FreeRTOS reports the count in StackType_t units, which on this port is one
     * byte (FT_STACK_UNIT_BYTES, asserted against sizeof(StackType_t) above).
     * No conversion, and worth saying out loud because the IDF documentation
     * describes the unit as a word. */
    return (UBaseType_t)free_bytes;
}

UBaseType_t uxTaskGetNumberOfTasks(void)
{
    return (UBaseType_t)ft_task_count();
}

char *pcTaskGetName(TaskHandle_t xTaskToQuery)
{
    return (char *)ft_task_name(xTaskToQuery);
}

TaskHandle_t xTaskGetHandle(const char *pcNameToQuery)
{
    return (TaskHandle_t)ft_task_find(pcNameToQuery);
}

TaskHandle_t xTaskGetIdleTaskHandle(void)
{
    return (TaskHandle_t)ft_task_idle_handle();
}

/* eTaskState is an enum in task.h; the numeric value comes back through
 * ft_task_state() so this file does not have to include task.h's enum twice.
 * The cast is safe because ft_query.c documents that the values line up. */
eTaskState eTaskGetState(TaskHandle_t xTask)
{
    return (eTaskState)ft_task_state(xTask);
}

BaseType_t xTaskAbortDelay(TaskHandle_t xTask)
{
    return (BaseType_t)ft_task_abort_delay(xTask);
}

BaseType_t IRAM_ATTR xTaskResumeFromISR(TaskHandle_t xTaskToResume)
{
    ft_base_t woken = ftFAIL;

    (void)ft_task_resume_isr(xTaskToResume, &woken);

    return (BaseType_t)woken;
}

BaseType_t xTaskDelayUntil(TickType_t *const pxPreviousWakeTime,
                           const TickType_t xTimeIncrement)
{
    return (BaseType_t)ft_task_delay_until((ft_tick_t *)pxPreviousWakeTime,
                                           (ft_tick_t)xTimeIncrement);
}

void vTaskSuspend(TaskHandle_t xTaskToSuspend)
{
    ft_task_suspend(xTaskToSuspend);
}

void vTaskSuspendAll(void)
{
    ft_task_suspend_all();
}

BaseType_t xTaskResumeAll(void)
{
    return ft_task_resume_all();
}

void vTaskSwitchContext(void)
{
    /* Must never be reached: it would mean the FreeRTOS scheduler is running,
     * which this image does not do. Print loudly the first time. */
    static rt_uint32_t warned;

    ft_warn_once(&warned,
                 "vTaskSwitchContext() called - the FreeRTOS scheduler is not "
                 "running under RT-Thread");
}

void vTaskPrioritySet(TaskHandle_t xTask, UBaseType_t uxNewPriority)
{
    ft_task_set_priority(xTask, (ft_ubase_t)uxNewPriority);
}

UBaseType_t uxTaskPriorityGet(const TaskHandle_t xTask)
{
    return (UBaseType_t)ft_task_get_priority(xTask);
}

void vTaskSetThreadLocalStoragePointer(TaskHandle_t xTaskToSet,
                                       BaseType_t xIndex,
                                       void *pvValue)
{
    ft_task_set_tls(xTaskToSet, (ft_ubase_t)xIndex, pvValue);
}

void *pvTaskGetThreadLocalStoragePointer(TaskHandle_t xTaskToQuery,
                                         BaseType_t xIndex)
{
    return ft_task_get_tls(xTaskToQuery, (ft_ubase_t)xIndex);
}

/* ------------------------------------------------------------ event group -- */

EventGroupHandle_t xEventGroupCreate(void)
{
    return (EventGroupHandle_t)ft_event_create();
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t xEventGroup,
                               const EventBits_t uxBitsToSet)
{
    return (EventBits_t)ft_event_set_bits((struct ft_event *)xEventGroup,
                                          (rt_uint32_t)uxBitsToSet);
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t xEventGroup,
                                 const EventBits_t uxBitsToClear)
{
    return (EventBits_t)ft_event_clear_bits((struct ft_event *)xEventGroup,
                                            (rt_uint32_t)uxBitsToClear);
}

/* Only the ISR variant is a real function: plain xEventGroupGetBits is a macro
 * over xEventGroupClearBits(group, 0) in event_groups.h, so defining it here
 * would be a redefinition error - the same trap xTaskCreate and xTaskNotify set.
 * The read is one word, so no lock is needed. */
EventBits_t xEventGroupGetBitsFromISR(EventGroupHandle_t xEventGroup)
{
    return (EventBits_t)ft_event_get_bits((struct ft_event *)xEventGroup);
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t xEventGroup,
                                const EventBits_t uxBitsToWaitFor,
                                const BaseType_t xClearOnExit,
                                const BaseType_t xWaitForAllBits,
                                TickType_t xTicksToWait)
{
    return (EventBits_t)ft_event_wait_bits((struct ft_event *)xEventGroup,
                                           (rt_uint32_t)uxBitsToWaitFor,
                                           xClearOnExit ? 1 : 0,
                                           xWaitForAllBits ? 1 : 0,
                                           xTicksToWait);
}

void vEventGroupDelete(EventGroupHandle_t xEventGroup)
{
    ft_event_delete((struct ft_event *)xEventGroup);
}

/* ------------------------------------------------------- static creation -- */

/*
 * xQueueCreateWithCaps() in idf_additions.c allocates a StaticQueue_t and calls
 * these three, so they are on esp_libc's and esp_driver_uart's init path even
 * when the caller never asked for static allocation.
 */

QueueHandle_t xQueueGenericCreateStatic(const UBaseType_t uxQueueLength,
                                        const UBaseType_t uxItemSize,
                                        uint8_t *pucQueueStorage,
                                        StaticQueue_t *pxStaticQueue,
                                        const uint8_t ucQueueType)
{
    switch (ucQueueType)
    {
    case queueQUEUE_TYPE_MUTEX:
        return (QueueHandle_t)ft_ipc_create_mutex_stub(
            pxStaticQueue, sizeof(*pxStaticQueue), 0);

    case queueQUEUE_TYPE_RECURSIVE_MUTEX:
        return (QueueHandle_t)ft_ipc_create_mutex_stub(
            pxStaticQueue, sizeof(*pxStaticQueue), 1);

    default:
        return (QueueHandle_t)ft_ipc_create_stub(
            uxQueueLength ? uxQueueLength : 1, uxItemSize, pucQueueStorage,
            pxStaticQueue, sizeof(*pxStaticQueue));
    }
}

BaseType_t xQueueGenericGetStaticBuffers(QueueHandle_t xQueue,
                                         uint8_t **ppucQueueStorage,
                                         StaticQueue_t **ppxStaticQueue)
{
    if (xQueue == NULL || ppucQueueStorage == NULL || ppxStaticQueue == NULL)
    {
        return pdFALSE;
    }

    *ppucQueueStorage = (uint8_t *)ft_ipc_storage(ft_ipc_resolve(xQueue));
    *ppxStaticQueue   = (StaticQueue_t *)ft_ipc_static_cb(ft_ipc_resolve(xQueue));

    return pdTRUE;
}

QueueHandle_t xQueueCreateCountingSemaphoreStatic(const UBaseType_t uxMaxCount,
                                                  const UBaseType_t uxInitialCount,
                                                  StaticQueue_t *pxStaticQueue)
{
    struct ft_ipc *ipc = ft_ipc_create_stub(uxMaxCount ? uxMaxCount : 1, 0,
                                            NULL, pxStaticQueue,
                                            sizeof(*pxStaticQueue));
    UBaseType_t i;

    if (ipc == NULL)
    {
        return NULL;
    }

    for (i = 0; i < uxInitialCount; i++)
    {
        if (ft_ipc_give(ipc) != FT_OK)
        {
            break;
        }
    }

    return (QueueHandle_t)ipc;
}

/* ------------------------------------------------------- notifications -- */

/*
 * Only the generic functions exist as symbols. task.h defines xTaskNotify,
 * xTaskNotifyWait, ulTaskNotifyTake and friends as function-like macros over
 * these, so defining the short names here is a redefinition error - the same
 * trap as xTaskCreate being a static inline in task.h. The compiler caught it,
 * which is why this block talks to the generic layer only.
 *
 * configTASK_NOTIFICATION_ARRAY_ENTRIES is 1 in this configuration, so index 0
 * is the only valid index and the others are ignored.
 */

BaseType_t xTaskGenericNotify(TaskHandle_t xTaskToNotify,
                              UBaseType_t uxIndexToNotify,
                              uint32_t ulValue,
                              eNotifyAction eAction,
                              uint32_t *pulPreviousNotificationValue)
{
    RT_UNUSED(uxIndexToNotify);

    if (pulPreviousNotificationValue != NULL)
    {
        *pulPreviousNotificationValue = ft_task_notify_peek(xTaskToNotify);
    }

    return ft_result(ft_task_notify(xTaskToNotify, ulValue, (ft_base_t)eAction));
}

BaseType_t xTaskGenericNotifyFromISR(TaskHandle_t xTaskToNotify,
                                     UBaseType_t uxIndexToNotify,
                                     uint32_t ulValue,
                                     eNotifyAction eAction,
                                     uint32_t *pulPreviousNotificationValue,
                                     BaseType_t *pxHigherPriorityTaskWoken)
{
    RT_UNUSED(uxIndexToNotify);

    if (pulPreviousNotificationValue != NULL)
    {
        *pulPreviousNotificationValue = ft_task_notify_peek(xTaskToNotify);
    }

    return ft_result(ft_task_notify_isr(xTaskToNotify, ulValue, (ft_base_t)eAction,
                                        (ft_base_t *)pxHigherPriorityTaskWoken));
}

void vTaskGenericNotifyGiveFromISR(TaskHandle_t xTaskToNotify,
                                   UBaseType_t uxIndexToNotify,
                                   BaseType_t *pxHigherPriorityTaskWoken)
{
    RT_UNUSED(uxIndexToNotify);

    (void)ft_task_notify_isr(xTaskToNotify, 0, FT_NOTIFY_INCREMENT,
                             (ft_base_t *)pxHigherPriorityTaskWoken);
}

BaseType_t xTaskGenericNotifyWait(UBaseType_t uxIndexToWaitOn,
                                  uint32_t ulBitsToClearOnEntry,
                                  uint32_t ulBitsToClearOnExit,
                                  uint32_t *pulNotificationValue,
                                  TickType_t xTicksToWait)
{
    ft_base_t timed_out = 0;
    rt_uint32_t value;

    RT_UNUSED(uxIndexToWaitOn);

    value = ft_task_notify_wait(ulBitsToClearOnEntry, ulBitsToClearOnExit,
                                xTicksToWait, &timed_out);

    if (pulNotificationValue != NULL)
    {
        *pulNotificationValue = value;
    }

    return timed_out ? pdFALSE : pdTRUE;
}

uint32_t ulTaskGenericNotifyTake(UBaseType_t uxIndexToWaitOn,
                                 BaseType_t xClearCountOnExit,
                                 TickType_t xTicksToWait)
{
    RT_UNUSED(uxIndexToWaitOn);

    return ft_task_notify_take(xClearCountOnExit, xTicksToWait);
}

BaseType_t xTaskGenericNotifyStateClear(TaskHandle_t xTask,
                                        UBaseType_t uxIndexToClear)
{
    RT_UNUSED(uxIndexToClear);

    /* FreeRTOS returns pdTRUE when a notification was pending and clears it. */
    return ft_task_notify_clear(xTask) ? pdTRUE : pdFALSE;
}

/* ------------------------------------------------------------ timeouts -- */

/* TimeOut_t is { BaseType_t xOverflowCount; TickType_t xTimeOnEntering; } and
 * ft_timeout_t mirrors it, so the cast below is a reinterpretation of the same
 * two fields. */
_Static_assert(sizeof(TimeOut_t) == sizeof(ft_timeout_t),
               "ft_timeout_t must match FreeRTOS TimeOut_t field for field");

void vTaskSetTimeOutState(TimeOut_t *const pxTimeOut)
{
    ft_timeout_set((ft_timeout_t *)pxTimeOut);
}

void vTaskInternalSetTimeOutState(TimeOut_t *const pxTimeOut)
{
    /* Same bookkeeping; FreeRTOS splits it only to skip a scheduler-state check
     * that this port does not need. */
    ft_timeout_set((ft_timeout_t *)pxTimeOut);
}

BaseType_t xTaskCheckForTimeOut(TimeOut_t *const pxTimeOut,
                                TickType_t *const pxTicksToWait)
{
    /* TickType_t is unsigned int and ft_tick_t is unsigned long: same width on
     * rv32, different types, so the remaining-tick value has to be copied
     * rather than aliased. */
    ft_tick_t remaining = *pxTicksToWait;
    ft_base_t expired = ft_timeout_check((ft_timeout_t *)pxTimeOut, &remaining);

    *pxTicksToWait = remaining;

    return expired ? pdTRUE : pdFALSE;
}

/* ------------------------------------------------- IDF task additions -- */

#if CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS
void vTaskSetThreadLocalStoragePointerAndDelCallback(TaskHandle_t xTaskToSet,
                                                     BaseType_t xIndex,
                                                     void *pvValue,
                                                     TlsDeleteCallbackFunction_t pvDelCallback)
{
    /* esp_event hangs its per-task state on slot 0 with a free() callback, so
     * the callback has to actually run: ft_task_delete() invokes it. */
    ft_task_set_tls_with_cb(xTaskToSet, (ft_ubase_t)xIndex, pvValue,
                            (ft_tls_del_cb_t)pvDelCallback);
}
#endif
