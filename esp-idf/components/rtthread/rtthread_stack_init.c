/* SPDX-License-Identifier: Apache-2.0 */

#include "sdkconfig.h"
#include <rtthread.h>
#include "rt_hw_stack_frame.h"

extern rt_uint8_t *rt_hw_stack_init_core(void *entry, void *parameter,
                                       rt_uint8_t *stack_addr, void *exit);

rt_uint8_t *rt_hw_stack_init(void *entry, void *parameter,
                           rt_uint8_t *stack_addr, void *exit)
{
    rt_uintptr_t top = RT_ALIGN_DOWN((rt_uintptr_t)stack_addr + sizeof(rt_ubase_t),
                                   RT_ALIGN_SIZE);
    rt_ubase_t threadptr = 0;

#if CONFIG_LIBC_PICOLIBC
    /* IDF's RISC-V ABI uses tp for GCC TLS, including picolibc's errno.
     * Reserve the TLS image above the initial frame with 16-byte alignment.
     * It remains part of the thread's allocation until that stack is freed,
     * and both cooperative and interrupt returns restore a valid per-thread tp.
     */
    extern char _thread_local_data_start[], _thread_local_data_end[];
    extern char _thread_local_bss_start[], _thread_local_bss_end[];
    rt_size_t data_size = (rt_uintptr_t)_thread_local_data_end -
                          (rt_uintptr_t)_thread_local_data_start;
    rt_size_t bss_size = (rt_uintptr_t)_thread_local_bss_end -
                         (rt_uintptr_t)_thread_local_bss_start;
    rt_size_t tls_size = RT_ALIGN(data_size + bss_size, 16);
    top = RT_ALIGN_DOWN(top - tls_size, 16);
    rt_memcpy((void *)top, _thread_local_data_start, data_size);
    rt_memset((void *)(top + data_size), 0, bss_size);
    threadptr = top;
#endif

    rt_hw_stack_frame_t *frame = (rt_hw_stack_frame_t *)rt_hw_stack_init_core(
        entry, parameter, (rt_uint8_t *)(top - sizeof(rt_ubase_t)), exit);
    __asm__ volatile ("mv %0, gp" : "=r" (frame->gp));
    frame->tp = threadptr;
    return (rt_uint8_t *)frame;
}
