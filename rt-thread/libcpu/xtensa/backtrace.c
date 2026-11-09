/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, Xtensa windowed-ABI backtrace
 *
 * Xtensa (windowed ABI) backtrace for RT-Thread.
 *
 * The kernel's generic walker in kservice.c assumes a frame-pointer chain
 * (`fp` of frame i points at frame i-1). Xtensa has no frame pointer and no
 * such chain: a call does not push a return address onto the stack, it rotates
 * the register window, and the return address lives in a0 of the *caller's*
 * window. When the window overflows, the hardware writes the 4-register
 * base-save area to `sp - 16 .. sp - 1`. That area is the only stack-resident
 * link between a frame and its caller:
 *
 *     [sp - 16] a0 of the caller   (i.e. the caller's return address, frame i-1)
 *     [sp - 12] sp of the caller   (frame i-1's stack pointer)
 *     [sp -  8] a2
 *     [sp -  4] a3
 *
 * So walking the stack is: pc/next_pc/sp -> read (sp-16) and (sp-12) -> repeat.
 * This is the same relation ESP-IDF's esp_backtrace_get_next_frame() uses; the
 * difference here is that the starting frame comes from an RT-Thread thread's
 * saved context instead of a live CPU, and that every pointer is validated
 * against that thread's own stack before it is dereferenced.
 *
 * Two frame layouts can be sitting at thread->sp, and the first word says which
 * (see cpuport.h):
 *
 *   XtSolFrame   first word == 0            -> a0 is in the base-save area at
 *                                             sp+0x10, sp in XT_SOL_SP
 *   FP block     first word == XT_RT_FP_FRAME_MARK
 *                                           -> the XtExcFrame is
 *                                             XT_RT_FP_BLOCK_SIZE above, with
 *                                             a0/a1 at XT_STK_A0/XT_STK_A1
 *
 * A thread that never ran has no link at all, so its walk yields its entry
 * point and stops - which is the honest answer.
 */

/* MSH_CMD_EXPORT casts the handler to `long (*)(void)`, which is upstream's
 * design and trips -Werror=cast-function-type. */
#pragma GCC diagnostic ignored "-Wcast-function-type"

#include <rtthread.h>
#include <rthw.h>
#include <xtensa_context.h>

#include "cpuport.h"

#define XT_RT_BT_MAGIC  0x4241434bUL  /* "BACK" */

/*
 * A saved a0 is not a plain address.
 *
 * In the windowed ABI a call4/8/12 stores the return address with the call
 * increment in the top two bits (CALLINC in bits 31:30), because the window
 * rotation uses them to know how far to rotate back. A saved a0 therefore reads
 * like 0x8037e279 for code that actually lives at 0x4037e279, and a validator
 * that compares the raw value against the executable ranges rejects every real
 * frame.
 *
 * The transformation is not a plain mask. The convention (ESP-IDF's
 * esp_cpu_process_stack_pc) is: if bit 31 is set, the address belongs to the
 * 0x40000000 segment, so mask off the CALLINC bits and OR 0x40000000 back in.
 * Masking alone would produce 0x0037e279 - a flash offset that is not mapped on
 * this chip - and every walk would still terminate immediately.
 *
 * Measured both ways: with the raw value nothing walked, and with a plain mask
 * the printed addresses (0x0037xxxx) did not resolve in the ELF either.
 */
#define XT_RT_BT_PROCESS_PC(pc)                                 \
    (((pc) & 0x80000000UL) ? (((pc) & 0x3FFFFFFFUL) | 0x40000000UL) : (pc))

/*
 * Reject a pointer that cannot be part of this thread's stack.
 *
 * `stack_addr` is the low end and grows up to `stack_addr + stack_size`; Xtensa
 * stacks grow down, so a valid sp satisfies
 *     stack_addr <= sp <= stack_addr + stack_size
 *
 * The bounds are checked before every load. Without this a stale or corrupted
 * sp in a TCB sends the walker into arbitrary memory, and on this chip a load
 * from an unmapped address is a panic - a backtrace that crashes the system it
 * is trying to describe is worse than no backtrace.
 */
static rt_bool_t xt_rt_bt_ptr_ok(rt_thread_t thread, rt_uintptr_t p)
{
    rt_uintptr_t lo;
    rt_uintptr_t hi;

    RT_ASSERT(thread != RT_NULL);
    RT_ASSERT(thread->stack_addr != RT_NULL);
    RT_ASSERT(thread->stack_size != 0);

    lo = (rt_uintptr_t)thread->stack_addr;
    hi = lo + thread->stack_size;

    /* The walker reads sp-16 and sp-12, so the frame has to be at least one
     * base-save area above the bottom of the stack. */
    return (p >= lo + 16) && (p <= hi) && ((p & 0x3) == 0);
}

/* A PC recovered from a saved a0 has to look like code. The CALLINC bits have
 * to be removed first (see XT_RT_BT_PROCESS_PC); after that only the segment
 * matters, because alignment is 2 bytes - Xtensa's code-density option
 * (XCHAL_HAVE_DENSITY, on for every ESP32) allows 16-bit instructions, so valid
 * addresses end in 0b10 as well as 0b00. Requiring 4-byte alignment rejected
 * real frames: measured, `xtbt tidle0` stopped on 0x4037ddea.
 */
static rt_bool_t xt_rt_bt_pc_ok(rt_uintptr_t pc)
{
    pc = XT_RT_BT_PROCESS_PC(pc);

    if (pc == 0)
    {
        return RT_FALSE;
    }

    if ((pc & 0x1) != 0)
    {
        return RT_FALSE;
    }

    /* IROM / IRAM / RTC text. Anything else cannot hold instructions here. */
    if ((pc >= 0x40000000 && pc < 0x40400000) ||
        (pc >= 0x42000000 && pc < 0x44000000) ||
        (pc >= 0x50000000 && pc < 0x50100000))
    {
        return RT_TRUE;
    }

    /* flash-mapped addresses also appear in the 0x003xxxxx window on some
     * mapping configurations; accept it so a valid chain is not cut short. */
    if (pc >= 0x00200000 && pc < 0x01000000)
    {
        return RT_TRUE;
    }

    return RT_FALSE;
}

/*
 * Recover the innermost frame of a thread from its saved context.
 *
 * Returns -RT_ENOSYS for a thread that has never run: its stack_init context
 * carries an entry point in the frame but no base-save area, so the caller gets
 * one address and nothing to unwind.
 */
rt_err_t rt_hw_backtrace_frame_get(rt_thread_t thread, struct rt_hw_backtrace_frame *frame)
{
    rt_uintptr_t sp;
    rt_uintptr_t first;

    if (thread == RT_NULL || frame == RT_NULL)
    {
        return -RT_EINVAL;
    }

    sp = (rt_uintptr_t)thread->sp;
    if (!xt_rt_bt_ptr_ok(thread, sp))
    {
        return -RT_EINVAL;
    }

    first = *(volatile rt_uintptr_t *)sp;

    if (first == XT_RT_FP_FRAME_MARK)
    {
        /* Interrupted thread: an XtExcFrame sits above the FP block, and the
         * frame's saved a0/a1 are the link to the caller. `pc` is used rather
         * than `a0` because a thread that was preempted while blocked in a
         * kernel call has its *return address* in a0 - pointing at the caller
         * of the function it is inside - while `pc` is where it actually
         * stopped. Printing both would be ideal; the kernel's struct only has
         * one pc slot, so the interrupted instruction wins. */
        XtExcFrame *exc = (XtExcFrame *)(sp + XT_RT_FP_BLOCK_SIZE);

        frame->pc = (rt_uintptr_t)exc->pc;
        frame->fp = (rt_uintptr_t)exc->a1;
    }
    else
    {
        /* Cooperatively switched thread: XtSolFrame, whose own struct carries
         * a0/a1 at XT_SOL_A0/XT_SOL_A1 (sp + 0x10 / 0x14). Those are the
         * yielding thread's return address and stack pointer, i.e. exactly the
         * same pair the interrupt path takes from the exception frame - which is
         * why one walker serves both.
         *
         * The first word is NOT reliably 0 for this kind: it holds XT_SOL_EXIT
         * (cpuport.c writes XT_RT_SOL_YIELDED there), but a thread that has been
         * resumed and re-switched may carry a leftover value. So anything that
         * is not the FP marker is treated as solicited, and the sanity checks
         * below are what reject a genuinely bogus context. */
        rt_uintptr_t *sol = (rt_uintptr_t *)sp;

        frame->pc = sol[4];     /* XT_SOL_A0 */
        frame->fp = sol[5];     /* XT_SOL_A1 */
    }

    /* Normalise before the check: a saved a0 carries the CALLINC bits, and
     * every consumer (rt_backtrace_frame, addr2line) wants the real address. */
    frame->pc = XT_RT_BT_PROCESS_PC(frame->pc);

    /* A thread that has not run yet has no link; say so instead of walking
     * into whatever the initial frame happens to contain. */
    if (!xt_rt_bt_pc_ok(frame->pc) || frame->fp == 0)
    {
        return -RT_ENOSYS;
    }

    return RT_EOK;
}

/*
 * Step one frame outwards.
 *
 * The relation is the base-save area: [fp-16] is the caller's a0 (frame i-1's
 * return address) and [fp-12] is the caller's sp. `frame->pc` becomes the PC of
 * the frame being entered and `frame->fp` its stack pointer.
 *
 * Returns non-zero when the walk is finished - the same convention the kernel's
 * rt_backtrace_frame() expects (`if (rt_hw_backtrace_frame_unwind(...)) break;`).
 */
rt_err_t rt_hw_backtrace_frame_unwind(rt_thread_t thread, struct rt_hw_backtrace_frame *frame)
{
    rt_uintptr_t next_a0;
    rt_uintptr_t next_sp;
    rt_uintptr_t *base_save;

    if (thread == RT_NULL || frame == RT_NULL)
    {
        return -RT_EINVAL;
    }

    if (!xt_rt_bt_ptr_ok(thread, frame->fp))
    {
        return -RT_ENOSYS;
    }

    base_save = (rt_uintptr_t *)frame->fp;
    next_a0 = base_save[-4];    /* [fp - 16] */
    next_sp = base_save[-3];    /* [fp - 12] */

    /* A zero a0 terminates the chain: it is what rt_hw_stack_init writes there
     * for a thread's outermost frame. */
    if (next_a0 == 0 || !xt_rt_bt_pc_ok(next_a0))
    {
        return -RT_ENOSYS;
    }

    /* Same frame means the walk has stopped making progress (a thread that only
     * ever called itself), so stop rather than spin. */
    if (next_sp == frame->fp || !xt_rt_bt_ptr_ok(thread, next_sp))
    {
        return -RT_ENOSYS;
    }

    frame->pc = XT_RT_BT_PROCESS_PC(next_a0);
    frame->fp = next_sp;

    return RT_EOK;
}

/* ------------------------------------------------------------------ msh -- */

/*
 * Deliberately NOT static.
 *
 * `-u sym` creates an *external* undefined reference, which a local symbol cannot
 * satisfy, so a static handler can never be used as an anchor (`ld` accepts the
 * dangling reference silently - `-u` means "force", not "require"). And anchoring
 * on this file's own rt_hw_backtrace_frame_* does not work either: kservice.c
 * defines weak versions in the same archive, so the linker satisfies the forced
 * reference with those and never pulls this member in at all. Measured on the
 * board: "rt_hw_backtrace_frame_unwind is not implemented" while this object sat
 * unused inside the archive.
 *
 * A non-static name nothing else defines is therefore the only reliable anchor,
 * which is why this one is exported.
 */
int xt_rt_backtrace_cmd(int argc, char **argv)
{
    rt_thread_t thread;
    char name[RT_NAME_MAX + 1];
    rt_uintptr_t sp;
    rt_uintptr_t w0;

    if (argc >= 2)
    {
        rt_strncpy(name, argv[1], RT_NAME_MAX);
        name[RT_NAME_MAX] = '\0';
        thread = rt_thread_find(name);
        if (thread == RT_NULL)
        {
            rt_kprintf("xtbt: no thread named '%s'\n", name);
            return -1;
        }
    }
    else
    {
        thread = rt_thread_self();
    }

    sp = (rt_uintptr_t)thread->sp;

    rt_kprintf("xtbt: %s sp %p stack %p+%u status 0x%02x\n",
               thread->parent.name, thread->sp, thread->stack_addr,
               (unsigned)thread->stack_size, (unsigned)RT_SCHED_CTX(thread).stat);

    /*
     * A RUNNING thread has no saved frame to walk: thread->sp is only updated
     * when the scheduler switches a thread out, so for the thread executing this
     * command the field still holds the sp from its last switch-in, which is
     * already stale. Saying so is more useful than walking it.
     */
    if (thread == rt_thread_self())
    {
        rt_kprintf("      this thread is running, so it has no saved frame;\n");
        rt_kprintf("      backtrace another thread, or trigger a fault\n");
        return 0;
    }

    if (!xt_rt_bt_ptr_ok(thread, sp))
    {
        rt_kprintf("      sp is outside the thread's stack\n");
        return -1;
    }

    w0 = *(volatile rt_uintptr_t *)sp;

    if (w0 == XT_RT_FP_FRAME_MARK)
    {
        XtExcFrame *exc = (XtExcFrame *)(sp + XT_RT_FP_BLOCK_SIZE);

        rt_kprintf("      saved frame: FP block, XtExcFrame at %p\n", (void *)exc);
        rt_kprintf("      pc %p  a0 %p  a1 %p  ps 0x%08x\n",
                   (void *)(rt_uintptr_t)XT_RT_BT_PROCESS_PC((rt_uintptr_t)exc->pc),
                   (void *)(rt_uintptr_t)XT_RT_BT_PROCESS_PC((rt_uintptr_t)exc->a0),
                   (void *)exc->a1, (unsigned)exc->ps);
    }
    else
    {
        rt_kprintf("      saved frame: solicited, first word 0x%08x\n",
                   (unsigned)w0);
        rt_kprintf("      pc %p  ps 0x%08x\n",
                   (void *)(rt_uintptr_t)XT_RT_BT_PROCESS_PC(
                       *(volatile rt_uintptr_t *)(sp + 4)),
                   (unsigned)(*(volatile rt_uintptr_t *)(sp + 8)));
    }

    return (int)rt_backtrace_thread(thread);
}MSH_CMD_EXPORT_ALIAS(xt_rt_backtrace_cmd, xtbt, backtrace an Xtensa thread by name);
