/*
 * Task introspection on RT-Thread.
 *
 * Every function here is declared unconditionally in task.h, so any component in
 * the tree compiles against them - while tasks.c, the only definition, is not
 * compiled when the shim is on. That combination is invisible until the first
 * caller appears: it compiles fine and fails at link time, in a file nobody
 * touched.
 *
 * None of these had a caller in this image yet. That is the reason this file
 * exists rather than a note saying "add it when needed": the cost of writing
 * them now is a dozen lines each, and the cost of not writing them is a link
 * failure in someone else's component at the least convenient moment.
 */

#include <rtthread.h>

#include "sdkconfig.h"
#include "ft_core.h"

/* Number of threads RT-Thread knows about, idle included. Walks the object
 * list rather than keeping a counter: a counter would have to be maintained on
 * every create and delete path in the kernel, and this is a diagnostic, not a
 * hot path. */
ft_ubase_t ft_task_count(void)
{
    struct rt_object_information *info;
    rt_list_t *node;
    ft_ubase_t n = 0;
    rt_base_t  level;

    info = rt_object_get_information(RT_Object_Class_Thread);
    if (info == RT_NULL)
    {
        return 0;
    }

    level = rt_spin_lock_irqsave(&info->spinlock);
    rt_list_for_each(node, &info->object_list)
    {
        n++;
    }
    rt_spin_unlock_irqrestore(&info->spinlock, level);

    return n;
}

const char *ft_task_name(void *handle)
{
    rt_thread_t thread = handle ? (rt_thread_t)handle : rt_thread_self();

    return (thread != RT_NULL) ? thread->parent.name : RT_NULL;
}

void *ft_task_find(const char *name)
{
    /* rt_thread_find() takes a non-const char * and passes it to
     * rt_object_find(), which only ever compares it. The cast is the price of
     * a prototype that predates const-correctness in this tree. */
    return (name != RT_NULL) ? (void *)rt_thread_find((char *)name) : RT_NULL;
}

/* The idle thread has no FreeRTOS equivalent to hand out, but callers use this
 * handle to name the idle task in diagnostics and to skip it when registering
 * for watchdogs, so returning it is more useful than returning NULL. */
void *ft_task_idle_handle(void)
{
    struct rt_cpu *cpu = rt_cpu_index(0);

    return (cpu != RT_NULL) ? (void *)cpu->idle_thread : RT_NULL;
}

/*
 * eTaskState, from RT-Thread's thread.state.
 *
 * The only non-obvious case is eBlocked vs eSuspended. FreeRTOS reports a task
 * waiting with an infinite timeout as eSuspended, because its scheduler cannot
 * tell that apart from an explicit suspend. RT-Thread does distinguish them, by
 * whether the thread timer was armed - a bounded wait arms it, an unbounded wait
 * does not - and that lands on the same answer by a different route.
 */
rt_uint32_t ft_task_state(void *handle)
{
    rt_thread_t thread = handle ? (rt_thread_t)handle : rt_thread_self();
    rt_uint8_t  stat;

    /* Values match the order of eTaskState in task.h. */
    enum
    {
        FT_STATE_RUNNING = 0,
        FT_STATE_READY,
        FT_STATE_BLOCKED,
        FT_STATE_SUSPENDED,
        FT_STATE_DELETED,
        FT_STATE_INVALID,
    };

    if (thread == RT_NULL)
    {
        return FT_STATE_INVALID;
    }

    stat = RT_SCHED_CTX(thread).stat & RT_THREAD_STAT_MASK;

    switch (stat)
    {
    case RT_THREAD_RUNNING:
        /* Only the caller can be RUNNING; anyone else in that band has been
         * preempted and is really ready. */
        return (thread == rt_thread_self()) ? FT_STATE_RUNNING : FT_STATE_READY;

    case RT_THREAD_READY:
        return FT_STATE_READY;

    case RT_THREAD_CLOSE:
        return FT_STATE_DELETED;

    case RT_THREAD_INIT:
        return FT_STATE_READY;      /* created, not yet started */

    default:
        /* RT_THREAD_SUSPEND_MASK plus a reason. An armed timer means the wait
         * has a deadline, i.e. FreeRTOS's eBlocked; no timer means the thread is
         * parked until someone resumes it, i.e. eSuspended. */
        return RT_SCHED_CTX(thread).sched_flag_ttmr_set ? FT_STATE_BLOCKED
                                                       : FT_STATE_SUSPENDED;
    }
}

/* xTaskAbortDelay: make a task stop waiting, as if its wait had expired.
 *
 * rt_thread_resume() is the whole mechanism - it takes the thread off the
 * suspend list and puts it back on the ready list, which is what a timeout does
 * too. Nothing else is needed to make the abandoned wait report failure:
 * _rt_sem_take() sets thread->error = RT_EINTR before suspending
 * (src/ipc.c:604) and returns it if nothing cleared it, so the blocked call
 * comes back with an error rather than as a phantom success. Setting the error
 * here would actually be wrong - it would have to be set before the thread runs
 * again, and there is no point in this function where that is safe.
 *
 * Returns pdPASS when a thread was actually resumed, pdFAIL when it was not
 * suspended to begin with, which is FreeRTOS's contract.
 */
ft_base_t ft_task_abort_delay(void *handle)
{
    rt_thread_t thread = handle ? (rt_thread_t)handle : rt_thread_self();

    if (thread == RT_NULL)
    {
        return ftFAIL;
    }

    return (rt_thread_resume(thread) == RT_EOK) ? ftPASS : ftFAIL;
}

/* xTaskResumeFromISR.
 *
 * RT-Thread defers the actual switch to the outermost trap exit, so there is
 * nothing to yield to here: the flag only tells the caller whether a thread was
 * actually made ready, i.e. whether its own exit path has anything to do. */
ft_base_t ft_task_resume_isr(void *handle, ft_base_t *woken)
{
    rt_thread_t thread = handle ? (rt_thread_t)handle : rt_thread_self();
    rt_err_t    rc;

    if (woken != RT_NULL)
    {
        *woken = ftFAIL;
    }

    if (thread == RT_NULL)
    {
        return ftFAIL;
    }

    rc = rt_thread_resume(thread);

    if (rc == RT_EOK && woken != RT_NULL)
    {
        *woken = ftPASS;
    }

    return (rc == RT_EOK) ? ftPASS : ftFAIL;
}

/* xTaskDelayUntil, in ticks.
 *
 * rt_thread_delay_until() has the same contract, down to advancing the wake time
 * for the next call, so the work here is only the unit conversion and the
 * return value. The wake time stays in FreeRTOS ticks on the caller's side,
 * which means it has to be translated in and back out; doing that lossily would
 * accumulate drift in exactly the periodic loop this function exists to keep
 * exact.
 */
ft_base_t ft_task_delay_until(ft_tick_t *prev, ft_tick_t increment)
{
    rt_tick_t period;
    rt_tick_t deadline;
    rt_tick_t now;
    ft_base_t on_time;

    if (prev == RT_NULL || increment == 0)
    {
        return ftFAIL;
    }

    /* Round up: a caller asking for one FreeRTOS tick must not get a zero-tick
     * delay, which RT-Thread would reject outright (src/thread.c:662). */
    period = (rt_tick_t)(((rt_uint64_t)increment * RT_TICK_PER_SECOND
                          + CONFIG_FREERTOS_HZ - 1) / CONFIG_FREERTOS_HZ);
    if (period == 0)
    {
        period = 1;
    }

    deadline = (rt_tick_t)(((rt_uint64_t)(*prev) * RT_TICK_PER_SECOND)
                           / CONFIG_FREERTOS_HZ);
    now      = rt_tick_get();

    /* FreeRTOS tests the *next* wake time against now (tasks.c:1509-1540:
     * `xTimeToWake = *pxPreviousWakeTime + xTimeIncrement; ... pdFALSE if the
     * time to wake is in the past`). Testing the previous wake time instead
     * reports pdFALSE - "I missed my deadline" - for a periodic task that is
     * merely late by less than one period, which is exactly the case the
     * function exists to keep exact. */
    on_time = ((rt_int32_t)(now - (deadline + period)) < 0) ? ftPASS : ftFAIL;

    /* Same guard as every other blocking call - see ft_block_allowed(). A delay
     * with the scheduler locked parks the caller with no way back. */
    if (ft_block_allowed(increment, "xTaskDelayUntil"))
    {
        rt_thread_delay_until(&deadline, period);
    }

    *prev = (ft_tick_t)(((rt_uint64_t)deadline * CONFIG_FREERTOS_HZ)
                        / RT_TICK_PER_SECOND);

    return on_time;
}
