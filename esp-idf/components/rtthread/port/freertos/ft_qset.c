/*
 * Queue sets on RT-Thread.
 *
 * A FreeRTOS queue set is a wait-any over a group of queues and semaphores:
 * xQueueSelectFromSet() blocks until at least one member has something and
 * returns that member's handle. The kernel implements it with an event counter
 * inside the set plus a hook in the send path of every member.
 *
 * The counter maps to a counting semaphore directly - one release per send, one
 * take per select - so the only real work here is the membership list and the
 * "find the member that is ready" scan.
 *
 * Why bother: esp_ble_mesh and esp_ble_iso are the only users, and neither is in
 * this image, so this is not on the critical path. It is here because
 * xQueueCreateSet/SelectFromSet are declared unconditionally in queue.h and
 * returning a stub would turn "BLE does not work yet" into "BLE hangs at
 * startup with no message". A correct implementation costs less than the
 * debugging a stub would cost later.
 *
 * A member must be empty when it is added, which FreeRTOS also requires. That
 * rule is what makes the notification bookkeeping exact: the set's count and the
 * members' contents can only disagree by the items received while not in a set.
 */

#include <rthw.h>
#include <rtthread.h>

#include "sdkconfig.h"
#include "ft_core.h"

#define FT_QSET_MAX      12      /* members per set, plenty for BLE's 2..4 */
#define FT_QSET_SETS     4       /* simultaneous sets */

struct ft_qset
{
    rt_uint32_t         members;
    struct ft_ipc      *item[FT_QSET_MAX];
    struct rt_semaphore notify;
    rt_uint8_t          in_use;
};

static struct ft_qset s_qset[FT_QSET_SETS];
static rt_bool_t      s_qset_ready;

static struct ft_qset *ft_qset_from_handle(void *handle)
{
    return (struct ft_qset *)handle;
}

void *ft_qset_create(ft_ubase_t length)
{
    rt_base_t level;
    int i;

    level = rt_hw_interrupt_disable();

    if (!s_qset_ready)
    {
        for (i = 0; i < FT_QSET_SETS; i++)
        {
            s_qset[i].in_use  = 0;
            s_qset[i].members = 0;
        }
        s_qset_ready = RT_TRUE;
    }

    for (i = 0; i < FT_QSET_SETS; i++)
    {
        if (!s_qset[i].in_use)
        {
            struct ft_qset *set = &s_qset[i];

            rt_hw_interrupt_enable(level);

            /* rt_sem_init allocates nothing (it only initialises the object in
             * place), so this is safe to do outside the critical section and
             * safe to repeat when the slot is reused. */
            if (rt_sem_init(&set->notify, "ftqset", 0, RT_IPC_FLAG_PRIO) != RT_EOK)
            {
                return RT_NULL;
            }

            level = rt_hw_interrupt_disable();

            set->members = 0;
            set->in_use  = 1;
            rt_memset(set->item, 0, sizeof(set->item));

            rt_hw_interrupt_enable(level);
            return set;
        }
    }

    rt_hw_interrupt_enable(level);

    rt_kprintf("freertos-shim: out of queue sets (%d)\n", FT_QSET_SETS);
    return RT_NULL;
}

ft_base_t ft_qset_add(void *handle, struct ft_ipc *queue)
{
    struct ft_qset *set = ft_qset_from_handle(handle);
    rt_base_t level;

    if (set == RT_NULL || queue == RT_NULL || set->members >= FT_QSET_MAX)
    {
        return ftFAIL;
    }

    level = rt_hw_interrupt_disable();

    /* FreeRTOS refuses a member that is not empty, because the set's event count
     * does not know about the items already in it: they would never produce a
     * notification and the queue would look empty forever. */
    if (ft_ipc_get_membership(queue) != RT_NULL ||
        ft_ipc_messages_waiting(queue) != 0)
    {
        rt_hw_interrupt_enable(level);
        return ftFAIL;
    }

    set->item[set->members++] = queue;
    ft_ipc_set_membership(queue, set);

    rt_hw_interrupt_enable(level);
    return ftPASS;
}

ft_base_t ft_qset_remove(void *handle, struct ft_ipc *queue)
{
    struct ft_qset *set = ft_qset_from_handle(handle);
    rt_base_t level;
    rt_uint32_t i;

    if (set == RT_NULL || queue == RT_NULL)
    {
        return ftFAIL;
    }

    level = rt_hw_interrupt_disable();

    if (ft_ipc_get_membership(queue) != set)
    {
        rt_hw_interrupt_enable(level);
        return ftFAIL;          /* not a member of this set */
    }

    for (i = 0; i < set->members; i++)
    {
        if (set->item[i] == queue)
        {
            /* Order is not meaningful to the caller, so fill the hole with the
             * last member rather than shifting. */
            set->item[i] = set->item[set->members - 1];
            set->item[set->members - 1] = RT_NULL;
            set->members--;
            break;
        }
    }

    ft_ipc_set_membership(queue, RT_NULL);

    rt_hw_interrupt_enable(level);
    return ftPASS;
}

/* Called from ft_ipc_send() after an item lands. One release per stored item, so
 * the set's semaphore count equals the number of ready members - which is what
 * FreeRTOS's queue set counter means. */
void ft_qset_signal(struct ft_ipc *queue)
{
    struct ft_qset *set;

    if (queue == RT_NULL)
    {
        return;
    }

    set = (struct ft_qset *)ft_ipc_get_membership(queue);
    if (set != RT_NULL)
    {
        rt_sem_release(&set->notify);
    }
}

void *ft_qset_select(void *handle, ft_tick_t ticks, ft_base_t *timed_out)
{
    struct ft_qset *set = ft_qset_from_handle(handle);
    rt_uint32_t i;

    if (timed_out != RT_NULL)
    {
        *timed_out = 0;
    }

    if (set == RT_NULL)
    {
        return RT_NULL;
    }

    /* The guard the other blocking calls have. Without it a select with the
     * scheduler locked suspends the caller and never switches away. */
    if (!ft_block_allowed(ticks, "xQueueSelectFromSet"))
    {
        if (timed_out != RT_NULL)
        {
            *timed_out = 1;
        }
        return RT_NULL;
    }

    /* Loop rather than take once and give up: between the release and this take
     * the item that caused it can be consumed by another reader, which would
     * leave one count in the semaphore and nothing to return. Taking again is
     * the correct response - the count is still an accurate statement that some
     * member *had* data, just not which one any more. */
    while (rt_sem_take(&set->notify, ft_ticks_to_rt(ticks)) == RT_EOK)
    {
        rt_base_t level = rt_hw_interrupt_disable();

        for (i = 0; i < set->members; i++)
        {
            if (set->item[i] != RT_NULL &&
                ft_ipc_messages_waiting(set->item[i]) != 0)
            {
                void *found = set->item[i];

                rt_hw_interrupt_enable(level);
                return found;
            }
        }

        rt_hw_interrupt_enable(level);
        /* Nothing ready: the notification was stale. Go round again, but do not
         * let a zero timeout spin the CPU. */
        if (ticks == 0)
        {
            break;
        }
    }

    if (timed_out != RT_NULL)
    {
        *timed_out = 1;
    }

    return RT_NULL;
}

void *ft_qset_select_isr(void *handle, ft_base_t *timed_out)
{
    return ft_qset_select(handle, 0, timed_out);
}

/* Forget a queue's membership when it is deleted. Without this the set keeps a
 * dangling pointer and the next select dereferences freed memory. FreeRTOS
 * asserts instead; either way the queue must not be deleted while in a set, but
 * quietly surviving is better than the alternative. */
void ft_qset_forget(struct ft_ipc *queue)
{
    struct ft_qset *set;

    if (queue == RT_NULL)
    {
        return;
    }

    set = (struct ft_qset *)ft_ipc_get_membership(queue);
    if (set != RT_NULL)
    {
        (void)ft_qset_remove(set, queue);
    }
}
