/*
 * Tasks on RT-Thread.
 *
 * The mapping is mostly mechanical, with three traps:
 *
 * 1. Priorities run in opposite directions. FreeRTOS counts up (higher value =
 *    more important), RT-Thread counts down. Both conversions live in
 *    ft_prio_to_rt()/ft_prio_to_ft() so the sign can never be flipped in one
 *    place and not another.
 *
 * 2. Stack sizes. FreeRTOS documents xTaskCreate's depth in words, but IDF's
 *    RISC-V port defines StackType_t as uint8_t, so the unit here is a byte and
 *    rt_thread_create wants bytes too - the conversion factor is
 *    FT_STACK_UNIT_BYTES (1). Getting this from the documentation instead of
 *    from portmacro.h gives a task four times the stack it asked for, which is
 *    quiet and wasteful rather than fatal, and that is exactly why it survives
 *    review.
 *
 * 3. rt_thread_control(RT_THREAD_CTRL_CHANGE_PRIORITY) dereferences its
 *    argument as a pointer to rt_uint8_t (src/thread.c:845), so the value has
 *    to be passed by address, not cast to void *.
 *
 * Thread local storage has no RT-Thread equivalent, so a small fixed side table
 * keyed by thread pointer stands in. configNUM_THREAD_LOCAL_STORAGE_POINTERS is
 * 2 in this configuration (CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS * 2),
 * and the table is sized well past any plausible task count; a thread that does
 * not fit simply gets NULL back rather than corrupting someone else's slot.
 */

#include <rtthread.h>

#include "sdkconfig.h"

/* IRAM_ATTR: part of the call tree that runs with the flash cache off.
 * See ft_task_scheduler_state(). */
#include "esp_attr.h"
#include "ft_core.h"

/* Bumped by libcpu/xtensa/cpuport.c the first time a core publishes a thread. */

#define FT_MAX_PRIORITIES   25      /* configMAX_PRIORITIES */
#define FT_TASK_TIMESLICE   10
#define FT_TLS_SLOTS        (CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS * 2)
/* Concurrent threads that can hold TLS, not total tasks ever created.
 *
 * 16 was too tight for what is coming: every pthread calls
 * pthread_setspecific() (components/pthread/pthread_local_storage.c), and
 * mbedTLS's PSA layer and the Wi-Fi stack both create pthreads. When the table
 * is full the write is dropped, pthread_getspecific() then returns NULL, and in
 * the _with_cb variant the deletion callback is dropped with it - so the
 * component's allocation leaks and its per-thread state reads back NULL.
 *
 * The cost is small and fixed: 64 x (2 x 4 B values + 2 x 4 B callbacks) is
 * 1 KiB, against a 128 KiB heap. The exit hook still frees a slot as soon as
 * the thread reaches the reaper, so this is capacity, not a leak. */
#define FT_TLS_THREADS      64

static void ft_task_exit_hook(struct rt_thread *thread);

/* Exit marks live further down; ft_task_create() has to clear a recycled
 * address before handing the thread out. */
static void ft_exit_mark_clear(rt_thread_t thread);

/*
 * The lowest RT-Thread priority band (RT_THREAD_PRIORITY_MAX - 1) belongs to the
 * idle thread, and only to it. Mapping a FreeRTOS task there would put it in
 * idle's band, where RT-Thread 5.x rotates equal priorities by timeslice - so a
 * tskIDLE_PRIORITY task, which in FreeRTOS means "run only when nothing else
 * can", would take every second tick away from idle. Idle is not decoration:
 * rt_defunct_execute() runs from it, so it is what frees finished threads and
 * their stacks.
 *
 * So the mapped band stops one short of idle: ft 0 -> MAX-2, ft 24 -> MAX-26.
 */
rt_uint8_t ft_prio_to_rt(ft_ubase_t prio)
{
    if (prio >= FT_MAX_PRIORITIES)
    {
        prio = FT_MAX_PRIORITIES - 1;
    }

    /* ft 0  -> RT_THREAD_PRIORITY_MAX - 2 (lowest assignable band, above idle)
     * ft 24 -> RT_THREAD_PRIORITY_MAX - 26 (highest of the mapped band) */
    return (rt_uint8_t)(RT_THREAD_PRIORITY_MAX - 2u - prio);
}

ft_ubase_t ft_prio_to_ft(rt_uint8_t prio)
{
    /* Anything at or below the mapped floor reports as the lowest FreeRTOS
     * priority. That includes the idle thread itself, which has no FreeRTOS
     * equivalent; reporting 0 for it is the least surprising answer. */
    if (prio >= RT_THREAD_PRIORITY_MAX - 1u)
    {
        return 0;
    }

    /* Clamp, because RT-Thread has more priority levels above the mapped band
     * (0..5) than FreeRTOS has total. uxTaskPriorityGet() on one of RT-Thread's
     * own high-priority threads - the timer thread is priority 0 - would
     * otherwise return 25..30, past configMAX_PRIORITIES, and callers do index
     * arrays with that value. */
    if (prio < RT_THREAD_PRIORITY_MAX - 1u - (FT_MAX_PRIORITIES - 1u))
    {
        return FT_MAX_PRIORITIES - 1u;
    }

    return (ft_ubase_t)(RT_THREAD_PRIORITY_MAX - 2u - prio);
}

/* -------------------------------------------------- pre-scheduler start -- */

/*
 * Tasks created before the scheduler is running cannot be started yet.
 *
 * rt_thread_startup() -> rt_thread_resume() -> rt_sched_insert_thread() walks
 * rt_thread_priority_table[], and that array is only initialised by
 * rt_system_scheduler_init(). Creating a task before it - which esp_timer does,
 * because esp_timer_init() now runs as a SECONDARY-stage init function - stores
 * into a NULL list head: a store access fault at address 4, inside
 * rt_list_insert_after (rtservice.h:63).
 *
 * rt_thread_create() itself is safe that early: it allocates, initialises the
 * thread object and its timer, and returns. Only *starting* it touches the
 * scheduler. So creation is allowed and startup is deferred to the first call
 * that runs with a scheduler, which is ft_task_self() from any created task or
 * the explicit flush in the boot path.
 *
 * The FIFO order matters for the timer task: esp_timer's alarm ISR looks up
 * s_timer_task by pointer, and the pointer is valid as soon as create returns,
 * so starting it late is invisible to the rest of esp_timer.
 */
#define FT_PENDING_TASKS    8

static rt_thread_t s_pending[FT_PENDING_TASKS];
static int         s_pending_core[FT_PENDING_TASKS];
static rt_uint32_t s_pending_count;

/* Called from the first context that has a scheduler. Idempotent. */
void ft_task_start_pending(void)
{
    rt_thread_t start[FT_PENDING_TASKS];
    rt_uint32_t n = 0;
    rt_uint32_t i;
    rt_base_t   level;

    /*
     * Snapshot the deferred list under a SHORT critical section, then start the
     * threads with interrupts unmasked.
     *
     * rt_thread_startup() can switch away - it makes the thread ready and, if it
     * outranks the caller, schedules it. Anything after that point in this
     * function runs on a DIFFERENT thread, so a matching
     * rt_hw_interrupt_enable(level) at the bottom would be executed by a thread
     * that never took the lock.
     *
     * On SMP rt_hw_interrupt_disable() IS rt_cpus_lock() (rthw.h:146), and its
     * slow path calls rt_enter_critical(), which raises the CALLING thread's
     * critical_lock_nest. Doing the startup inside that region means the nest is
     * raised on a thread that may not be the one to lower it.
     *
     * (The deeper cause of the stuck nest turned out to be elsewhere - a broken
     * S32C1I in libcpu/xtensa/atomic_xtensa.c made every rt_atomic_sub on a
     * non-zero word a silent no-op. But keeping startup out of the critical
     * section is still correct, and it makes the lock region short.)
     */
#ifdef RT_USING_SMP
    /*
     * Pin FIRST, with interrupts open.
     *
     * rt_thread_control(BIND_CPU) ends up in rt_sched_thread_bind_cpu(), which
     * takes the scheduler lock. Doing that inside rt_hw_interrupt_disable() is a
     * lock-order inversion on this port: under RT_USING_SMP that macro IS
     * rt_cpus_lock(), so it holds _cpus_lock and then asks for
     * _mp_scheduler_lock, while every other path takes them the other way round.
     * Measured: ft_task_start_pending() never returned, rt_components_init()
     * never reached finsh_system_init(), and the boot stopped at the banner with
     * no panic at all.
     */
    for (n = 0; n < s_pending_count && n < FT_PENDING_TASKS; n++)
    {
        if (s_pending[n] != RT_NULL &&
            s_pending_core[n] >= 0 && s_pending_core[n] < RT_CPUS_NR)
        {
            rt_thread_control(s_pending[n], RT_THREAD_CTRL_BIND_CPU,
                              (void *)(rt_ubase_t)s_pending_core[n]);
        }
    }
#endif

    n = 0;
    level = rt_hw_interrupt_disable();

    /* Copy the handles out, clear the table, and leave the critical section
     * BEFORE any of them can be started. */
    n = 0;
    for (i = 0; i < s_pending_count && i < FT_PENDING_TASKS; i++)
    {
        if (s_pending[i] != RT_NULL)
        {
            start[n++] = s_pending[i];
            s_pending[i] = RT_NULL;
        }
    }
    s_pending_count = 0;

    rt_hw_interrupt_enable(level);

    for (i = 0; i < n; i++)
    {
        rt_thread_startup(start[i]);
    }
}

/* Remove a thread from the pending table. Called when such a task is deleted
 * before the flush: otherwise ft_task_start_pending() would run
 * rt_thread_startup() on a freed object, which resumes a thread whose stack and
 * scheduler context have already been released. Reached from esp_timer's
 * init-failure path (esp_timer.c: deinit_timer_task -> vTaskDelete) at exactly
 * the SECONDARY stage where creation is deferred. */
static void ft_pending_drop(rt_thread_t thread)
{
    rt_base_t level;
    rt_uint32_t i;
    rt_uint32_t kept;

    level = rt_hw_interrupt_disable();

    kept = 0;
    for (i = 0; i < s_pending_count; i++)
    {
        if (s_pending[i] != thread && s_pending[i] != RT_NULL)
        {
            s_pending_core[kept] = s_pending_core[i];
            s_pending[kept++] = s_pending[i];
        }
    }

    s_pending_count = kept;

    rt_hw_interrupt_enable(level);
}

static rt_bool_t ft_task_scheduler_ready(void)
{
    /* rt_thread_self() is non-NULL only once rt_system_scheduler_start() has
     * switched to the first thread; that is the same test the scheduler-state
     * function uses, and the one esp_libc keys its "no locking yet" path off. */
    return (rt_thread_self() != RT_NULL) ? RT_TRUE : RT_FALSE;
}

void *ft_task_create(const char *name, void (*entry)(void *), void *param,
                     ft_ubase_t stack_words, ft_ubase_t prio, void **handle,
                     int core)
{
    rt_uint32_t stack_bytes = (rt_uint32_t)stack_words * FT_STACK_UNIT_BYTES;
    rt_thread_t thread;

    if (handle != RT_NULL)
    {
        *handle = RT_NULL;
    }

    if (entry == RT_NULL || stack_bytes < 256)
    {
        return RT_NULL;
    }

    thread = rt_thread_create(name ? name : "fttask", entry, param,
                              stack_bytes, ft_prio_to_rt(prio),
                              FT_TASK_TIMESLICE);
    if (thread == RT_NULL)
    {
        return RT_NULL;
    }

    /*
     * Every shim task runs on CPU0.
     *
     * The FreeRTOS entry point the IDF drivers use is xTaskCreatePinnedToCore(), and with
     * this port the pin is only advisory: rt_thread_create() leaves the thread unbound,
     * so either core may schedule it. Ordinary code copes; IDF components do not -
     * esp_wifi's task is not reentrant across cores. Measured with CPU1 scheduling on:
     * `s31wifi` prints "bringing up esp_wifi" and the board takes
     * rst:0x1a (CPU_LOCKUP_RESET).
     *
     * Binding here is the honest translation of "pinned to core 0", and it keeps the
     * shim's contract with the drivers it exists to run.
     */
#ifndef RT_USING_SMP
    RT_UNUSED(core);
#endif
#ifdef RT_USING_SMP
    /*
     * Bind to the requested core - but only once there is a scheduler to bind in.
     *
     * rt_thread_control(RT_THREAD_CTRL_BIND_CPU) goes through
     * rt_sched_thread_bind_cpu() -> rt_sched_lock() -> rt_cpu_self(), and
     * _cpus[] is only initialised by rt_system_scheduler_init(). esp_ipc_init()
     * is an __attribute__((constructor)), so it runs from __libc_init_array()
     * in start_cpu0_default() - BEFORE that call - and creates two tasks
     * through xTaskCreatePinnedToCore. Measured:
     *
     *   Guru Meditation Error: Core 0 panic'ed (LoadStoreError)
     *   PC : 0x4038b760  rt_sched_lock (scheduler_mp.c:571)
     *      <- rt_sched_thread_bind_cpu <- rt_thread_control <- ft_task_create
     *      <- xTaskCreatePinnedToCore <- esp_ipc_init
     *
     * So the bind joins the same deferral table as the startup itself and is
     * applied in ft_task_start_pending(), by which time
     * rt_system_scheduler_init() has run.
     */
    if (ft_task_scheduler_ready())
    {
        /* core < 0 means "any core": leave the thread unbound so either
         * scheduler may pick it. rt_thread_control() would reject a negative
         * cpu anyway, and binding to 0 would silently undo the request. */
        if (core >= 0 && core < RT_CPUS_NR)
        {
            rt_thread_control(thread, RT_THREAD_CTRL_BIND_CPU,
                              (void *)(rt_ubase_t)core);
        }
    }
#endif

    /* The allocator hands out recycled addresses. If this one belonged to a task
     * that has already exited, the mark left by its exit hook now describes the
     * new task, and a later vTaskDelete() of the old handle would be ignored -
     * or worse, the reverse. Only records still naming this exact pointer are
     * dropped; the exit hook owns the rest. */
    ft_exit_mark_clear(thread);

    /* Arm the TLS slot release for the case where the task function returns
     * instead of calling vTaskDelete(). See ft_task_exit_hook(). */
    thread->cleanup = ft_task_exit_hook;

    if (ft_task_scheduler_ready())
    {
        if (rt_thread_startup(thread) != RT_EOK)
        {
            rt_thread_delete(thread);
            return RT_NULL;
        }
    }
    else
    {
        /* Defer. The handle is still handed back, so a component that only
         * stores it - esp_timer keeps s_timer_task for its ISR to notify - sees
         * what it expects. */
        rt_base_t level = rt_hw_interrupt_disable();

        if (s_pending_count < FT_PENDING_TASKS)
        {
            s_pending_core[s_pending_count] = core;
            s_pending[s_pending_count++] = thread;
        }
        else
        {
            /* Out of slots. Starting now would corrupt the scheduler, so drop
             * the task instead and say so - a silent failure here would show up
             * as a component that never runs. */
            rt_hw_interrupt_enable(level);
            rt_kprintf("freertos-shim: %d tasks pending before the scheduler, "
                       "'%s' was dropped\n", FT_PENDING_TASKS, name ? name : "?");
            rt_thread_delete(thread);
            return RT_NULL;
        }

        rt_hw_interrupt_enable(level);
    }

    if (handle != RT_NULL)
    {
        *handle = thread;
    }

    return thread;
}

void ft_task_delay(ft_tick_t ticks)
{
    if (ticks == 0)
    {
        /* FreeRTOS vTaskDelay(0) is a yield, not a no-op. */
        rt_thread_yield();
        return;
    }

    /* Sleeping with the scheduler locked, or from an ISR, has to be refused -
     * the same rule ft_ipc.c enforces on every blocking call, and for the same
     * reason. rt_schedule() is a no-op while rt_scheduler_lock_nest is non-zero
     * (src/scheduler_up.c:296), so _thread_sleep() would suspend this thread,
     * fail to switch away, and return with the thread off the ready queue and
     * its status still set to suspend. The next reschedule parks it forever,
     * with no trace of where it stopped.
     *
     * FreeRTOS asserts here too (configASSERT( uxSchedulerSuspended == 0 ) in
     * vTaskDelay), so refusing is the port-compatible behaviour, not a
     * deviation. This path was missing the guard while every queue and
     * semaphore call had it. */
    if (!ft_block_allowed(ticks, "vTaskDelay"))
    {
        return;
    }

    /* portMAX_DELAY means "forever", and it used to reach
     * rt_thread_mdelay() by luck: 0xFFFFFFFF * 1000 / 1000 stays 0xFFFFFFFF,
     * the cast to rt_int32_t makes it -1, and rt_tick_from_millisecond()
     * happens to read a negative argument as RT_WAITING_FOREVER
     * (src/clock.c:218). That is three coincidences in a row, and the third one
     * is documented as "not a valid timeout", not as "sleep forever". Spell it
     * out instead, with the constant RT-Thread defines for exactly this. */
    if (ticks == FT_MAX_DELAY)
    {
        rt_thread_delay(RT_WAITING_FOREVER);
        return;
    }

    rt_thread_mdelay((rt_int32_t)(((rt_uint64_t)ticks * 1000u) / CONFIG_FREERTOS_HZ));
}

/* Defined with the TLS table further down; declared here because both
 * vTaskDelete and the exit hook have to release the per-slot callbacks. */
static void ft_tls_release(rt_thread_t thread);

/* ------------------------------------------------------------ exit marks -- */

/*
 * Threads whose entry function has returned.
 *
 * The status field is not enough to answer "is this task already gone?". On the
 * board, vTaskDelete() of a task that had already returned crashed inside
 * rt_timer_detach() -> rt_list_remove(), i.e. the status read back was NOT
 * RT_THREAD_CLOSE - because by then the idle thread had already called
 * rt_object_delete(), the thread object had been freed, and its memory had been
 * reused by something else. Reading `stat` from freed memory answers a question
 * about the new owner, not about the task.
 *
 * So the exit hook records the pointer while it is still unique, and
 * ft_task_create() drops any stale record when the allocator hands that address
 * out again. A delete that finds a mark does nothing: the task is already
 * finished and the idle thread owns the cleanup.
 */
#define FT_EXIT_MARKS       16

static rt_thread_t s_exited[FT_EXIT_MARKS];

static void ft_exit_mark_add(rt_thread_t thread)
{
    rt_base_t level = rt_hw_interrupt_disable();
    int i;

    for (i = 0; i < FT_EXIT_MARKS; i++)
    {
        if (s_exited[i] == thread)
        {
            break;                      /* already marked */
        }
        if (s_exited[i] == RT_NULL)
        {
            s_exited[i] = thread;
            break;
        }
    }

    rt_hw_interrupt_enable(level);
}

static void ft_exit_mark_clear(rt_thread_t thread)
{
    rt_base_t level = rt_hw_interrupt_disable();
    int i;

    for (i = 0; i < FT_EXIT_MARKS; i++)
    {
        if (s_exited[i] == thread)
        {
            s_exited[i] = RT_NULL;
        }
    }

    rt_hw_interrupt_enable(level);
}

/* 1 when the mark was present (and is now consumed). */
static rt_bool_t ft_exit_mark_take(rt_thread_t thread)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_bool_t found = RT_FALSE;
    int i;

    for (i = 0; i < FT_EXIT_MARKS; i++)
    {
        if (s_exited[i] == thread)
        {
            s_exited[i] = RT_NULL;
            found = RT_TRUE;
            break;
        }
    }

    rt_hw_interrupt_enable(level);

    return found;
}

/*
 * Release the TLS slot when a task function returns instead of calling
 * vTaskDelete.
 *
 * This is not an edge case. FreeRTOS allows a task function to simply return,
 * and IDF components do it: esp_event's loop task suspends itself, but a task
 * that finishes its work returns. RT-Thread handles the return by routing the
 * thread through _thread_exit() -> rt_thread_defunct_enqueue(), which never
 * reaches ft_task_delete() - so the slot stayed occupied forever.
 *
 * Two consequences, and the second is the dangerous one:
 *
 *   1. 16 slots, no reuse. The table fills up and later tasks silently get NULL
 *      from ft_task_get_tls(), which for esp_event means losing the per-task
 *      state it hangs off index 0.
 *
 *   2. rt_object_delete() frees the thread object (src/defunct.c:133), so the
 *      address is available for the next rt_thread_create(). ft_tls_lookup()
 *      compares pointers, so a new thread allocated at the same address
 *      inherits the dead task's TLS values AND its deletion callbacks. The
 *      callback then runs with a value that belongs to someone else.
 *
 * rt_thread_t::cleanup is the right hook: it is invoked from the reaper
 * (src/defunct.c:113) after the thread leaves the scheduler but before the
 * object is freed, so the slot is cleared while the pointer is still unique.
 * RT-Thread only ever zeroes this field itself, nothing else assigns it.
 */
static void ft_task_exit_hook(struct rt_thread *thread)
{
    /* Mark first: from here on a vTaskDelete() of this handle is a no-op rather
     * than a second trip through the defunct queue. */
    ft_exit_mark_add(thread);

    ft_tls_release(thread);

    /* Same lifetime problem, same window: the notification table is keyed by
     * thread pointer too, and a stale count makes a completion-counter wait
     * return early. */
    ft_notify_forget(thread);
}

void ft_task_delete(void *handle)
{
    rt_thread_t thread = handle ? (rt_thread_t)handle : rt_thread_self();

    /* FreeRTOS runs the TLSP deletion callbacks from prvDeleteTCB(), i.e. as
     * part of the delete. IDF's esp_event relies on that to free the per-task
     * state it hangs off index 0.
     *
     * Released here as well as from the exit hook: this path runs immediately,
     * while the hook waits for the idle thread to drain the defunct list. Doing
     * both is safe - the second call finds no slot and returns. */
    ft_tls_release(thread);
    ft_notify_forget(thread);

    /*
     * A task that already returned from its entry function must not be enqueued
     * a second time.
     *
     * RT-Thread closes such a thread inside _thread_exit() and puts it on the
     * defunct queue; the idle thread then calls rt_object_delete() on it, which
     * frees the thread object AND its stack. rt_thread_delete() on that stale
     * handle runs _thread_detach() -> rt_thread_defunct_enqueue() again, so the
     * same memory is queued twice and freed twice.
     *
     * Two crashes on the board came from this, and the second one is why the
     * check below is a pointer mark rather than a status test:
     *
     *   1. rt_smem_free -> rt_object_get_type, MTVAL=0x0c, A0=0 - the freed
     *      block's pool_ptr had already been cleared to MEM_FREED.
     *   2. rt_timer_detach -> rt_list_remove, MTVAL=0xffff0007 - by the time the
     *      delete ran, the thread object had been freed AND its memory reused,
     *      so `stat` no longer belonged to this task. A status test passes.
     *
     * FreeRTOS has the same rule - deleting a task that has already deleted
     * itself is a use-after-free - but it matters more here because esp_event's
     * loop task and esp_timer's task both *return* to exit. */
    if (ft_exit_mark_take(thread))
    {
        return;
    }

    /* Deleting a task that was created before the scheduler and has not been
     * started yet: drop it from the pending table first, or the flush at
     * INIT_COMPONENT will start a freed thread. */
    ft_pending_drop(thread);

    /* RT-Thread 5.x has no public rt_thread_exit(); rt_thread_delete() accepts
     * the calling thread and hands it to the reaper (src/thread.c:614 routes
     * through _thread_detach), which is what vTaskDelete(NULL) means.
     *
     */
    rt_thread_delete(thread);

    /*
     * vTaskDelete(NULL) must NOT return.
     *
     * rt_thread_delete() -> _thread_detach() only performs a context switch if
     * a switch is already pending (scheduler_up.c: IS_CRITICAL_SWITCH_PEND),
     * and rt_thread_close() never requests one. So deleting the *calling* thread
     * returns to a caller that is already on the defunct queue - and when its
     * entry function then returns, _thread_exit() enqueues the same list node a
     * second time. A node that is already linked re-inserts onto itself
     * (A->next = A->prev = A), rt_defunct_execute() then dequeues it forever,
     * and the idle thread never does anything else again.
     *
     * FreeRTOS's contract is explicit that vTaskDelete(NULL) does not return
     * (task.h), and components rely on it: esp_console's REPL task ends with it,
     * esp_vfs_l2tap's cleanup task too. So park here instead. The thread object
     * is already closed and queued; a freed thread must never be scheduled
     * again, which is exactly what suspend-then-reschedule gives us. */
    if (thread == rt_thread_self())
    {
        for (;;)
        {
            rt_thread_suspend(thread);
            rt_schedule();
            /* If the idle thread ever woke us, something is wrong - but falling
             * through to another delete would corrupt the queue, so loop. */
        }
    }
}

void ft_task_suspend(void *handle)
{
    rt_thread_t thread = handle ? (rt_thread_t)handle : rt_thread_self();

    /* Same guard as every other blocking call: with the scheduler locked
     * rt_schedule() is a no-op (scheduler_up.c:296), so the caller would leave
     * the ready queue and never be resumed - status still "suspend", no trace.
     * FreeRTOS asserts on the same condition in vTaskSuspend. */
    if (!ft_block_allowed(1, "vTaskSuspend"))
    {
        return;
    }

    if (rt_thread_suspend(thread) == RT_EOK && thread == rt_thread_self())
    {
        rt_schedule();
    }
}

void ft_task_set_priority(void *handle, ft_ubase_t prio)
{
    rt_thread_t thread = handle ? (rt_thread_t)handle : rt_thread_self();
    rt_uint8_t  new_prio = ft_prio_to_rt(prio);

    /* RESET_PRIORITY, not CHANGE_PRIORITY.
     *
     * CHANGE_PRIORITY maps to _rt_sched_update_priority(..., update_init_prio =
     * RT_FALSE): it rewrites current_priority and leaves init_priority at the
     * value passed to rt_thread_create. Every priority-inheritance recomputation
     * starts from the *initial* priority - _thread_get_mutex_priority() seeds
     * `priority = rt_sched_thread_get_init_prio(thread)` (src/ipc.c:852) - so the
     * first time the task hands back a mutex, _check_and_update_prio() resets it
     * to its creation-time priority. A caller that raises a task's priority and
     * then uses any mutex silently loses the raise.
     *
     * RESET_PRIORITY routes to rt_sched_thread_reset_priority(update_init_prio =
     * RT_TRUE), which is what vTaskPrioritySet means: it moves both uxPriority
     * and uxBasePriority.
     *
     * By address: rt_thread_control dereferences the argument. */
    rt_thread_control(thread, RT_THREAD_CTRL_RESET_PRIORITY, &new_prio);
}

ft_ubase_t ft_task_get_priority(void *handle)
{
    rt_thread_t thread = handle ? (rt_thread_t)handle : rt_thread_self();

    /* Single line, no SMP guard: sched_thread_priv (and its current_priority) is
     * declared unconditionally in rtsched.h, unlike the SMP-only fields this
     * file guards elsewhere. An earlier attempt to "fix" this for single core by
     * reading thread->current_priority was wrong - struct rt_thread has no such
     * member; the field lives in sched_thread_ctx.sched_thread_priv. */
    return ft_prio_to_ft(RT_SCHED_PRIV(thread).current_priority);
}

void *ft_task_self(void)
{
    return rt_thread_self();
}

ft_tick_t ft_task_tick_count(void)
{
    return (ft_tick_t)rt_tick_get();
}

/* ------------------------------------------------------------------- TLS -- */

struct ft_tls_entry
{
    rt_thread_t thread;
    void       *value[FT_TLS_SLOTS];
    ft_tls_del_cb_t cb[FT_TLS_SLOTS];
};

static struct ft_tls_entry s_tls[FT_TLS_THREADS];

/* How many slots are occupied. Exists for the self test: the release path only
 * fires when a task function returns or a task is deleted, and neither is
 * visible any other way without an on-board run. */
rt_uint32_t ft_tls_slots_used(void)
{
    rt_uint32_t used = 0;
    rt_base_t   level;
    int         i;

    level = rt_hw_interrupt_disable();
    for (i = 0; i < FT_TLS_THREADS; i++)
    {
        if (s_tls[i].thread != RT_NULL)
        {
            used++;
        }
    }
    rt_hw_interrupt_enable(level);

    return used;
}

/* Reported once, from both TLS writers. A missing slot is not something to
 * absorb quietly: the value write disappears, and in the with_cb variant the
 * deletion callback goes with it, so whatever the component allocated is never
 * freed. */
static void ft_tls_overflow_report(void)
{
    static rt_uint32_t warned;

    if (!warned)
    {
        warned = 1;
        rt_kprintf("freertos-shim: TLS table full (%d threads); TLS writes are "
                   "being dropped\n", FT_TLS_THREADS);
    }
}

static struct ft_tls_entry *ft_tls_lookup(rt_thread_t thread, rt_bool_t create)
{
    struct ft_tls_entry *free_entry = RT_NULL;
    int i;

    if (thread == RT_NULL)
    {
        return RT_NULL;
    }

    for (i = 0; i < FT_TLS_THREADS; i++)
    {
        if (s_tls[i].thread == thread)
        {
            return &s_tls[i];
        }
        if (free_entry == RT_NULL && s_tls[i].thread == RT_NULL)
        {
            free_entry = &s_tls[i];
        }
    }

    if (!create || free_entry == RT_NULL)
    {
        return RT_NULL;
    }

    rt_memset(free_entry->value, 0, sizeof(free_entry->value));
    rt_memset(free_entry->cb, 0, sizeof(free_entry->cb));
    free_entry->thread = thread;

    return free_entry;
}

void ft_task_set_tls(void *handle, ft_ubase_t index, void *value)
{
    rt_thread_t thread = handle ? (rt_thread_t)handle : rt_thread_self();
    rt_base_t   level;
    struct ft_tls_entry *entry;

    if (index >= FT_TLS_SLOTS)
    {
        return;
    }

    level = rt_hw_interrupt_disable();
    entry = ft_tls_lookup(thread, RT_TRUE);
    if (entry != RT_NULL)
    {
        entry->value[index] = value;
    }
    else
    {
        /* Table full, or the thread cannot be represented. Never silent: a TLS
         * write that vanishes is only noticed much later, as component state
         * that reads back as NULL. */
        ft_tls_overflow_report();
    }
    rt_hw_interrupt_enable(level);
}

void ft_task_set_tls_with_cb(void *handle, ft_ubase_t index, void *value,
                             ft_tls_del_cb_t cb)
{
    rt_thread_t thread = handle ? (rt_thread_t)handle : rt_thread_self();
    rt_base_t   level;
    struct ft_tls_entry *entry;

    if (index >= FT_TLS_SLOTS)
    {
        return;
    }

    level = rt_hw_interrupt_disable();
    entry = ft_tls_lookup(thread, RT_TRUE);
    if (entry != RT_NULL)
    {
        entry->value[index] = value;
        entry->cb[index]    = cb;
    }
    else
    {
        /* Worse than the plain set failing: the deletion callback is how the
         * component frees what value points at, so dropping it here leaks. */
        ft_tls_overflow_report();
    }
    rt_hw_interrupt_enable(level);
}

/* Runs the callbacks and drops the slot. The callbacks are invoked with
 * interrupts enabled: they are component code and may malloc/free. */
static void ft_tls_release(rt_thread_t thread)
{
    struct ft_tls_entry *entry;
    ft_tls_del_cb_t      cb;
    void                *value;
    rt_base_t            level;
    int                  i;

    level = rt_hw_interrupt_disable();
    entry = ft_tls_lookup(thread, RT_FALSE);

    if (entry == RT_NULL)
    {
        rt_hw_interrupt_enable(level);
        return;
    }

    for (i = 0; i < FT_TLS_SLOTS; i++)
    {
        if (entry->value[i] == RT_NULL || entry->cb[i] == RT_NULL)
        {
            continue;
        }

        cb    = entry->cb[i];
        value = entry->value[i];

        entry->value[i] = RT_NULL;
        entry->cb[i]    = RT_NULL;

        rt_hw_interrupt_enable(level);
        cb(i, value);
        level = rt_hw_interrupt_disable();
    }

    entry->thread = RT_NULL;
    rt_hw_interrupt_enable(level);
}

void *ft_task_get_tls(void *handle, ft_ubase_t index)
{
    rt_thread_t thread = handle ? (rt_thread_t)handle : rt_thread_self();
    rt_base_t   level;
    struct ft_tls_entry *entry;
    void *value = RT_NULL;

    if (index >= FT_TLS_SLOTS)
    {
        return RT_NULL;
    }

    level = rt_hw_interrupt_disable();
    entry = ft_tls_lookup(thread, RT_FALSE);
    if (entry != RT_NULL)
    {
        value = entry->value[index];
    }
    rt_hw_interrupt_enable(level);

    return value;
}

ft_tick_t ft_task_tick_count_isr(void)
{
    return (ft_tick_t)rt_tick_get();
}

/* Depth of vTaskSuspendAll() nesting. Separate from rt_critical_level() on
 * purpose - see ft_task_scheduler_state(). */
static rt_uint32_t s_suspend_all_depth;

ft_base_t IRAM_ATTR ft_task_scheduler_state(void)
{
    /* Three states, and NOT_STARTED is not decorative.
     *
     * esp_libc's lock layer uses it as its "no locking yet" escape hatch
     * (components/esp_libc/src/locks.c:128 and :137): before the scheduler is up
     * a lock is a no-op rather than a blocking call. If this function never
     * reports NOT_STARTED, the very first libc lock taken during CORE init falls
     * through to the ISR branch instead, because xPortCanYield() is false while
     * rt_thread_self() is still NULL - and that branch aborts on a recursive
     * mutex ("recursive mutexes make no sense in ISR context", locks.c:150).
     *
     * rt_thread_self() is the honest test: RT-Thread's _rt_scheduler_stack_check
     * keeps it non-NULL only once a thread is running, and the CORE-stage init
     * functions do run before that. */
    /*
     * "Has a thread started running on this core" - asked of the PORT, not of
     * pcpu->current_thread.
     *
     * rt_thread_self() reads _cpus[rt_hw_cpu_id()].current_thread, and on a
     * dual-core bring-up that word stays NULL for a surprisingly long time: the
     * second core comes up late, and until the first _rtt_resume() on a core
     * publishes a thread, the field is zero. Measured with a counter in this
     * function on the SMP build: 126 calls, rt_cpu_self() == 0x3fcad920 (the
     * real &_cpus[0]), and current_thread == 0 on every one of them - while
     * `s3smp`, running later, read `tshell` out of that same address.
     *
     * The consequence was not cosmetic. esp_libc's lock layer treats
     * NOT_STARTED as "locks are no-ops yet" (locks.c:129 and :138), so every
     * libc lock taken in that window returned success WITHOUT taking anything.
     * Measured: 21 lock acquisitions, 0 of them actually locked. nvs_flash_init()
     * then ran its flash sequence with no mutual exclusion at all and hung.
     *
     * rt_xt_running_count is bumped by the port on every published switch (see
     * rt_xt_stat_post_switch), so it answers the question the caller is really
     * asking and does not depend on a field that can still be NULL.
     */
    if (ft_port_running_count() == 0)
    {
        return FT_SCHEDULER_NOT_STARTED;
    }

    /*
     * SUSPENDED only between vTaskSuspendAll()/xTaskResumeAll() - NOT whenever
     * rt_critical_level() is nonzero.
     *
     * Those are two different nests. vPortEnterCritical() also calls
     * rt_enter_critical(), so keying off rt_critical_level() reports SUSPENDED
     * for every driver that guards a register write with portENTER_CRITICAL.
     * Callers use `== taskSCHEDULER_RUNNING` as "may I yield", and
     * spi_flash_os_func_app.c's spi_flash_os_yield() does exactly that: inside a
     * cache-disabled flash window the erase yield then stops yielding, which
     * stretches the window in which the 300 ms interrupt watchdog and the 5 s
     * task watchdog are being run against a stalled idle.
     *
     * FreeRTOS's own answer is about the suspend-all nest alone, so a shim-owned
     * counter is the truthful source. rt_critical_level() is not consulted.
     */
    return s_suspend_all_depth ? FT_SCHEDULER_SUSPENDED : FT_SCHEDULER_RUNNING;
}

void ft_task_suspend_all(void)
{
    rt_enter_critical();
    s_suspend_all_depth++;
}

ft_base_t ft_task_resume_all(void)
{
    if (s_suspend_all_depth != 0)
    {
        s_suspend_all_depth--;
    }

    rt_exit_critical();

    /* FreeRTOS returns pdTRUE when a switch is pending so the caller can
     * yield immediately. RT-Thread's exit already reschedules when needed, so
     * the value is only a hint; answering "yes" costs at most one extra
     * scheduler pass and never delays a waiting task. */
    return ftPASS;
}


