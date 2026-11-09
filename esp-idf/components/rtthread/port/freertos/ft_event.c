/*
 * Event groups on RT-Thread.
 *
 * The mapping is nearly direct: rt_event already supports AND/OR matching and
 * clear-on-match, which are exactly xEventGroupWaitBits' xWaitForAllBits and
 * xClearOnExit.
 *
 * Two details that do not map one to one:
 *
 * 1. xEventGroupClearBits() clears only the given bits, while rt_event_control
 *    with RT_IPC_CMD_RESET clears the whole set (src/ipc.c:2312 does
 *    `event->set = 0`). So the partial clear is done here, under the same
 *    spinlock rt_event uses internally rather than with a bare interrupt lock,
 *    to stay correct if this ever runs on both cores.
 *
 * 2. FreeRTOS returns the whole bit set as it stood when the wait was satisfied,
 *    *before* any bits are cleared. rt_event_recv with RT_EVENT_FLAG_CLEAR
 *    clears first, so reading the object afterwards returns the post-clear value
 *    - which for a clear-on-exit wait is 0, and that is wrong. The clear is
 *    therefore done here instead of by rt_event_recv, which also fixes a second
 *    difference: FreeRTOS clears only the bits it matched, while
 *    RT_EVENT_FLAG_CLEAR clears every bit in the wait mask (src/ipc.c:2188
 *    `event->set &= ~set`). An OR wait for {A,B} that matched only A must leave
 *    B alone, and that is observable: esp_event's loop waits on several bits.
 */

#include <rtthread.h>

#include "ft_core.h"

struct ft_event
{
    struct rt_event event;
    rt_uint8_t      in_use;
};

struct ft_event *ft_event_create(void)
{
    struct ft_event *e = rt_malloc(sizeof(*e));

    if (e == RT_NULL)
    {
        return RT_NULL;
    }

    rt_memset(e, 0, sizeof(*e));

    if (rt_event_init(&e->event, "ftev", RT_IPC_FLAG_PRIO) != RT_EOK)
    {
        rt_free(e);
        return RT_NULL;
    }

    e->in_use = 1;
    return e;
}

void ft_event_delete(struct ft_event *event)
{
    if (event == RT_NULL)
    {
        return;
    }

    rt_event_detach(&event->event);
    rt_free(event);
}

rt_uint32_t ft_event_set_bits(struct ft_event *event, rt_uint32_t bits)
{
    if (event == RT_NULL)
    {
        return 0;
    }

    if (bits != 0)
    {
        rt_event_send(&event->event, bits);
    }

    return event->event.set;
}

rt_uint32_t ft_event_clear_bits(struct ft_event *event, rt_uint32_t bits)
{
    rt_base_t level;
    rt_uint32_t before;

    if (event == RT_NULL)
    {
        return 0;
    }

    level = rt_spin_lock_irqsave(&event->event.spinlock);

    /* FreeRTOS returns the value *before* the clear (event_groups.c:503-505).
     * This used to return the post-clear value; nothing in the tree consumes the
     * result today, but xEventGroupGetBits() is xEventGroupClearBits(g, 0) and a
     * caller that starts using the return value would be silently wrong. */
    before = event->event.set;
    event->event.set &= ~bits;

    rt_spin_unlock_irqrestore(&event->event.spinlock, level);

    return before;
}

rt_uint32_t ft_event_get_bits(struct ft_event *event)
{
    if (event == RT_NULL)
    {
        return 0;
    }

    return event->event.set;
}

rt_uint32_t ft_event_wait_bits(struct ft_event *event, rt_uint32_t bits,
                               ft_base_t clear_on_exit, ft_base_t wait_for_all,
                               ft_tick_t ticks)
{
    rt_uint32_t option = wait_for_all ? RT_EVENT_FLAG_AND : RT_EVENT_FLAG_OR;
    rt_uint32_t received = 0;
    rt_uint32_t before;

    if (event == RT_NULL)
    {
        return 0;
    }

    /* Waiting for no bits at all is a poll of the current value. rt_event_recv
     * with a zero mask would wait for nothing and can return immediately with
     * RT_EOK, which is not what a caller expects. */
    if (bits == 0)
    {
        return event->event.set;
    }

    /* RT_EVENT_FLAG_CLEAR is deliberately NOT passed to rt_event_recv: it clears
     * the entire wait mask, while FreeRTOS clears only the bits that matched.
     * The clear is done below, under the event's own spinlock, once the caller
     * has been unblocked. */

    /* The same guard every other blocking call in this port has, which this one
     * was missing. rt_event_recv() suspends the caller when the bits are not
     * there yet; with the scheduler locked rt_schedule() is a no-op, so the
     * thread is left off the ready queue with its status still set to suspend
     * and nothing to wake it. FreeRTOS asserts on the same condition. */
    if (!ft_block_allowed(ticks, "xEventGroupWaitBits"))
    {
        return 0;
    }

    if (rt_event_recv(&event->event, bits, option, ft_ticks_to_rt(ticks),
                      &received) != RT_EOK)
    {
        return 0;
    }

    /* Snapshot before clearing: this is what FreeRTOS's return value means. */
    before = ft_event_get_bits(event);

    if (clear_on_exit)
    {
        /* Only the bits the wait matched. For AND that is the whole mask; for
         * OR it is `mask & set` as it stood when the wait was satisfied, which
         * `received` already reports. */
        rt_uint32_t matched = wait_for_all ? bits : received;

        if (matched != 0)
        {
            (void)ft_event_clear_bits(event, matched);
        }
    }

    return before;
}
