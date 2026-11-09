/*
 * SPDX-FileCopyrightText: 2026 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * ESP-IDF FreeRTOS API additions for the RT-Thread scons build.
 *
 * ESP-IDF v6.1 ships <freertos/idf_additions.h> together with
 * components/freertos/esp_additions/idf_additions.c.  The "WithCaps" family
 * of creation functions there is written against the static-allocation API of
 * the kernel in components/freertos/FreeRTOS-Kernel (FreeRTOS V11): it
 * allocates the control block and the data storage itself and then calls the
 * x...Static() constructor.  V11 also provides the FreeRTOSConfig.h indirection
 * (configNUMBER_OF_CORES) and the helpers (xTaskGetStaticBuffers(),
 * xQueueGetStaticBuffers(), vPortAssertIfInISR(), ...) that go with it.
 *
 * The RT-Thread ESP BSPs run on the FreeRTOS-Wrapper kernel (FreeRTOS V10.4.6)
 * instead, which has none of that: its objects are allocated by the wrapper's
 * own xQueueGenericCreate()/xTaskCreate() and it spells the core count
 * configNUM_CORES.  Implementing ESP-IDF's file here would therefore mean
 * back-porting a large part of V11's object model.
 *
 * The wrapper has a single heap (pvPortMalloc() is rt_thread_malloc()), so
 * the memory capabilities ESP-IDF asks for cannot be honoured anyway: this file
 * forwards every call to the plain dynamic API and drops the caps.  This is
 * the same trade-off the rest of the scons port makes - the caps only matter
 * when an application puts kernel objects into PSRAM, which the ESP BSPs never
 * do.
 *
 * The declarations the callers see come from ESP-IDF's own
 * <freertos/idf_additions.h>, which the root SConscript puts on the include
 * path; only the definitions live here.
 */

#include <stdint.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"

/* -------------------------------------------- Tasks ------------------------------------------- */

#if ( configSUPPORT_STATIC_ALLOCATION == 1 )

BaseType_t xTaskCreatePinnedToCoreWithCaps( TaskFunction_t pvTaskCode,
                                            const char * const pcName,
                                            const configSTACK_DEPTH_TYPE usStackDepth,
                                            void * const pvParameters,
                                            UBaseType_t uxPriority,
                                            TaskHandle_t * const pvCreatedTask,
                                            const BaseType_t xCoreID,
                                            UBaseType_t uxMemoryCaps )
{
    /* the wrapper has one heap, see the file comment */
    ( void ) uxMemoryCaps;
    return xTaskCreatePinnedToCore( pvTaskCode, pcName, usStackDepth, pvParameters,
                                   uxPriority, pvCreatedTask, xCoreID );
}

void vTaskDeleteWithCaps( TaskHandle_t xTaskToDelete )
{
    vTaskDelete( xTaskToDelete );
}

#endif /* configSUPPORT_STATIC_ALLOCATION == 1 */

/* ------------------------------------------- Queues ------------------------------------------ */

#if ( configSUPPORT_STATIC_ALLOCATION == 1 )

QueueHandle_t xQueueCreateWithCaps( UBaseType_t uxQueueLength,
                                    UBaseType_t uxItemSize,
                                    UBaseType_t uxMemoryCaps )
{
    /* the wrapper has one heap, see the file comment */
    ( void ) uxMemoryCaps;
    return xQueueGenericCreate( uxQueueLength, uxItemSize, queueQUEUE_TYPE_BASE );
}

void vQueueDeleteWithCaps( QueueHandle_t xQueue )
{
    vQueueDelete( xQueue );
}

#endif /* configSUPPORT_STATIC_ALLOCATION == 1 */

/* ----------------------------------------- Semaphores ---------------------------------------- */

SemaphoreHandle_t xSemaphoreCreateGenericWithCaps( UBaseType_t uxMaxCount,
                                                   UBaseType_t uxInitialCount,
                                                   const uint8_t ucQueueType,
                                                   UBaseType_t uxMemoryCaps )
{
    /* the wrapper has one heap, see the file comment */
    ( void ) uxMemoryCaps;

    /* ESP-IDF passes 0 for the binary and mutex variants, the wrapper wants the
       queue length and item size of the matching plain constructor instead. */
    if( ( ucQueueType == queueQUEUE_TYPE_BINARY_SEMAPHORE ) ||
        ( ucQueueType == queueQUEUE_TYPE_MUTEX ) ||
        ( ucQueueType == queueQUEUE_TYPE_RECURSIVE_MUTEX ) )
    {
        uxMaxCount = 1;
    }

    return xQueueGenericCreate( uxMaxCount, 0, ucQueueType );
}

void vSemaphoreDeleteWithCaps( SemaphoreHandle_t xSemaphore )
{
    vQueueDelete( ( QueueHandle_t ) xSemaphore );
}

/* --------------------------------------- Event groups --------------------------------------- */

#if ( configSUPPORT_STATIC_ALLOCATION == 1 )

EventGroupHandle_t xEventGroupCreateWithCaps( UBaseType_t uxMemoryCaps )
{
    /* the wrapper has one heap, see the file comment */
    ( void ) uxMemoryCaps;
    return xEventGroupCreate();
}

void vEventGroupDeleteWithCaps( EventGroupHandle_t xEventGroup )
{
    vEventGroupDelete( xEventGroup );
}

#endif /* configSUPPORT_STATIC_ALLOCATION == 1 */
