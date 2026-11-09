/*
 * SPDX-FileCopyrightText: 2026 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * FreeRTOS event-list helpers for the RT-Thread scons build.
 *
 * ESP-IDF v6.1's components/esp_ringbuf blocks senders/receivers on the task
 * event lists of the FreeRTOS kernel it ships in
 * components/freertos/FreeRTOS-Kernel.  The RT-Thread ESP BSPs run on the
 * FreeRTOS-Wrapper kernel (FreeRTOS V10.4.6), which has no event-list
 * machinery at all: no vTaskPlaceOnEventList(), no xTaskRemoveFromEventList()
 * and not even vTaskInternalSetTimeOutState() in its public headers.
 *
 * This file provides the two functions esp_ringbuf needs on top of the
 * wrapper's list implementation (list.c is part of the wrapper) and the RT-Thread
 * scheduler.  A waiter owns one statically allocated list item and polls a
 * volatile flag until the waker removes its item from the list or the timeout
 * expires; the critical sections around the list operations are the ones the
 * caller already holds (esp_ringbuf protects its lists with its own mux), so
 * xTaskRemoveFromEventList() stays usable from an ISR.
 *
 * vTaskInternalSetTimeOutState() is implemented by the wrapper's tasks.c but is
 * not declared in its task.h, so it is only declared here (see portmacro.h of
 * the BSP, which every ESP-IDF translation unit pulls in).
 */

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/list.h"
#include "freertos/portmacro.h"

#include "rtthread.h"

/* One waiter slot per task that can block in an esp_ringbuf operation.  The
   RT-Thread ESP BSPs do not use the ring buffer paths at runtime, the slots
   only have to be enough for the drivers that are linked in. */
#ifndef CONFIG_FREERTOS_RT_WRAPPER_EVENT_LIST_WAITERS
#define CONFIG_FREERTOS_RT_WRAPPER_EVENT_LIST_WAITERS 8
#endif

typedef struct {
    ListItem_t item;
    volatile BaseType_t woken;
    volatile BaseType_t in_use;
} rt_wrapper_event_waiter_t;

static rt_wrapper_event_waiter_t s_waiters[CONFIG_FREERTOS_RT_WRAPPER_EVENT_LIST_WAITERS];

static rt_wrapper_event_waiter_t *prvAllocateWaiter(void)
{
    rt_wrapper_event_waiter_t *waiter = NULL;
    uint32_t i;

    portENTER_CRITICAL(NULL);
    for (i = 0; i < CONFIG_FREERTOS_RT_WRAPPER_EVENT_LIST_WAITERS; i++) {
        if (s_waiters[i].in_use == pdFALSE) {
            s_waiters[i].in_use = pdTRUE;
            s_waiters[i].woken = pdFALSE;
            waiter = &s_waiters[i];
            break;
        }
    }
    portEXIT_CRITICAL(NULL);

    return waiter;
}

/**
 * Block the calling task on an event list.
 *
 * @param pxEventList  list to queue on
 * @param xTicksToWait timeout in ticks, portMAX_DELAY to wait forever
 */
void vTaskPlaceOnEventList(List_t *const pxEventList, TickType_t xTicksToWait)
{
    rt_wrapper_event_waiter_t *waiter = prvAllocateWaiter();
    TickType_t start;

    configASSERT(waiter != NULL);
    if (waiter == NULL) {
        return;
    }

    /* vListInsert() keeps the list ordered by xItemValue, highest first, which
       matches the "tasks are stored in priority order" contract of the FreeRTOS
       event lists. */
    vListInitialiseItem(&waiter->item);
    waiter->item.xItemValue = (TickType_t)uxTaskPriorityGet(NULL);
    vListInsert(pxEventList, &waiter->item);

    start = xTaskGetTickCount();
    while ((waiter->woken == pdFALSE) && ((xTaskGetTickCount() - start) < xTicksToWait)) {
        rt_thread_yield();
    }

    /* the timeout may have won the race against xTaskRemoveFromEventList() */
    portENTER_CRITICAL(NULL);
    if (waiter->item.pxContainer != NULL) {
        (void)uxListRemove(&waiter->item);
    }
    portEXIT_CRITICAL(NULL);

    waiter->in_use = pdFALSE;
}

/**
 * Unblock the highest priority task waiting on an event list.
 *
 * Safe to call from an ISR: it only touches the list and a flag, never blocks.
 *
 * @param pxEventList list to unblock a task from
 * @return pdTRUE if a task was unblocked, pdFALSE if the list was empty
 */
BaseType_t xTaskRemoveFromEventList(List_t *const pxEventList)
{
    rt_wrapper_event_waiter_t *waiter;
    ListItem_t *item = listGET_HEAD_ENTRY(pxEventList);

    if (listLIST_IS_EMPTY(pxEventList) == pdTRUE) {
        return pdFALSE;
    }

    (void)uxListRemove(item);
    waiter = (rt_wrapper_event_waiter_t *)item;
    waiter->woken = pdTRUE;

    return pdTRUE;
}
