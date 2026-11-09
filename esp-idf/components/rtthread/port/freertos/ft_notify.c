/*
 * Task notifications and timeout bookkeeping on RT-Thread.
 *
 * Why this is not part of ft_task.c: notifications need a per-task wakeup
 * object, and a side table keyed by thread is the only place to put it because
 * TaskHandle_t here is the rt_thread_t itself.
 *
 * The notification is a counting semaphore. That covers both idioms FreeRTOS
 * uses it for:
 *
 *   - stream_buffer.c (reached through esp_ringbuf, which esp_driver_uart
 *     pulls in) wakes a blocked peer with xTaskNotify(eNoAction) and waits with
 *     xTaskNotifyWait, so only the semaphore count matters.
 *   - esp_timer, esp_eth and esp_ipc count completions with xTaskNotifyGive and
 *     ulTaskNotifyTake, which is the counting case.
 *
 * The bit payload lives beside the semaphore in `value` and is only touched by
 * eSetBits / eSetValue.
 *
 * rt_sem_init() on a static object allocates nothing, which is what makes it
 * legal for the first notifier to be an ISR.
 */

#include <rthw.h>
#include <rtthread.h>

#include "sdkconfig.h"
#include "ft_core.h"

/* Concurrent threads that can be notified, not total tasks ever created.
 *
 * Task notifications are the cheapest blocking primitive esp_timer, esp_eth and
 * esp_ipc have, and stream buffers do their whole protocol with them - so the
 * count that matters is "how many tasks can be waiting on a notification at
 * once". 16 was tight enough that Wi-Fi plus a few drivers could exhaust it, and
 * the failure mode is a dropped wakeup that looks exactly like a slow peer.
 *
 * 64 x (rt_semaphore + two words) is about 3 KiB against a 128 KiB heap, and
 * the exit hook still frees an entry when the task is reaped. */
#define FT_NOTIFY_TASKS     64

struct ft_notify
{
    rt_thread_t          thread;
    struct rt_semaphore  sem;
    rt_uint32_t          value;
};

static struct ft_notify s_notify[FT_NOTIFY_TASKS];
static rt_bool_t        s_notify_ready;

/*
 * Drop a thread's notification slot.
 *
 * The table has the same lifetime problem as the TLS one: entries are created on
 * demand and keyed by rt_thread_t, and nothing released them. The slot is not
 * just wasted table space - rt_object_delete() frees the thread object
 * (src/defunct.c:133) and the address is reused by the next rt_thread_create(),
 * so a new thread can inherit a dead thread's pending count. For a task that
 * uses xTaskNotifyGive/ulTaskNotifyTake as a completion counter, inheriting a
 * stale count means one wait returns immediately without its work being done.
 *
 * Called from ft_task.c's exit hook, before the object is freed, which is the
 * only window where the pointer is still unique.
 */
void ft_notify_forget(rt_thread_t thread)
{
    int i;
    rt_base_t level;

    if (thread == RT_NULL || !s_notify_ready)
    {
        return;
    }

    level = rt_hw_interrupt_disable();
    for (i = 0; i < FT_NOTIFY_TASKS; i++)
    {
        if (s_notify[i].thread == thread)
        {
            s_notify[i].value  = 0;
            s_notify[i].thread = RT_NULL;

            /* Reset the count. The semaphore object itself stays initialised -
             * rt_sem_detach() would free the internal ipc object, and this slot
             * is meant to be reusable without re-initialising. Writers go
             * through ft_notify_lookup() which clears the value but not the
             * count, so clear it here where it is still safe. */
            s_notify[i].sem.value = 0;
            break;
        }
    }
    rt_hw_interrupt_enable(level);
}

/* Called with interrupts disabled by every caller. */
static struct ft_notify *ft_notify_lookup(rt_thread_t thread, rt_bool_t create)
{
    struct ft_notify *free_entry = RT_NULL;
    int i;

    if (thread == RT_NULL)
    {
        return RT_NULL;
    }

    if (!s_notify_ready)
    {
        if (!create)
        {
            return RT_NULL;
        }

        for (i = 0; i < FT_NOTIFY_TASKS; i++)
        {
            rt_sem_init(&s_notify[i].sem, "ftntf", 0, RT_IPC_FLAG_PRIO);
            s_notify[i].thread = RT_NULL;
            s_notify[i].value  = 0;
        }

        s_notify_ready = RT_TRUE;
    }

    for (i = 0; i < FT_NOTIFY_TASKS; i++)
    {
        if (s_notify[i].thread == thread)
        {
            return &s_notify[i];
        }
        if (free_entry == RT_NULL && s_notify[i].thread == RT_NULL)
        {
            free_entry = &s_notify[i];
        }
    }

    if (!create || free_entry == RT_NULL)
    {
        /* Table full, and the caller is about to lose a wakeup. Silent would be
         * the worst answer: a notification that goes nowhere looks exactly like
         * a slow peer, so the bug gets chased in the wrong component. The TLS
         * table reports the same condition the same way. */
        if (create && free_entry == RT_NULL)
        {
            static rt_uint32_t warned;

            if (!warned)
            {
                warned = 1;
                rt_kprintf("freertos-shim: notification table full (%d tasks); "
                           "notifications are being dropped\n", FT_NOTIFY_TASKS);
            }
        }
        return RT_NULL;
    }

    free_entry->value  = 0;
    free_entry->thread = thread;

    return free_entry;
}

int ft_task_notify(void *handle, rt_uint32_t value, ft_base_t action)
{
    rt_thread_t thread = handle ? (rt_thread_t)handle : rt_thread_self();
    struct ft_notify *e;
    rt_base_t level;
    int rc = FT_OK;

    /* The value update has to be atomic against an ISR notifying the same task,
     * but rt_sem_release() must run with interrupts on: it can reschedule. */
    level = rt_hw_interrupt_disable();
    e = ft_notify_lookup(thread, RT_TRUE);

    if (e == RT_NULL)
    {
        rt_hw_interrupt_enable(level);
        return FT_ERR_PARAM;
    }

    switch (action)
    {
    case FT_NOTIFY_SET_BITS:
        e->value |= value;
        break;

    case FT_NOTIFY_INCREMENT:
        e->value++;
        break;

    case FT_NOTIFY_SET_VALUE_OVERWRITE:
        e->value = value;
        break;

    case FT_NOTIFY_SET_VALUE_NO_OVERWRITE:
        if (e->sem.value != 0)
        {
            rc = FT_ERR_STATE;
        }
        else
        {
            e->value = value;
        }
        break;

    default:                    /* FT_NOTIFY_NO_ACTION keeps the value */
        break;
    }

    rt_hw_interrupt_enable(level);

    if (rc == FT_OK)
    {
        rt_sem_release(&e->sem);
    }

    return rc;
}

int ft_task_notify_isr(void *handle, rt_uint32_t value, ft_base_t action,
                       ft_base_t *woken)
{
    int rc = ft_task_notify(handle, value, action);

    /* rt_sem_release() has already made the task ready; the caller only uses
     * this flag to decide whether to yield on the way out of the ISR. */
    if (woken != RT_NULL && rc == FT_OK)
    {
        *woken = 1;
    }

    return rc;
}

rt_uint32_t ft_task_notify_wait(rt_uint32_t clear_on_entry,
                                rt_uint32_t clear_on_exit,
                                ft_tick_t ticks, ft_base_t *timed_out)
{
    struct ft_notify *e;
    rt_base_t level;
    rt_uint32_t rv = 0;

    if (timed_out != RT_NULL)
    {
        *timed_out = 0;
    }

    if (!ft_block_allowed(ticks, "xTaskNotifyWait"))
    {
        if (timed_out != RT_NULL)
        {
            *timed_out = 1;
        }
        return 0;
    }

    level = rt_hw_interrupt_disable();
    e = ft_notify_lookup(rt_thread_self(), RT_TRUE);
    if (e != RT_NULL && clear_on_entry != 0)
    {
        e->value &= ~clear_on_entry;
    }
    rt_hw_interrupt_enable(level);

    if (e == RT_NULL)
    {
        return 0;
    }

    if (rt_sem_take(&e->sem, ft_ticks_to_rt(ticks)) != RT_EOK)
    {
        if (timed_out != RT_NULL)
        {
            *timed_out = 1;
        }
        return 0;
    }

    level = rt_hw_interrupt_disable();
    rv = e->value;
    if (clear_on_exit != 0)
    {
        e->value &= ~clear_on_exit;
    }
    rt_hw_interrupt_enable(level);

    return rv;
}

rt_uint32_t ft_task_notify_clear(void *handle)
{
    rt_thread_t thread = handle ? (rt_thread_t)handle : rt_thread_self();
    struct ft_notify *e;
    rt_base_t level;
    rt_uint32_t rv = 0;

    level = rt_hw_interrupt_disable();
    e = ft_notify_lookup(thread, RT_FALSE);
    if (e != RT_NULL)
    {
        rv = e->value;
        e->value = 0;
        e->sem.value = 0;
    }
    rt_hw_interrupt_enable(level);

    return rv;
}

rt_uint32_t ft_task_notify_peek(void *handle)
{
    rt_thread_t thread = handle ? (rt_thread_t)handle : rt_thread_self();
    struct ft_notify *e;
    rt_base_t level;
    rt_uint32_t rv = 0;

    level = rt_hw_interrupt_disable();
    e = ft_notify_lookup(thread, RT_FALSE);
    if (e != RT_NULL)
    {
        rv = e->value;
    }
    rt_hw_interrupt_enable(level);

    return rv;
}

rt_uint32_t ft_task_notify_take(ft_base_t clear_on_exit, ft_tick_t ticks)
{
    struct ft_notify *e;
    rt_base_t level;
    rt_uint32_t rv;

    if (!ft_block_allowed(ticks, "ulTaskNotifyTake"))
    {
        return 0;
    }

    level = rt_hw_interrupt_disable();
    e = ft_notify_lookup(rt_thread_self(), RT_TRUE);
    rt_hw_interrupt_enable(level);

    if (e == RT_NULL)
    {
        return 0;
    }

    if (rt_sem_take(&e->sem, ft_ticks_to_rt(ticks)) != RT_EOK)
    {
        return 0;
    }

    /* rt_sem_take already decremented; the value FreeRTOS reports is the count
     * before this take, i.e. one more than what is left. Writing sem.value
     * directly is safe here: we are the only taker that got through. */
    level = rt_hw_interrupt_disable();
    rv = (rt_uint32_t)e->sem.value + 1u;
    if (clear_on_exit)
    {
        e->sem.value = 0;
    }
    rt_hw_interrupt_enable(level);

    return rv;
}

/* --------------------------------------------------------------- timeouts -- */

static ft_tick_t ft_now(void)
{
    return (ft_tick_t)(((rt_uint64_t)rt_tick_get() * CONFIG_FREERTOS_HZ) /
                       RT_TICK_PER_SECOND);
}

void ft_timeout_set(ft_timeout_t *timeout)
{
    if (timeout == RT_NULL)
    {
        return;
    }

    timeout->xOverflowCount  = 0;
    timeout->xTimeOnEntering = ft_now();
}

ft_base_t ft_timeout_check(ft_timeout_t *timeout, ft_tick_t *remaining)
{
    ft_tick_t now;
    ft_tick_t elapsed;

    if (timeout == RT_NULL || remaining == RT_NULL)
    {
        return ftPASS;
    }

    /* portMAX_DELAY never expires; FreeRTOS short-circuits the same way. */
    if (*remaining == FT_MAX_DELAY)
    {
        return ftFAIL;
    }

    now     = ft_now();
    elapsed = (ft_tick_t)(now - timeout->xTimeOnEntering);

    if (elapsed < *remaining)
    {
        *remaining = (ft_tick_t)(*remaining - elapsed);
        timeout->xTimeOnEntering = now;
        return ftFAIL;
    }

    *remaining = 0;

    return ftPASS;
}
