/*
 * Link probe: take the address of every FreeRTOS function the IDF headers
 * promise. If one of them has no definition, the link fails here with a name,
 * instead of in some component later with a wall of linker output.
 *
 * This file is deliberately never called. Its only job is to be compiled and
 * linked, so the linker has to resolve every symbol in the table. Keep it in the
 * build: it costs one page of flash and converts "we think the shim is complete"
 * into something the build actually checks.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"

/* Anything in here that the linker cannot resolve is a gap.
 *
 * Getting the linker to actually check this is the whole difficulty, and the
 * first two attempts failed silently:
 *
 *   1. A plain const array. --gc-sections dropped the section before its
 *      contents were resolved, so the link succeeded whether or not the symbols
 *      existed. The image did not even contain the table.
 *   2. __attribute__((used, retain)). `used` only stops the compiler removing
 *      it, and ESP-IDF's linker script has no KEEP() for this section, so the
 *      linker still dropped it. Same silent pass.
 *
 * What works is a real reference from reachable code: selftest.c reads
 * ft_link_probe[0] from the s31test command, and msh's command table is KEEP()'d,
 * so this section survives and every entry has to resolve.
 *
 * Consequence worth knowing: the table now costs 72 pointers of flash. If that
 * ever matters, delete the file and rely on the fact that a missing definition
 * only hurts when something calls it - which is what the last three rounds were
 * about, and why this file exists. */
const void *ft_link_probe[] = {
    /* task.h */
    (const void *)xTaskCreatePinnedToCore,
    (const void *)xTaskCreatePinnedToCoreWithCaps,
    (const void *)vTaskDelete,
    (const void *)vTaskDelay,
    (const void *)xTaskDelayUntil,
    (const void *)vTaskSuspend,
    (const void *)vTaskResume,
    (const void *)xTaskResumeFromISR,
    (const void *)xTaskAbortDelay,
    (const void *)xTaskGetTickCount,
    (const void *)xTaskGetTickCountFromISR,
    (const void *)xTaskGetSchedulerState,
    (const void *)xTaskGetCurrentTaskHandle,
    (const void *)xTaskGetIdleTaskHandle,
    (const void *)uxTaskGetStackHighWaterMark,
    (const void *)uxTaskGetNumberOfTasks,
    (const void *)pcTaskGetName,
    (const void *)xTaskGetHandle,
    (const void *)eTaskGetState,
    (const void *)vTaskPrioritySet,
    (const void *)uxTaskPriorityGet,
    (const void *)vTaskSuspendAll,
    (const void *)xTaskResumeAll,
    (const void *)vTaskSetTimeOutState,
    (const void *)xTaskCheckForTimeOut,
    (const void *)xTaskGetCurrentTaskHandleForCore,
    (const void *)xTaskGetIdleTaskHandleForCore,

    /* queue.h. xQueueOverwrite and xQueueOverwriteFromISR are deliberately
     * absent: they are function-like macros over xQueueGenericSend(FromISR),
     * which is in the table already, so there is no symbol to check. */
    (const void *)xQueueGenericCreate,
    (const void *)xQueueGenericCreateStatic,
    (const void *)xQueueCreateWithCaps,
    (const void *)vQueueDelete,
    (const void *)vQueueDeleteWithCaps,
    (const void *)xQueueGenericSend,
    (const void *)xQueueGenericSendFromISR,
    (const void *)xQueueReceive,
    (const void *)xQueueReceiveFromISR,
    (const void *)xQueuePeek,
    (const void *)xQueueGenericReset,
    (const void *)uxQueueMessagesWaiting,
    (const void *)uxQueueMessagesWaitingFromISR,
    (const void *)uxQueueSpacesAvailable,
    (const void *)xQueueIsQueueEmptyFromISR,
    (const void *)xQueueIsQueueFullFromISR,
    (const void *)xQueueCreateSet,
    (const void *)xQueueAddToSet,
    (const void *)xQueueRemoveFromSet,
    (const void *)xQueueSelectFromSet,
    (const void *)xQueueSelectFromSetFromISR,

    /* semphr.h */
    (const void *)xQueueCreateMutex,
    (const void *)xQueueCreateMutexStatic,
    (const void *)xQueueCreateCountingSemaphore,
    (const void *)xQueueCreateCountingSemaphoreStatic,
    (const void *)xSemaphoreCreateBinaryWithCaps,
    (const void *)xSemaphoreCreateMutexWithCaps,
    (const void *)xSemaphoreCreateCountingWithCaps,
    (const void *)vSemaphoreDeleteWithCaps,
    (const void *)xQueueSemaphoreTake,
    (const void *)xQueueGiveFromISR,
    (const void *)xQueueTakeMutexRecursive,
    (const void *)xQueueGiveMutexRecursive,
    (const void *)xQueueGetMutexHolder,

    /* event_groups.h. xEventGroupClearBitsFromISR is a function-like macro in
     * this configuration (configUSE_TRACE_FACILITY is off), expanding to
     * xTimerPendFunctionCallFromISR - a timers.c symbol the shim does not
     * provide. Nothing calls it, and taking its address is not possible. */
    (const void *)xEventGroupCreate,
    (const void *)vEventGroupDelete,
    (const void *)xEventGroupSetBits,
    (const void *)xEventGroupClearBits,
    (const void *)xEventGroupGetBitsFromISR,
    (const void *)xEventGroupWaitBits,

    /* task notifications, reached through the generic layer */
    (const void *)xTaskGenericNotify,
    (const void *)xTaskGenericNotifyFromISR,
    (const void *)xTaskGenericNotifyWait,
    (const void *)ulTaskGenericNotifyTake,
    (const void *)xTaskGenericNotifyStateClear,
};

/* Number of entries, for the self test to report and to keep the table alive. */
const unsigned ft_link_probe_count = sizeof(ft_link_probe) / sizeof(ft_link_probe[0]);
