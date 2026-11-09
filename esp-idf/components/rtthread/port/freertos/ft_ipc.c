/*
 * Queues, semaphores and mutexes on RT-Thread.
 *
 * FreeRTOS builds all three from one object: a queue has length + item size,
 * a binary semaphore is a length-1 queue with no payload, a counting semaphore
 * is a length-N queue with no payload, and a mutex is a separate type with
 * priority inheritance. The same shape is mirrored here so that
 * xQueueSemaphoreTake() can accept any of them and dispatch on `kind`.
 *
 * Blocking uses two RT-Thread semaphores per object: `available` counts stored
 * items, `free_slots` counts room. A sender must first take a free slot, so N
 * concurrent senders can never collide on the ring index; the index update
 * itself is done under an interrupt lock because an ISR can push at any time.
 */

#include <rthw.h>
#include <rtthread.h>

#include "sdkconfig.h"
#include "ft_core.h"

/* Bumped by libcpu/xtensa/cpuport.c the first time a core publishes a thread. */

enum ft_ipc_kind
{
    FT_IPC_KIND_QUEUE = 1,
    FT_IPC_KIND_SEM,
    FT_IPC_KIND_MUTEX,      /* non-recursive, like xSemaphoreCreateMutex */
    FT_IPC_KIND_RECMUTEX,   /* recursive, like xSemaphoreCreateRecursiveMutex */
};

struct ft_ipc
{
    rt_uint8_t  kind;
    rt_uint8_t  owns_memory;

    rt_uint8_t *buf;
    rt_uint32_t item_size;
    rt_uint32_t length;
    rt_uint32_t head;           /* next slot to write */
    rt_uint32_t tail;           /* next slot to read */
    rt_uint32_t count;          /* stored items, authoritative for the queries */

    struct rt_semaphore available;
    struct rt_semaphore free_slots;

    struct rt_mutex mutex;      /* both mutex kinds */

    /* RT-Thread mutexes are recursive by nature: rt_mutex_take by the owner
     * just bumps hold. FreeRTOS's plain mutex is not, and code relies on that,
     * so the plain kind guards against a second take explicitly - reading
     * `mutex.owner`, which is the only owner record there is. A shadow copy
     * used to live in this struct and had to be deleted: rt_thread_delete() on
     * the holder releases the mutex through _thread_detach_from_mutex()
     * (src/thread.c) and clears the kernel's owner, with no way to reach a
     * duplicate here. The stale pointer then survived address reuse and made
     * the mutex permanently "already held" by whoever landed on that address
     * next. */

    /* Set when the object was heap allocated by ft_ipc_create_static_ram()
     * because the caller's control block (a FreeRTOS StaticQueue_t) is too
     * small to hold struct ft_ipc. Kept so xQueueGetStaticBuffers() can hand
     * the caller's own block back for it to free. */
    void       *static_cb;

    /* Set when the caller's static control block holds a stub pointing here, so
     * deleting the object must also clear that stub. */
    rt_uint8_t  has_stub;

    /* The queue set this queue belongs to, or NULL. FreeRTOS requires that a
     * queue be empty when it is added to a set, and the set is notified on every
     * successful send - that notification is what xQueueSelectFromSet() waits
     * on. Kept as an opaque pointer because ft_qset lives in ft_compat.c and
     * this file must not depend on it. */
    void       *set;
};

/* FreeRTOS ticks to RT-Thread ticks. Both are 1 kHz in this configuration, but
 * the conversion is written out so changing either one cannot silently scale
 * every timeout in the system. */
rt_int32_t ft_ticks_to_rt(ft_tick_t ticks)
{
    if (ticks == FT_MAX_DELAY)
    {
        return RT_WAITING_FOREVER;
    }

    /* Round up, so a wait that asks for less than one RT-Thread tick still waits
     * one instead of turning into a non-blocking poll. Identity today (both
     * rates are 1 kHz), but the whole point of writing the conversion out is
     * that changing either rate cannot silently change behaviour. */
    return (rt_int32_t)((((rt_uint64_t)ticks * RT_TICK_PER_SECOND)
                         + CONFIG_FREERTOS_HZ - 1u) / CONFIG_FREERTOS_HZ);
}

rt_size_t ft_ipc_size(void)
{
    return sizeof(struct ft_ipc);
}

/* The stub, not the object, has to fit in the caller's control block; the other
 * half of that check lives in freertos_api.c where StaticQueue_t is in scope. */
_Static_assert(sizeof(struct ft_ipc_stub) <= 16,
               "the stub is meant to be small enough for any StaticQueue_t");

/* The size is exported rather than asserted here because this file must not
 * include the FreeRTOS headers - that separation is what keeps it reusable. The
 * one place the comparison matters is freertos_api.c, which has StaticQueue_t
 * in scope and does the check with a static assert. */

/*
 * Refuse to block when the RT-Thread scheduler is locked, or from an ISR.
 *
 * This is not defensive coding; it turns a hang into an error. rt_schedule()
 * does nothing at all while rt_scheduler_lock_nest is non-zero
 * (src/scheduler_up.c:296), so _rt_sem_take() suspends the caller, fails to
 * switch away, and returns -RT_EINTR while the thread is still on the
 * semaphore's suspend list (src/ipc.c sets thread->error = RT_EINTR before the
 * suspend and only the resumer clears it). The caller then keeps running with
 * its status already set to suspend and its ready-queue entry gone, so the next
 * reschedule parks it forever with no trace.
 *
 * FreeRTOS enforces the same rule with configASSERT in vTaskPlaceOnEventList.
 * The difference is that an assert stops the board at the call site, which is
 * what we want here too - but recoverable, because some IDF paths probe for it.
 */
rt_bool_t ft_block_allowed(ft_tick_t ticks, const char *what)
{
    static rt_uint32_t warned;

    if (ticks == 0)
    {
        return RT_TRUE;         /* non-blocking is always legal */
    }

    if (rt_interrupt_get_nest() != 0)
    {
        if (!warned)
        {
            warned = 1;
            rt_kprintf("freertos-shim: %s cannot block in an ISR\n", what);
        }
        return RT_FALSE;
    }

    /*
     * No scheduler yet - there is no thread to suspend, and _rt_sem_take()
     * stores through rt_thread_self() without checking (src/ipc.c:601 then :604
     * `thread->error = RT_EINTR`). That is a store fault at address 44,
     * offsetof(struct rt_thread, error), from a NULL base.
     *
     * This is a separate window from the mutex check in ft_ipc_take(): that one
     * short-circuits mutexes, but queues, semaphores and notifications all reach
     * rt_sem_take() through here, and IDF's SECONDARY stage runs before
     * rt_system_scheduler_init() and does take semaphores (esp_coex's wrappers
     * call xSemaphoreTake with OSI_FUNCS_TIME_BLOCKING). Checking the thread here
     * covers every blocking entry point at once - which is what this function
     * exists for, and what its own header documents it as doing.
     *
     * Returning RT_FALSE makes the callers report FT_ERR_STATE, i.e. a refusal,
     * never a half-suspended thread.
     */
    if (ft_port_running_count() == 0)
    {
        if (!warned)
        {
            warned = 1;
            rt_kprintf("freertos-shim: %s cannot block before the scheduler "
                       "starts\n", what);
        }
        return RT_FALSE;
    }

    /*
     * A non-zero scheduler nesting is not by itself a reason to refuse.
     *
     * rt_enter_critical() / rt_spin_lock() raise the CALLING thread's
     * critical_lock_nest, and _rt_sem_take() itself takes a spinlock before
     * suspending - so a thread that legitimately blocks is normally one or two
     * levels deep. Refusing at the first non-zero level made every blocking
     * call fail, and esp_ipc's worker (priority 6, above every kernel thread)
     * then spun in `while (true) { ulTaskNotifyTake(portMAX_DELAY); }`.
     * Measured consequence: the boot stopped right after the banner because
     * rt_components_init() never reached finsh_system_init() - the shell thread
     * was never created at all.
     *
     * What must still be refused is a DEEPER nest together with an interrupt
     * context: those are the two states in which rt_schedule() cannot run and
     * a suspended thread would be left half-linked. Both are checked above and
     * below this comment; the plain "nest == 0" rule was the broken one.
     */

    if (rt_critical_level() > 1)
    {
        if (!warned)
        {
            warned = 1;
            rt_kprintf("freertos-shim: %s cannot block with the scheduler locked "
                       "(critical level %d)\n", what, (int)rt_critical_level());
        }
        return RT_FALSE;
    }

    return RT_TRUE;
}

static struct ft_ipc *ft_ipc_init(struct ft_ipc *q, rt_uint8_t kind,
                                  ft_ubase_t length, ft_ubase_t item_size,
                                  rt_uint8_t *storage, rt_bool_t owns)
{
    rt_memset(q, 0, sizeof(*q));

    q->kind        = kind;
    q->owns_memory = owns ? 1 : 0;
    q->buf         = storage;
    q->item_size   = item_size;
    q->length      = length ? (rt_uint32_t)length : 1u;

    if (kind == FT_IPC_KIND_MUTEX || kind == FT_IPC_KIND_RECMUTEX)
    {
        if (rt_mutex_init(&q->mutex, "ftmtx", RT_IPC_FLAG_PRIO) != RT_EOK)
        {
            return RT_NULL;
        }
        return q;
    }

    if (rt_sem_init(&q->available, "ftav", 0, RT_IPC_FLAG_PRIO) != RT_EOK)
    {
        return RT_NULL;
    }

    if (rt_sem_init(&q->free_slots, "ftfs", q->length, RT_IPC_FLAG_PRIO) != RT_EOK)
    {
        rt_sem_detach(&q->available);
        return RT_NULL;
    }

    return q;
}

struct ft_ipc *ft_ipc_create(ft_ubase_t length, ft_ubase_t item_size)
{
    struct ft_ipc *q;
    rt_uint8_t    *buf = RT_NULL;

    if (length == 0)
    {
        return RT_NULL;
    }

    /* ft_ubase_t is unsigned long, which is 32 bits here, so
     * length * item_size can wrap. A 65536-entry queue of 65536-byte items
     * would ask rt_malloc for 0 bytes, succeed, and then let the first send
     * write past the end of a zero-length buffer - a heap corruption whose
     * report arrives somewhere unrelated. FreeRTOS rejects the same combination
     * by returning NULL from xQueueGenericCreate.
     *
     * RT_SIZE_MAX rather than SIZE_MAX: rtdef.h picks the right width for the
     * target, and it is already on the include path. */
    if (item_size != 0 && length > (ft_ubase_t)(RT_SIZE_MAX / item_size))
    {
        return RT_NULL;
    }

    q = rt_malloc(sizeof(*q));
    if (q == RT_NULL)
    {
        return RT_NULL;
    }

    if (item_size != 0)
    {
        buf = rt_malloc((rt_size_t)length * item_size);
        if (buf == RT_NULL)
        {
            rt_free(q);
            return RT_NULL;
        }
    }

    if (ft_ipc_init(q, item_size ? FT_IPC_KIND_QUEUE : FT_IPC_KIND_SEM,
                    length, item_size, buf, RT_TRUE) == RT_NULL)
    {
        if (buf)
        {
            rt_free(buf);
        }
        rt_free(q);
        return RT_NULL;
    }

    return q;
}

struct ft_ipc *ft_ipc_create_static(ft_ubase_t length, ft_ubase_t item_size,
                                    void *storage, void *cb, ft_ubase_t cb_size)
{
    /* Refusing here is the whole point of the size check: writing a 160-byte
     * object into a caller's StaticQueue_t would corrupt whatever follows it,
     * and the symptom would appear somewhere unrelated minutes later. */
    if (cb == RT_NULL || cb_size < sizeof(struct ft_ipc) || length == 0)
    {
        return RT_NULL;
    }

    return ft_ipc_init((struct ft_ipc *)cb,
                       item_size ? FT_IPC_KIND_QUEUE : FT_IPC_KIND_SEM,
                       length, item_size, (rt_uint8_t *)storage, RT_FALSE);
}

/* ------------------------------------------------------------------ stub -- */

/*
 * Static handles as stubs.
 *
 * FreeRTOS's static creation functions return the caller's control block as the
 * handle, and callers depend on it - esp_libc asserts
 *
 *     assert(handle == (SemaphoreHandle_t) &s_common_mutex);   locks.c:422
 *
 * The object cannot go inside that block: StaticQueue_t is a placeholder sized
 * for FreeRTOS's own TCB (~60 bytes) while a queue here needs two RT-Thread
 * semaphores plus a mutex (~180). Placing it anyway would overrun the caller's
 * memory by more than 100 bytes.
 *
 * So the block gets a two-word stub and the real object lives on the heap. Every
 * API entry passes the handle through ft_ipc_resolve(), which follows the stub
 * only when the magic matches - a real struct ft_ipc starts with three one-byte
 * flags, so its first word cannot be FT_IPC_MAGIC by accident.
 */

struct ft_ipc *FT_IRAM ft_ipc_resolve(void *handle)
{
    struct ft_ipc_stub *stub;

    if (handle == RT_NULL)
    {
        return RT_NULL;
    }

    stub = (struct ft_ipc_stub *)handle;

    if (stub->magic == FT_IPC_MAGIC)
    {
        return stub->real;
    }

    return (struct ft_ipc *)handle;
}

static struct ft_ipc *ft_ipc_stub_finish(struct ft_ipc *real, void *cb)
{
    struct ft_ipc_stub *stub = (struct ft_ipc_stub *)cb;

    stub->magic = FT_IPC_MAGIC;
    stub->real  = real;

    real->static_cb = cb;
    real->has_stub  = 1;

    /* The handle is the caller's block, not the object - that is the point of the
     * stub. Callers keep using the pointer they passed in. */
    return (struct ft_ipc *)cb;
}

struct ft_ipc *ft_ipc_create_stub(ft_ubase_t length, ft_ubase_t item_size,
                                  void *storage, void *cb, rt_uint32_t cb_size)
{
    struct ft_ipc *q;

    if (cb == RT_NULL || length == 0 ||
        cb_size < sizeof(struct ft_ipc_stub))
    {
        return RT_NULL;
    }

    q = rt_malloc(sizeof(*q));
    if (q == RT_NULL)
    {
        return RT_NULL;
    }

    if (ft_ipc_init(q, item_size ? FT_IPC_KIND_QUEUE : FT_IPC_KIND_SEM,
                    length, item_size, (rt_uint8_t *)storage, RT_FALSE) == RT_NULL)
    {
        rt_free(q);
        return RT_NULL;
    }

    return ft_ipc_stub_finish(q, cb);
}

struct ft_ipc *ft_ipc_create_mutex_stub(void *cb, rt_uint32_t cb_size,
                                        ft_base_t recursive)
{
    struct ft_ipc *q;

    if (cb == RT_NULL || cb_size < sizeof(struct ft_ipc_stub))
    {
        return RT_NULL;
    }

    q = rt_malloc(sizeof(*q));
    if (q == RT_NULL)
    {
        return RT_NULL;
    }

    if (ft_ipc_init(q, recursive ? FT_IPC_KIND_RECMUTEX : FT_IPC_KIND_MUTEX,
                    1, 0, RT_NULL, RT_FALSE) == RT_NULL)
    {
        rt_free(q);
        return RT_NULL;
    }

    return ft_ipc_stub_finish(q, cb);
}

struct ft_ipc *ft_ipc_create_static_ram(ft_ubase_t length, ft_ubase_t item_size,
                                        void *storage, void *cb)
{
    struct ft_ipc *q;

    if (cb == RT_NULL || length == 0)
    {
        return RT_NULL;
    }

    q = rt_malloc(sizeof(*q));
    if (q == RT_NULL)
    {
        return RT_NULL;
    }

    if (ft_ipc_init(q, item_size ? FT_IPC_KIND_QUEUE : FT_IPC_KIND_SEM,
                    length, item_size, (rt_uint8_t *)storage, RT_FALSE) == RT_NULL)
    {
        rt_free(q);
        return RT_NULL;
    }

    q->static_cb = cb;

    return q;
}

void *ft_ipc_static_cb(struct ft_ipc *ipc)
{
    ipc = ft_ipc_resolve(ipc);
    return (ipc != RT_NULL) ? ipc->static_cb : RT_NULL;
}

struct ft_ipc *ft_ipc_create_mutex_static(void *cb, ft_base_t recursive)
{
    struct ft_ipc *q;

    q = rt_malloc(sizeof(*q));
    if (q == RT_NULL)
    {
        return RT_NULL;
    }

    if (ft_ipc_init(q, recursive ? FT_IPC_KIND_RECMUTEX : FT_IPC_KIND_MUTEX,
                    1, 0, RT_NULL, RT_FALSE) == RT_NULL)
    {
        rt_free(q);
        return RT_NULL;
    }

    q->static_cb = cb;

    return q;
}

void *ft_ipc_storage(struct ft_ipc *ipc)
{
    ipc = ft_ipc_resolve(ipc);
    return (ipc != RT_NULL) ? ipc->buf : RT_NULL;
}

struct ft_ipc *ft_ipc_create_sem(ft_ubase_t max_count, ft_ubase_t initial)
{
    struct ft_ipc *q;
    ft_ubase_t i;

    q = ft_ipc_create(max_count ? max_count : 1, 0);
    if (q == RT_NULL)
    {
        return RT_NULL;
    }

    for (i = 0; i < initial; i++)
    {
        if (ft_ipc_give(q) != FT_OK)
        {
            break;
        }
    }

    return q;
}

struct ft_ipc *ft_ipc_create_mutex_ex(ft_base_t recursive)
{
    struct ft_ipc *q = rt_malloc(sizeof(*q));

    if (q == RT_NULL)
    {
        return RT_NULL;
    }

    if (ft_ipc_init(q, recursive ? FT_IPC_KIND_RECMUTEX : FT_IPC_KIND_MUTEX,
                    1, 0, RT_NULL, RT_TRUE) == RT_NULL)
    {
        rt_free(q);
        return RT_NULL;
    }

    return q;
}

struct ft_ipc *ft_ipc_create_mutex(void)
{
    return ft_ipc_create_mutex_ex(0);
}

void *ft_ipc_mutex_holder(struct ft_ipc *ipc)
{
    ipc = ft_ipc_resolve(ipc);
    if (ipc == RT_NULL)
    {
        return RT_NULL;
    }

    /* Both mutex kinds answer from the kernel's own owner field. xSemaphoreGet-
     * MutexHolder is what esp_libc's _lock_close() asserts on, so a stale answer
     * here turns "this lock is free" into "somebody still holds it". */
    if (ipc->kind == FT_IPC_KIND_MUTEX || ipc->kind == FT_IPC_KIND_RECMUTEX)
    {
        return ipc->mutex.owner;
    }

    return RT_NULL;
}

void ft_ipc_delete(struct ft_ipc *ipc)
{
    ipc = ft_ipc_resolve(ipc);
    if (ipc == RT_NULL)
    {
        return;
    }

    /* A deleted queue must not stay in a queue set: the set would keep the
     * pointer and the next select would dereference freed memory. */
    ft_qset_forget(ipc);

    if (ipc->kind == FT_IPC_KIND_MUTEX || ipc->kind == FT_IPC_KIND_RECMUTEX)
    {
        rt_mutex_detach(&ipc->mutex);
    }
    else
    {
        rt_sem_detach(&ipc->available);
        rt_sem_detach(&ipc->free_slots);
    }

    if (ipc->owns_memory)
    {
        if (ipc->buf)
        {
            rt_free(ipc->buf);
        }
        rt_free(ipc);
    }
    else if (ipc->has_stub)
    {
        /* The handle the caller holds is its own control block, so the stub has
         * to be cleared before the object disappears - otherwise the next call
         * through that handle dereferences freed memory. The block itself
         * belongs to the caller and is left allocated. */
        if (ipc->static_cb != RT_NULL)
        {
            rt_memset(ipc->static_cb, 0, sizeof(struct ft_ipc_stub));
        }
        ipc->has_stub = 0;
        rt_free(ipc);
    }
    else if (ipc->static_cb != RT_NULL)
    {
        /* Heap object, caller's storage and control block. vQueueDeleteWithCaps()
         * frees those two itself, so only the object goes here. */
        ipc->static_cb = RT_NULL;
        rt_free(ipc);
    }
    else
    {
        rt_memset(ipc, 0, sizeof(*ipc));
    }
}

/* Ring update. Short and interrupt-locked: taking a free slot already
 * guarantees the slot exists, this only makes the index arithmetic atomic with
 * respect to an ISR pushing at the same time. */
static void ft_ipc_push(struct ft_ipc *q, const void *item)
{
    rt_base_t level = rt_hw_interrupt_disable();

    if (q->item_size != 0 && item != RT_NULL)
    {
        rt_memcpy(q->buf + q->head * q->item_size, item, q->item_size);
    }

    q->head = (q->head + 1u) % q->length;
    q->count++;

    rt_hw_interrupt_enable(level);
}

static void ft_ipc_pop(struct ft_ipc *q, void *item)
{
    rt_base_t level = rt_hw_interrupt_disable();

    if (q->item_size != 0 && item != RT_NULL)
    {
        rt_memcpy(item, q->buf + q->tail * q->item_size, q->item_size);
    }

    q->tail = (q->tail + 1u) % q->length;
    q->count--;

    rt_hw_interrupt_enable(level);
}

int FT_IRAM ft_ipc_send(struct ft_ipc *ipc, const void *item, ft_tick_t ticks)
{
    ipc = ft_ipc_resolve(ipc);
    if (ipc == RT_NULL)
    {
        return FT_ERR_PARAM;
    }

    if (ipc->kind == FT_IPC_KIND_MUTEX || ipc->kind == FT_IPC_KIND_RECMUTEX)
    {
        return ft_ipc_give(ipc);
    }

    /* Try the non-blocking path first. A free slot means this call cannot
     * block, so it stays legal inside a critical section - FreeRTOS's assert
     * guards vTaskPlaceOnEventList, i.e. the blocking path only. */
    if (rt_sem_trytake(&ipc->free_slots) != RT_EOK)
    {
        if (ticks == 0)
        {
            return FT_ERR_FULL;
        }

        if (!ft_block_allowed(ticks, "xQueueSend"))
        {
            return FT_ERR_STATE;
        }

        if (rt_sem_take(&ipc->free_slots, ft_ticks_to_rt(ticks)) != RT_EOK)
        {
            return FT_ERR_FULL;
        }
    }

    ft_ipc_push(ipc, item);
    rt_sem_release(&ipc->available);

    /* Tell the queue set, if this queue is a member of one. Done for the blocking
     * and the ISR path separately below, because the ISR one must not touch the
     * scheduler. */
    ft_qset_signal(ipc);

    return FT_OK;
}

int FT_IRAM ft_ipc_send_isr(struct ft_ipc *ipc, const void *item, ft_base_t *woken)
{
    ipc = ft_ipc_resolve(ipc);
    if (woken != RT_NULL)
    {
        *woken = ftFAIL;
    }

    if (ipc == RT_NULL)
    {
        return FT_ERR_PARAM;
    }

    if (ipc->kind == FT_IPC_KIND_MUTEX || ipc->kind == FT_IPC_KIND_RECMUTEX)
    {
        return ft_ipc_give_isr(ipc, woken);
    }

    if (rt_sem_trytake(&ipc->free_slots) != RT_EOK)
    {
        return FT_ERR_FULL;
    }

    ft_ipc_push(ipc, item);
    rt_sem_release(&ipc->available);
    ft_qset_signal(ipc);

    /* Conservative: RT-Thread's release does not report whether a waiter was
     * unblocked, so always ask for a reschedule. An unnecessary yield only
     * costs a scheduler pass, while a missing one delays a higher priority
     * task by a full tick. */
    if (woken != RT_NULL)
    {
        *woken = ftPASS;
    }

    return FT_OK;
}

static int ft_ipc_take_item(struct ft_ipc *ipc, void *item, ft_tick_t ticks)
{
    if (rt_sem_trytake(&ipc->available) != RT_EOK)
    {
        if (ticks == 0)
        {
            return FT_ERR_TIMEOUT;
        }

        if (!ft_block_allowed(ticks, "xQueueReceive"))
        {
            return FT_ERR_STATE;
        }

        if (rt_sem_take(&ipc->available, ft_ticks_to_rt(ticks)) != RT_EOK)
        {
            return FT_ERR_TIMEOUT;
        }
    }

    ft_ipc_pop(ipc, item);
    rt_sem_release(&ipc->free_slots);

    return FT_OK;
}

int ft_ipc_receive(struct ft_ipc *ipc, void *item, ft_tick_t ticks)
{
    ipc = ft_ipc_resolve(ipc);
    if (ipc == RT_NULL ||
        ipc->kind == FT_IPC_KIND_MUTEX || ipc->kind == FT_IPC_KIND_RECMUTEX)
    {
        return FT_ERR_PARAM;
    }

    return ft_ipc_take_item(ipc, item, ticks);
}

int ft_ipc_receive_isr(struct ft_ipc *ipc, void *item, ft_base_t *woken)
{
    ipc = ft_ipc_resolve(ipc);
    if (woken != RT_NULL)
    {
        *woken = ftFAIL;
    }

    if (ipc == RT_NULL ||
        ipc->kind == FT_IPC_KIND_MUTEX || ipc->kind == FT_IPC_KIND_RECMUTEX)
    {
        return FT_ERR_PARAM;
    }

    if (rt_sem_trytake(&ipc->available) != RT_EOK)
    {
        return FT_ERR_EMPTY;
    }

    ft_ipc_pop(ipc, item);
    rt_sem_release(&ipc->free_slots);

    if (woken != RT_NULL)
    {
        *woken = ftPASS;
    }

    return FT_OK;
}

int ft_ipc_take(struct ft_ipc *ipc, ft_tick_t ticks)
{
    ipc = ft_ipc_resolve(ipc);
    if (ipc == RT_NULL)
    {
        return FT_ERR_PARAM;
    }

    /* Before the scheduler starts there is no thread to own a mutex, and
     * rt_mutex_take() dereferences rt_thread_self() without checking
     * (src/ipc.c:1353, `thread->error = RT_EOK`). FreeRTOS's own contract for
     * this window is that locking is a no-op - esp_libc relies on it
     * (locks.c:128) - so the mutex kinds report success without touching the
     * RT-Thread object. Queue and semaphore operations still work, because
     * rt_sem_trytake() does not need a thread.
     *
     * This is not hypothetical: mbedtls's PSA init at the SECONDARY stage takes
     * its global mutex through pthread, and without this the CPU takes a store
     * fault at address 44 - offsetof(struct rt_thread, error) from a NULL base.
     */
    /*
     * "No thread yet" must be asked of the PORT, not of rt_thread_self().
     *
     * rt_thread_self() reads _cpus[rt_hw_cpu_id()].current_thread, and on the
     * dual-core bring-up that word is NULL for a while even after threads are
     * running: measured 126 consecutive NULLs from the real &_cpus[0] while
     * `s3smp` read `tshell` from the same address moments later.
     *
     * The cost of getting this wrong is that every mutex take returns FT_OK
     * WITHOUT taking anything, so nvs_flash_init() runs its flash sequence with
     * no mutual exclusion. rt_xt_running_count is bumped by the port the first
     * time it publishes a thread, so it answers the real question.
     */
    if ((ipc->kind == FT_IPC_KIND_MUTEX || ipc->kind == FT_IPC_KIND_RECMUTEX) &&
        ft_port_running_count() == 0)
    {
        return FT_OK;
    }

    if (ipc->kind == FT_IPC_KIND_RECMUTEX)
    {
        /* rt_mutex_take already lets the owner take it again, and for the
         * owner that path never blocks. */
        if (rt_mutex_trytake(&ipc->mutex) == RT_EOK)
        {
            return FT_OK;
        }

        if (ticks == 0)
        {
            return FT_ERR_TIMEOUT;
        }

        if (!ft_block_allowed(ticks, "xSemaphoreTake"))
        {
            return FT_ERR_STATE;
        }

        return (rt_mutex_take(&ipc->mutex, ft_ticks_to_rt(ticks)) == RT_EOK)
               ? FT_OK : FT_ERR_TIMEOUT;
    }

    if (ipc->kind == FT_IPC_KIND_MUTEX)
    {
        rt_thread_t self = rt_thread_self();

        /* Non-recursive: a second take by the holder must fail, where FreeRTOS
         * would deadlock.
         *
         * The check reads `ipc->mutex.owner`, i.e. the kernel's own field -
         * NOT a copy kept in this struct. A copy goes stale: rt_thread_delete()
         * on the holder releases the mutex through
         * _thread_detach_from_mutex() (src/thread.c), which clears
         * mutex->owner, but it has no way to clear a duplicate here. The
         * allocator then hands the dead thread's address to a new task, whose
         * `owner == self` comparison against the stale copy says "you already
         * hold this" - and that mutex is unusable for the rest of the run,
         * with esp_libc's _lock_close() also asserting on the phantom holder.
         *
         * `owner != RT_NULL` stays load-bearing: before the scheduler starts
         * rt_thread_self() is NULL and so is an unowned mutex, and NULL == NULL
         * once made every mutex look self-held (the psa_crypto_init -144 boot
         * failure). */
        if (self != RT_NULL && ipc->mutex.owner == self)
        {
            return FT_ERR_TIMEOUT;
        }

        if (rt_mutex_trytake(&ipc->mutex) == RT_EOK)
        {
            return FT_OK;
        }

        if (ticks == 0)
        {
            return FT_ERR_TIMEOUT;
        }

        if (!ft_block_allowed(ticks, "xSemaphoreTake"))
        {
            return FT_ERR_STATE;
        }

        return (rt_mutex_take(&ipc->mutex, ft_ticks_to_rt(ticks)) == RT_EOK)
               ? FT_OK : FT_ERR_TIMEOUT;
    }

    return ft_ipc_take_item(ipc, RT_NULL, ticks);
}

int FT_IRAM ft_ipc_give(struct ft_ipc *ipc)
{
    ipc = ft_ipc_resolve(ipc);
    if (ipc == RT_NULL)
    {
        return FT_ERR_PARAM;
    }

    /* The matching half of the pre-scheduler no-op in ft_ipc_take(), and it has
     * to use the same test - see the long note there. */
    if ((ipc->kind == FT_IPC_KIND_MUTEX || ipc->kind == FT_IPC_KIND_RECMUTEX) &&
        ft_port_running_count() == 0)
    {
        return FT_OK;
    }

    if (ipc->kind == FT_IPC_KIND_RECMUTEX)
    {
        return (rt_mutex_release(&ipc->mutex) == RT_EOK) ? FT_OK : FT_ERR_STATE;
    }

    if (ipc->kind == FT_IPC_KIND_MUTEX)
    {
        /* `mutex.owner`, not a shadow copy - same reason as in ft_ipc_take():
         * the kernel releases and clears it when the holder dies
         * (_thread_detach_from_mutex), a copy here would go stale and outlive
         * the thread it names.
         *
         * The comparison is only a fast path for "somebody else is holding", so
         * that a stray release reports FT_ERR_STATE instead of confusing
         * rt_mutex_release. Everything else - including the legitimate case of
         * releasing a mutex whose owner has died, which rt_mutex_release()
         * explicitly allows via RT_THREAD_CLOSE - is decided by the kernel. */
        if (ipc->mutex.owner != RT_NULL && ipc->mutex.owner != rt_thread_self())
        {
            return FT_ERR_STATE;    /* only the owner may release */
        }

        return (rt_mutex_release(&ipc->mutex) == RT_EOK) ? FT_OK : FT_ERR_STATE;
    }

    return ft_ipc_send(ipc, RT_NULL, FT_NO_WAIT);
}

int FT_IRAM ft_ipc_give_isr(struct ft_ipc *ipc, ft_base_t *woken)
{
    ipc = ft_ipc_resolve(ipc);
    if (ipc != RT_NULL &&
        (ipc->kind == FT_IPC_KIND_MUTEX || ipc->kind == FT_IPC_KIND_RECMUTEX))
    {
        /* A mutex cannot be released from an ISR in FreeRTOS either. */
        if (woken != RT_NULL)
        {
            *woken = ftFAIL;
        }
        return FT_ERR_PARAM;
    }

    return ft_ipc_send_isr(ipc, RT_NULL, woken);
}

ft_base_t ft_ipc_is_empty(struct ft_ipc *ipc)
{
    ipc = ft_ipc_resolve(ipc);
    if (ipc == RT_NULL)
    {
        return ftPASS;
    }

    return ipc->count ? ftFAIL : ftPASS;
}

ft_ubase_t ft_ipc_messages_waiting(struct ft_ipc *ipc)
{
    ipc = ft_ipc_resolve(ipc);
    if (ipc == RT_NULL ||
        ipc->kind == FT_IPC_KIND_MUTEX || ipc->kind == FT_IPC_KIND_RECMUTEX)
    {
        return 0;
    }

    return ipc->count;
}

ft_ubase_t ft_ipc_spaces_available(struct ft_ipc *ipc)
{
    ipc = ft_ipc_resolve(ipc);
    if (ipc == RT_NULL ||
        ipc->kind == FT_IPC_KIND_MUTEX || ipc->kind == FT_IPC_KIND_RECMUTEX)
    {
        return 0;
    }

    return (ipc->length > ipc->count) ? (ipc->length - ipc->count) : 0;
}

/* -------------------------------------------------------------- overwrite -- */

/*
 * xQueueOverwrite / xQueueOverwriteFromISR.
 *
 * FreeRTOS only defines these for queues of length one - they assert
 * uxQueueLength == 1 - and the semantics are "replace whatever is there, never
 * block, never fail". That is how a driver publishes the latest of something (a
 * GPIO event, an ADC sample) to a consumer that does not need every one.
 *
 * The earlier version of this port treated queueOVERWRITE as queueSEND_TO_BACK
 * and warned about it. That is wrong in a way that would have been hard to see:
 * the first overwrite on a full queue would have returned "full" where FreeRTOS
 * returns success, so the producer would have reported an error while the
 * consumer kept reading stale data. One of the I2C driver's event paths uses it
 * from an ISR.
 */
int ft_ipc_overwrite(struct ft_ipc *ipc, const void *item)
{
    ipc = ft_ipc_resolve(ipc);
    rt_base_t level;
    rt_bool_t was_empty;

    if (ipc == RT_NULL || item == RT_NULL ||
        ipc->kind == FT_IPC_KIND_MUTEX || ipc->kind == FT_IPC_KIND_RECMUTEX ||
        ipc->item_size == 0)
    {
        return FT_ERR_PARAM;
    }

    /* FreeRTOS asserts length == 1 for this call, and enforcing it keeps the
     * meaning unambiguous: on a longer queue "overwrite" would silently drop
     * every queued item but the newest, so a caller that expected a normal queue
     * would lose data instead of getting an error. */
    if (ipc->length != 1)
    {
        static rt_uint32_t warned;

        if (!warned)
        {
            warned = 1;
            rt_kprintf("freertos-shim: xQueueOverwrite needs a length-1 queue "
                       "(this one has %u)\n", (unsigned)ipc->length);
        }
        return FT_ERR_PARAM;
    }

    level = rt_hw_interrupt_disable();

    was_empty = (ipc->count == 0);

    /* One slot, so head and tail cannot differ by more than the wrap. Writing at
     * tail and leaving count at 1 makes the next receive take exactly this item.
     *
     * The free-slot count has to move together with `count`, because the ring's
     * invariant is
     *
     *     count + free_slots.value == length
     *
     * A send takes a free slot, a receive gives one back. Leaving free_slots
     * alone on the empty-to-full transition would advertise a free slot that no
     * longer exists, and the next ordinary send would succeed and push a second
     * item into a one-slot ring - silent corruption past the end of the buffer.
     * The first version of this function did exactly that; the self test is
     * written to catch it. */
    rt_memcpy(ipc->buf + ipc->tail * ipc->item_size, item, ipc->item_size);

    ipc->head  = (ipc->tail + 1u) % ipc->length;
    ipc->count = 1;

    /* Setting the counter directly rather than taking it: an overwrite must not
     * block, and there is never a legitimate waiter on a full length-1 queue. */
    ipc->free_slots.value = 0;

    rt_hw_interrupt_enable(level);

    /* Only the empty-to-full transition has to wake a receiver, and only that
     * transition may release the count - releasing a count that is already
     * there would let a later receive return an item that is not present. Done
     * with interrupts on because rt_sem_release can reschedule. */
    if (was_empty)
    {
        rt_sem_release(&ipc->available);
        ft_qset_signal(ipc);
    }

    return FT_OK;
}

int ft_ipc_overwrite_isr(struct ft_ipc *ipc, const void *item, ft_base_t *woken)
{
    ipc = ft_ipc_resolve(ipc);
    int rc = ft_ipc_overwrite(ipc, item);

    /* rt_sem_release() inside has already made a waiting receiver ready; the
     * caller only uses this flag to decide whether to yield on the way out. */
    if (woken != RT_NULL)
    {
        *woken = (rc == FT_OK) ? ftPASS : ftFAIL;
    }

    return rc;
}

/* ------------------------------------------------------------ peek, reset -- */

/*
 * xQueuePeek and xQueueGenericReset, both declared in queue.h unconditionally.
 * Neither has a caller in this image; they are here because the alternative is an
 * undefined reference the first time a driver reaches for one, in a file whose
 * author has no reason to know the shim exists.
 */

/* Copy the front item without removing it.
 *
 * The wait is done on the same `available` semaphore a receive uses, but taken
 * as a try-take plus a sleep loop rather than rt_sem_take: rt_sem_take would
 * decrement the count, and a peek must leave it alone. Sleeping a tick between
 * tries is slower to wake than a real waiter would be, but a peek is a polling
 * call by nature and nothing here is on a hot path.
 */
int ft_ipc_peek(struct ft_ipc *ipc, void *item, ft_tick_t ticks)
{
    ipc = ft_ipc_resolve(ipc);
    rt_uint32_t elapsed = 0;

    if (ipc == RT_NULL || item == RT_NULL ||
        ipc->kind == FT_IPC_KIND_MUTEX || ipc->kind == FT_IPC_KIND_RECMUTEX)
    {
        return FT_ERR_PARAM;
    }

    if (ipc->item_size == 0)
    {
        return FT_ERR_PARAM;    /* a semaphore has nothing to peek at */
    }

    for (;;)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        rt_uint32_t count = ipc->count;
        rt_uint32_t tail = ipc->tail;

        if (count != 0)
        {
            rt_memcpy(item, ipc->buf + tail * ipc->item_size, ipc->item_size);
            rt_hw_interrupt_enable(level);
            return FT_OK;
        }
        rt_hw_interrupt_enable(level);

        if (ticks == 0)
        {
            return FT_ERR_TIMEOUT;
        }

        if (ticks != FT_MAX_DELAY && elapsed >= ticks)
        {
            return FT_ERR_TIMEOUT;
        }

        if (!ft_block_allowed(ticks == FT_MAX_DELAY ? 1 : ticks, "xQueuePeek"))
        {
            return FT_ERR_STATE;
        }

        /* One tick is exactly one millisecond in this configuration, but the
         * conversion is written out so a change to either rate cannot turn this
         * into a busy loop. */
        rt_thread_mdelay((rt_int32_t)((1000u + CONFIG_FREERTOS_HZ - 1u)
                                      / CONFIG_FREERTOS_HZ));
        elapsed += 1;
    }
}

/* Empty the queue, keeping its capacity.
 *
 * Both counts are restored through rt_sem_control(RT_IPC_CMD_RESET) rather than
 * by assigning sem.value, because assignment cannot resume a waiter.
 *
 * The previous version wrote `ipc->free_slots.value = ipc->length` from inside a
 * critical section and claimed that "restores the free-slot count, which is what
 * unblocks senders". It does not: only rt_sem_release()/rt_sem_control() walk
 * parent.suspend_thread, so a thread already suspended in rt_sem_take(&free_slots)
 * - which is exactly what ft_ipc_send(q, item, portMAX_DELAY) does on a full
 * queue - stayed suspended forever after a reset that was supposed to empty the
 * queue. FreeRTOS's xQueueGenericReset explicitly unblocks writers (queue.h and
 * queue.c both say so).
 *
 * RT_IPC_CMD_RESET resumes every waiter with -RT_ERROR, which the shim already
 * maps to pdFALSE. Strictly better than a hang: the caller sees the failure.
 */
int ft_ipc_reset(struct ft_ipc *ipc)
{
    ipc = ft_ipc_resolve(ipc);

    if (ipc == RT_NULL ||
        ipc->kind == FT_IPC_KIND_MUTEX || ipc->kind == FT_IPC_KIND_RECMUTEX)
    {
        return FT_ERR_PARAM;
    }

    {
        rt_base_t level = rt_hw_interrupt_disable();

        ipc->head  = 0;
        ipc->tail  = 0;
        ipc->count = 0;

        rt_hw_interrupt_enable(level);
    }

    /* Outside the interrupt lock: rt_sem_control can reschedule. Receivers get
     * their count zeroed, senders get the full capacity back, and both sets of
     * waiters are released with an error. */
    (void)rt_sem_control(&ipc->available, RT_IPC_CMD_RESET, RT_NULL);
    (void)rt_sem_control(&ipc->free_slots, RT_IPC_CMD_RESET,
                         (void *)(rt_ubase_t)ipc->length);

    return FT_OK;
}

/* Queue-set membership. Two accessors rather than a public struct: struct ft_ipc
 * is private to this file on purpose, and one pointer is all ft_qset.c needs. */
void ft_ipc_set_membership(struct ft_ipc *ipc, void *set)
{
    ipc = ft_ipc_resolve(ipc);
    if (ipc != RT_NULL)
    {
        ipc->set = set;
    }
}

void *ft_ipc_get_membership(struct ft_ipc *ipc)
{
    ipc = ft_ipc_resolve(ipc);
    return (ipc != RT_NULL) ? ipc->set : RT_NULL;
}
