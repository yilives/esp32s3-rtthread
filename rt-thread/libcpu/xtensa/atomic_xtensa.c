/*
 * Hardware atomics for Xtensa, via S32C1I.
 *
 * WHY THIS FILE HAS TO EXIST
 * --------------------------
 * RT-Thread's SMP scheduler calls rt_atomic_load()/rt_atomic_add() from
 * rt_cpus_lock() (src/cpu_mp.c:177-179). With RT_USING_HW_ATOMIC off, those
 * expand to rt_soft_atomic_load() (include/rtatomic.h:282), whose first act is
 * rt_hw_interrupt_disable(). Under RT_USING_SMP that name is a macro onto
 * rt_cpus_lock() itself (include/rthw.h:146). The result is unbounded recursion:
 *
 *     rt_cpus_lock
 *       -> rt_atomic_load
 *         -> rt_soft_atomic_load
 *           -> rt_hw_interrupt_disable   (== rt_cpus_lock)
 *             -> rt_atomic_load
 *               -> ...
 *
 * Measured on the ESP32-S3 dual-core build: the tick interrupt enters
 * rt_schedule, calls rt_cpus_lock, and the stack walks down through
 * rt_xt_isr_stack (4 KB), through the BSS next to it, into rt_xt_core[], and off
 * the bottom of DRAM. ESP32-S3 DRAM starts at 0x3FC88000; the crash lands with
 * sp = 0x3fc80020 and a9 = 0x3fc80000, and the resulting double exception is
 * why the board printed no panic message at all.
 *
 * Upstream requires RT_USING_HW_ATOMIC for SMP for exactly this reason: 297 other
 * BSPs define it. This port was the exception.
 *
 * WHY S32C1I
 * ----------
 * A plain aligned load IS indivisible on Xtensa and would break the recursion by
 * itself, but the kernel uses the whole set, and leaving load/store unguarded
 * while the read-modify-write operations still took a lock would be
 * inconsistent. S32C1I is the ISA's only read-modify-write: it stores only if the
 * word still equals SCOMPARE1. Every RMW here is "read, compute, s32c1i, retry
 * if the compare failed".
 *
 * S32C1I needs a 16-byte-aligned address (an unaligned one raises
 * LoadStoreAlignment), and it operates on 32-bit words only. The 8/16-bit
 * helpers therefore fall back to masking interrupts, which is all they need:
 * the kernel's 8/16-bit atomics are counters and flags, not inter-core
 * handshakes.
 *
 * ORDERING
 * --------
 * rsync after writing SCOMPARE1 makes it visible to the s32c1i that follows,
 * the same sequence rt_hw_spin_lock() uses in cpuport.c.
 */

#include <rtthread.h>
#include <rthw.h>
#include <rtatomic.h>

/*
 * IRAM_ATTR on every function in this file.
 *
 * These are the kernel's atomics, and under SMP the interrupt path reaches them:
 *
 *     _rtt_int_enter  ->  rt_interrupt_get_nest()  ->  rt_atomic_load()
 *                          ->  rt_hw_atomic_load()
 *
 * rt_interrupt_get_nest() disables interrupts around the load, and the whole
 * chain was moved into IRAM for that reason - except this file. Measured on
 * hardware: the BLE controller's ISR (r_rwble_isr_hack -> r_rwip_timer_hus_handler_hack
 * -> global_interrupt_disable -> xPortInIsrContext) ran while CPU0 was formatting
 * the FAT partition with the flash cache off, and the fault landed here:
 *
 *   Guru Meditation Error: Core 0 panic'ed (Cache error).
 *   PC : 0x420ba2c6   rt_hw_atomic_load at libcpu/xtensa/atomic_xtensa.c:105
 *   Backtrace: ... xPortInIsrContext -> global_interrupt_disable
 *              -> r_rwip_timer_hus_handler_hack -> r_rwble_isr_hack
 *
 * 0x420ba2c4 is flash. After this attribute it is IRAM, so the whole nest-read
 * chain is reachable with the cache off.
 *
 * esp_attr.h is the IDF header that defines IRAM_ATTR. The fallback keeps the
 * file compilable in a plain RT-Thread tree, where the attribute is spelled out
 * instead.
 */
#if defined(__has_include)
#  if __has_include("esp_attr.h")
#    include "esp_attr.h"
#  endif
#endif

#ifndef IRAM_ATTR
#  define IRAM_ATTR __attribute__((section(".iram1")))
#endif

#ifdef RT_USING_HW_ATOMIC

/*
 * One compare-and-store attempt.
 *
 * SCOMPARE1 MUST hold the value we expect to find. S32C1I then stores `newval`
 * only if memory still equals SCOMPARE1, and returns whatever was in memory
 * before. The caller compares that return value with `expected` to know whether
 * the store happened.
 *
 * This was wrong in the first version of this file: SCOMPARE1 was set to 0
 * unconditionally, so the compare only ever succeeded on a zero word. With a
 * non-zero word the store silently did nothing, yet S32C1I still returned the
 * current value - which equaled `expected`, so every caller believed it had
 * succeeded and returned. Every add/sub/or/and/xor on a non-zero atomic was a
 * no-op that reported success.
 *
 * Measured consequence: rt_cpus_unlock() ran 248 times, each doing
 * rt_atomic_sub(&cpus_lock_nest, 1), and `cpus_lock_nest` stayed at 1 -
 *
 *   [FSP] nest=1 cpus=1 subs=1 before=1 after=1
 *
 * That kept rt_cpus_unlock() from ever reaching its `== 0` branch, so
 * rt_exit_critical() was never called from it and the per-thread
 * critical_lock_nest never returned to 0. Everything the scheduler then did
 * with a nest of 1 (refusing to switch, refusing to block) followed from here.
 */
rt_inline rt_atomic_t xt_s32c1i(volatile rt_atomic_t *addr,
                                rt_atomic_t expected,
                                rt_atomic_t newval)
{
    rt_atomic_t got;

    __asm__ volatile(
        "wsr     %[exp], SCOMPARE1  \n"
        "rsync                      \n"
        "mov     %[got], %[new]     \n"
        "s32c1i  %[got], %[adr], 0  \n"
        : [got] "=&r"(got)
        : [exp] "r"(expected), [new] "r"(newval), [adr] "r"(addr)
        : "memory");

    return got;
}

rt_inline int xt_aligned16(const void *p)
{
    return ((rt_ubase_t)p & 0xf) == 0;
}

rt_atomic_t IRAM_ATTR rt_hw_atomic_load(volatile rt_atomic_t *ptr)
{
    return *ptr;
}

void IRAM_ATTR rt_hw_atomic_store(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    *ptr = val;
}

rt_atomic_t IRAM_ATTR rt_hw_atomic_exchange(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    if (!xt_aligned16((const void *)ptr))
    {
        rt_base_t lvl = rt_hw_local_irq_disable();
        rt_atomic_t old = *ptr;
        *ptr = val;
        rt_hw_local_irq_enable(lvl);
        return old;
    }

    for (;;)
    {
        rt_atomic_t old = *ptr;
        if (xt_s32c1i(ptr, old, val) == old)
        {
            return old;
        }
    }
}

rt_atomic_t IRAM_ATTR rt_hw_atomic_add(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    if (!xt_aligned16((const void *)ptr))
    {
        rt_base_t lvl = rt_hw_local_irq_disable();
        rt_atomic_t old = *ptr;
        *ptr = old + val;
        rt_hw_local_irq_enable(lvl);
        return old;
    }

    for (;;)
    {
        rt_atomic_t old = *ptr;
        if (xt_s32c1i(ptr, old, old + val) == old)
        {
            return old;
        }
    }
}

rt_atomic_t IRAM_ATTR rt_hw_atomic_sub(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    return rt_hw_atomic_add(ptr, -val);
}

rt_atomic_t IRAM_ATTR rt_hw_atomic_or(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    if (!xt_aligned16((const void *)ptr))
    {
        rt_base_t lvl = rt_hw_local_irq_disable();
        rt_atomic_t old = *ptr;
        *ptr = old | val;
        rt_hw_local_irq_enable(lvl);
        return old;
    }

    for (;;)
    {
        rt_atomic_t old = *ptr;
        if (xt_s32c1i(ptr, old, old | val) == old)
        {
            return old;
        }
    }
}

rt_atomic_t IRAM_ATTR rt_hw_atomic_and(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    if (!xt_aligned16((const void *)ptr))
    {
        rt_base_t lvl = rt_hw_local_irq_disable();
        rt_atomic_t old = *ptr;
        *ptr = old & val;
        rt_hw_local_irq_enable(lvl);
        return old;
    }

    for (;;)
    {
        rt_atomic_t old = *ptr;
        if (xt_s32c1i(ptr, old, old & val) == old)
        {
            return old;
        }
    }
}

rt_atomic_t IRAM_ATTR rt_hw_atomic_xor(volatile rt_atomic_t *ptr, rt_atomic_t val)
{
    if (!xt_aligned16((const void *)ptr))
    {
        rt_base_t lvl = rt_hw_local_irq_disable();
        rt_atomic_t old = *ptr;
        *ptr = old ^ val;
        rt_hw_local_irq_enable(lvl);
        return old;
    }

    for (;;)
    {
        rt_atomic_t old = *ptr;
        if (xt_s32c1i(ptr, old, old ^ val) == old)
        {
            return old;
        }
    }
}

rt_atomic_t IRAM_ATTR rt_hw_atomic_compare_exchange_strong(volatile rt_atomic_t *ptr,
                                                 rt_atomic_t *expected,
                                                 rt_atomic_t desired)
{
    rt_atomic_t old = *ptr;

    if (old != *expected)
    {
        *expected = old;
        return 0;
    }

    if (!xt_aligned16((const void *)ptr))
    {
        rt_base_t lvl = rt_hw_local_irq_disable();
        if (*ptr == *expected)
        {
            *ptr = desired;
            rt_hw_local_irq_enable(lvl);
            return 1;
        }
        *expected = *ptr;
        rt_hw_local_irq_enable(lvl);
        return 0;
    }

    for (;;)
    {
        rt_atomic_t want = *expected;
        rt_atomic_t got;

        old = *ptr;
        if (old != want)
        {
            *expected = old;
            return 0;
        }

        got = xt_s32c1i(ptr, want, desired);
        if (got == old)
        {
            return 1;
        }
        if (got != want)
        {
            *expected = got;
            return 0;
        }
    }
}

void IRAM_ATTR rt_hw_atomic_flag_clear(volatile rt_atomic_t *ptr)
{
    rt_hw_atomic_store(ptr, 0);
}

rt_atomic_t IRAM_ATTR rt_hw_atomic_flag_test_and_set(volatile rt_atomic_t *ptr)
{
    return rt_hw_atomic_exchange(ptr, 1) != 0;
}

/* ---- 8- and 16-bit: S32C1I is 32-bit only, so mask interrupts ---- */

rt_atomic8_t rt_hw_atomic_load8(volatile rt_atomic8_t *ptr)
{
    rt_base_t lvl = rt_hw_local_irq_disable();
    rt_atomic8_t v = *ptr;
    rt_hw_local_irq_enable(lvl);
    return v;
}

void IRAM_ATTR rt_hw_atomic_store8(volatile rt_atomic8_t *ptr, rt_atomic8_t val)
{
    rt_base_t lvl = rt_hw_local_irq_disable();
    *ptr = val;
    rt_hw_local_irq_enable(lvl);
}

rt_atomic16_t rt_hw_atomic_load16(volatile rt_atomic16_t *ptr)
{
    rt_base_t lvl = rt_hw_local_irq_disable();
    rt_atomic16_t v = *ptr;
    rt_hw_local_irq_enable(lvl);
    return v;
}

void IRAM_ATTR rt_hw_atomic_store16(volatile rt_atomic16_t *ptr, rt_atomic16_t val)
{
    rt_base_t lvl = rt_hw_local_irq_disable();
    *ptr = val;
    rt_hw_local_irq_enable(lvl);
}

rt_atomic8_t rt_hw_atomic_and8(volatile rt_atomic8_t *ptr, rt_atomic8_t val)
{
    rt_base_t lvl = rt_hw_local_irq_disable();
    rt_atomic8_t old = *ptr;
    *ptr = old & val;
    rt_hw_local_irq_enable(lvl);
    return old;
}

rt_atomic8_t rt_hw_atomic_or8(volatile rt_atomic8_t *ptr, rt_atomic8_t val)
{
    rt_base_t lvl = rt_hw_local_irq_disable();
    rt_atomic8_t old = *ptr;
    *ptr = old | val;
    rt_hw_local_irq_enable(lvl);
    return old;
}

rt_atomic16_t rt_hw_atomic_and16(volatile rt_atomic16_t *ptr, rt_atomic16_t val)
{
    rt_base_t lvl = rt_hw_local_irq_disable();
    rt_atomic16_t old = *ptr;
    *ptr = old & val;
    rt_hw_local_irq_enable(lvl);
    return old;
}

rt_atomic16_t rt_hw_atomic_or16(volatile rt_atomic16_t *ptr, rt_atomic16_t val)
{
    rt_base_t lvl = rt_hw_local_irq_disable();
    rt_atomic16_t old = *ptr;
    *ptr = old | val;
    rt_hw_local_irq_enable(lvl);
    return old;
}

#endif /* RT_USING_HW_ATOMIC */
