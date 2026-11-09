/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 (Xtensa LX7) port
 *
 * ESP32-S3 (Xtensa LX7) context switch and RTOS callouts for the ESP-IDF
 * Xtensa porting layer.
 *
 * Division of labour with ESP-IDF
 * -------------------------------
 * This file deliberately does NOT contain the vector table. ESP-IDF's
 * `components/xtensa/xtensa_vectors.S` is OS agnostic: it builds the
 * XtExcFrame, saves/restores the register windows (`_xt_context_save` /
 * `_xt_context_restore` in xtensa_context.S), walks `_xt_interrupt_table`,
 * implements the level 2..6 and debug/NMI vectors, the coprocessor exception
 * path and `_xt_panic`, and calls out to the RTOS through exactly five macros.
 * Defining those five for RT-Thread (see xtensa_rtos.h in the freertos
 * component) makes every one of those behaviours available here unmodified -
 * which is the whole reason this port is a few hundred lines instead of a
 * thousand.
 *
 * What lives here:
 *   _rtt_int_enter   XT_RTOS_INT_ENTER   - save context, move to the ISR stack
 *   _rtt_int_exit    XT_RTOS_INT_EXIT    - restore, or perform the switch
 *   _rtt_resume                          - resume a thread from its saved sp
 *   _rtt_coproc_state / _rtt_coproc_exc_hook   (coprocessor bookkeeping)
 *   rt_hw_context_switch / _to / _interrupt
 *   rt_hw_interrupt_disable / _enable
 *
 * FPU policy
 * ----------
 * ESP32-S3 has a single-precision FPU as coprocessor 0 (XCHAL_CP_NUM == 2,
 * XCHAL_CP0_SA_SIZE == 72) and f0-f15 are *caller* saved in the windowed ABI.
 * That gives two different rules:
 *
 *   - Cooperative switch: f0-f15 need no save - GCC spilled every live value
 *     before calling into the kernel. FCR and FSR are control registers that
 *     the ABI does not cover, so they are saved into the 16 spare bytes above
 *     the solicited frame's base-save area.
 *   - Interrupt: f0-f15 are live in the interrupted instruction stream and no
 *     frame slot covers them, so level-1 entry saves FCR/FSR/f0-f15 eagerly
 *     into an 80-byte block below the XtExcFrame. thread->sp points at that
 *     block and the resume path steps over it. Sixteen `ssi` on entry and
 *     sixteen `lsi` on the way out buy the removal of lazy coprocessor
 *     ownership entirely.
 *
 * CPENABLE keeps CP0 enabled for thread code and is cleared while the C ISR
 * dispatch runs: no coprocessor instruction is legal outside a thread, and
 * clearing the enable bit turns a violation into a CoprocessorDisabled
 * exception instead of silent corruption of the interrupted thread's registers.
 * Every restore path therefore re-enables CP0 *before* touching an FP register
 * - doing it the other way round raises CoprocessorDisabled while interrupts
 * are masked, which is a DoubleException.
 *
 * Frame kinds at thread->sp
 * -------------------------
 *   0 in the first word            -> solicited frame (rt_hw_context_switch)
 *   XT_RT_FP_FRAME_MARK otherwise  -> FP block below an exception frame
 * See cpuport.h for the layouts and why both exist.
 */

#include <xtensa/config/core.h>
#include <xtensa/config/system.h>
#include <xtensa/coreasm.h>
#include <xtensa/corebits.h>
#include <xtensa_context.h>

/* RT_USING_SMP has to reach the assembler.
 *
 * It is not a -D flag (components/rtthread/CMakeLists.txt passes no RVCT/arch
 * defines; everything kernel-related arrives through rtconfig.h), so the two
 * #ifdefs below - the ABI of rt_hw_context_switch_to / _interrupt, and the
 * rt_hw_local_irq_* aliases - would silently take the UP branch in an SMP
 * build. Measured: "undefined reference to rt_hw_local_irq_disable" from
 * cpu_mp.c, scheduler_mp.c, irq.c and this port's own secondary entry, with
 * every one of them already implemented a few lines further down.
 *
 * rtconfig.h is safe to include here: it is a header of nothing but #defines
 * (checked), and it is on the assembler's include path already. */
#include "rtconfig.h"

#include "cpuport.h"

    .extern _xt_context_save
    .extern _xt_context_restore
    .extern xthal_window_spill_nw
    .extern rt_xt_core

#if !XCHAL_HAVE_WINDOWED
#error this port requires the Xtensa windowed register ABI (ESP32-S3 LX7)
#endif
#if !XCHAL_HAVE_XEA2
#error this port requires XEA2 vector layout (ESP32-S3 LX7)
#endif

/* The per-core offsets come from cpuport.h, which is included above: it defines
 * them outside its __ASSEMBLER__ guard exactly so one definition serves both
 * sides. cpuport.c asserts every one against offsetof(). */

/*
-------------------------------------------------------------------------------
  XT_CORE_BASE reg, tmp     --  reg = &rt_xt_core[cpu_id]

  SMP: rsr PRID, shift by the power-of-two stride, add the table base. Two
  scratch registers, no multiply, no lookup table.

  UP: one `movi`. There is one core and its PRID is always 0, so reading PRID
  and shifting by zero would be two wasted instructions on every interrupt
  entry - and, more usefully, it keeps the UP instruction sequence identical to
  the single-core version this port shipped with, so an SMP regression cannot be
  mistaken for a UP one.

  Clobbers both operands.
-------------------------------------------------------------------------------
*/
#ifdef RT_USING_SMP
    .macro  XT_CORE_BASE reg, tmp
    rsr.prid \reg
    /* PRID is not a small integer: the whole register is 0x6000 on the PRO core
     * and 0x0000 on the APP core, with the core number in bit 13. Shifting the
     * raw value would index rt_xt_core[24576]. IDF extracts the same bit with
     * `extui %0,%0,13,1` (components/xtensa/include/xt_utils.h:37-40). */
    extui   \reg, \reg, 13, 1
    slli    \reg, \reg, XT_RT_CORE_SHIFT
    movi    \tmp, rt_xt_core
    add     \reg, \reg, \tmp
    .endm
#else
    .macro  XT_CORE_BASE reg, tmp
    movi    \reg, rt_xt_core
    .endm
#endif

/*
-------------------------------------------------------------------------------
  XT_NEST_ADD reg, tmp, delta   --  rt_cpu_self()->irq_nest += delta

  The KERNEL's interrupt counter, which is a different variable from the port's
  own `nest`: rt_schedule() reads it (via rt_interrupt_get_nest()) to decide
  between the cooperative and the interrupt switch path, so it must be updated
  on every entry and every exit regardless of nesting.

  Under RT_USING_SMP the kernel defines it as `rt_cpu_self()->irq_nest`, which
  is a function call and a struct offset - neither of which belongs in a vector
  entry. So the port caches the address per core in XT_CORE_OFS_IRQ_NEST and
  updates the target directly. The slot is NULL until rt_xt_cpu_init() runs,
  and every entry before that point is a boot-time interrupt with no scheduler
  to inform, so NULL is tolerated rather than fatal.
-------------------------------------------------------------------------------
*/
    .macro  XT_NEST_ADD reg, tmp, delta
    XT_CORE_BASE \reg, \tmp
    l32i    \reg, \reg, XT_CORE_OFS_IRQ_NEST
    beqz    \reg, 9f
    l32i    \tmp, \reg, 0
    addi    \tmp, \tmp, \delta
    s32i    \tmp, \reg, 0
9:
    .endm


/*
-------------------------------------------------------------------------------
  XT_SET_CURRENT tcbreg, tmp1, tmp2   --  pcpu->current_thread = tcbreg

  The kernel keeps the running thread in pcpu->current_thread, and this port has
  to publish it: rt_thread_self(), rt_critical_level() and rt_enter_critical()
  all read that pointer, and rt_cpu_self() is called all over the scheduler.

  Inline, using the address cached by rt_xt_cpu_init(), on purpose. Calling C
  from the switch path is where this kept breaking: callers use `call0` (no
  window rotation) while a C callee needs `call8`, and the mismatch fails
  silently - measured as the boot stopping right after the banner.

  A null cached pointer means rt_xt_cpu_init() has not run yet (an interrupt
  before rt_hw_board_init); skipping the store is right there, because there is
  no scheduler to inform.
-------------------------------------------------------------------------------
*/
    .macro  XT_SET_CURRENT tcbreg, tmp1, tmp2
    XT_CORE_BASE \tmp1, \tmp2
    l32i    \tmp2, \tmp1, XT_CORE_OFS_CUR_PTR
    beqz    \tmp2, 9f
    s32i    \tcbreg, \tmp2, 0
9:
    .endm


    .text

/*
-------------------------------------------------------------------------------
  void _rtt_int_enter(void)          -- XT_RTOS_INT_ENTER

  Called by the IDF vector after the minimal frame (A1/PS/PC/A0/EXIT) exists,
  with PS.EXCM set, i.e. with interrupts disabled and *no C environment* - a
  windowed-ABI C function cannot be called here, which is why every routine in
  this file is hand-written assembly.

  Outermost entry (nest was 0):
    - save the remaining context (a2-a15, sar, loop regs, NCP extra state,
      register windows) with _xt_context_save
    - push the FP save block below the frame and remember it in this core's
      that address becomes the thread's saved sp
    - clear CPENABLE and continue on the dedicated ISR stack

  Nested entry (nest > 0): only the counter changes. The interrupted context is
  already C ISR code on the ISR stack, its frame is right here, CPENABLE is
  already clear and the FP block of the preempted thread is already safe.
-------------------------------------------------------------------------------
*/
    .align  4
    .global _rtt_int_enter
    .type   _rtt_int_enter, @function
_rtt_int_enter:
    /* _xt_context_save expects the caller to have stored a12/a13 already. */
    s32i    a12, sp, XT_STK_A12
    s32i    a13, sp, XT_STK_A13

    /* Park the return address: after the call a0 and a12 both hold it. */
    mov     a12, a0
    call0   _xt_context_save

    /*
     * Two counters, incremented together:
     *
     *   nest slot        the port's own, deciding outermost-vs-nested below
     *   rt_interrupt_nest the KERNEL's, which rt_schedule() reads through
     *                     rt_interrupt_get_nest() to choose between a
     *                     cooperative switch and an interrupt-time one
     *
     * The kernel counter is not decoration. Without it rt_interrupt_get_nest()
     * returns 0 while a C ISR is running, so rt_schedule() takes the
     * COOPERATIVE path from inside the interrupt: rt_hw_context_switch() leaves
     * a solicited frame and jumps to the next thread without ever unwinding this
     * vector's XtExcFrame. The port's nest then stays stuck at 1 and its frame
     * slot keeps pointing at the preempted thread, so the next interrupt is
     * treated as nested, its exit decrements to 0 and resumes from a stale
     * frame. Measured as an intermittent "Guru Meditation Error ...
     * InstrFetchProhibited, PC : 0x00000000" after a burst of shell input.
     *
     * rt_interrupt_enter()/rt_interrupt_leave() would maintain the kernel's copy
     * too, but they are C functions and this runs in the EXCM window where no C
     * ABI call is legal. RT_USING_HOOK is off here, so the hooks those functions
     * also call have no subscribers and updating the counter directly is
     * equivalent. XT_NEST_ADD writes through the cached per-core address, for
     * the reason documented at its definition.
     *
     * The OUTERMOST test below deliberately reads the value from before the
     * increment, held in a5: reloading the same word afterwards would always see
     * at least 1 and send every interrupt down the nested path - which skips the
     * FP save, never moves to the ISR stack, and produced a DoubleException on
     * the very first tick after this code was first written.
     */
    XT_CORE_BASE a2, a3
    l32i    a3, a2, XT_CORE_OFS_NEST        /* a3 = nest BEFORE this entry */
    addi    a4, a3, 1
    s32i    a4, a2, XT_CORE_OFS_NEST
    mov     a5, a3                      /* a5 = pre-increment nest, survives */

    XT_NEST_ADD a2, a3, 1

    bnez    a5, .Lrtt_enter_nested

    /* Outermost: FP save block immediately below the exception frame. */
    addi    sp, sp, -XT_RT_FP_BLOCK_SIZE
    rur.fcr a3
    s32i    a3, sp, XT_RT_FP_FCR
    rur.fsr a3
    s32i    a3, sp, XT_RT_FP_FSR
    ssi     f0,  sp, XT_RT_FP_F0 + 0
    ssi     f1,  sp, XT_RT_FP_F0 + 4
    ssi     f2,  sp, XT_RT_FP_F0 + 8
    ssi     f3,  sp, XT_RT_FP_F0 + 12
    ssi     f4,  sp, XT_RT_FP_F0 + 16
    ssi     f5,  sp, XT_RT_FP_F0 + 20
    ssi     f6,  sp, XT_RT_FP_F0 + 24
    ssi     f7,  sp, XT_RT_FP_F0 + 28
    ssi     f8,  sp, XT_RT_FP_F0 + 32
    ssi     f9,  sp, XT_RT_FP_F0 + 36
    ssi     f10, sp, XT_RT_FP_F0 + 40
    ssi     f11, sp, XT_RT_FP_F0 + 44
    ssi     f12, sp, XT_RT_FP_F0 + 48
    ssi     f13, sp, XT_RT_FP_F0 + 52
    ssi     f14, sp, XT_RT_FP_F0 + 56
    ssi     f15, sp, XT_RT_FP_F0 + 60
    movi    a3, XT_RT_FP_FRAME_MARK
    s32i    a3, sp, XT_RT_FP_MARKER_OFS

    XT_CORE_BASE a2, a3
    s32i    sp, a2, XT_CORE_OFS_FRAME

    /* No coprocessor instruction is legal from here until we restore. */
    movi    a3, 0
    wsr     a3, CPENABLE
    rsync

    /* Continue the C dispatch on this core's dedicated ISR stack. The
     * XtExcFrame's spill links stay on the thread's stack, where a window
     * overflow handler will look for them.
     *
     * The top is COMPUTED, not loaded. It used to be `l32i sp, a2,
     * XT_CORE_OFS_ISR_TOP`, i.e. it depended on rt_xt_core[] being intact - and
     * GDB showed that array being clobbered by the very interrupt path that
     * reads it, after which sp was garbage (0x3fc80020, below _data_start) and
     * the next C call died with LoadStoreAlignment inside rt_cpu_self().
     *
     * IDF computes it the same way (portasm.S:157-160: movi the base, mul the
     * size by the core id, add). Doing that here means the one load that had to
     * be correct before anything else is correct is no longer a load at all.
     *
     * a2 holds &rt_xt_core[cpu] (base + cpu*XT_RT_CORE_STRIDE); recover the
     * index from it and use the compile-time base and size. */
    movi    a3, rt_xt_core
    sub     a2, a2, a3                  /* a2 = cpu * XT_RT_CORE_STRIDE */
    srli    a2, a2, XT_RT_CORE_SHIFT    /* a2 = cpu id */
    movi    sp, rt_xt_isr_stack + XT_RT_ISR_STACK_SIZE
    movi    a3, XT_RT_ISR_STACK_SIZE
    mull    a3, a2, a3                  /* a3 = cpu * STACK_SIZE */
    add     sp, sp, a3                  /* sp = top of THIS core's ISR stack */

.Lrtt_enter_nested:
    mov     a0, a12
    ret

/*
-------------------------------------------------------------------------------
  void _rtt_int_exit(void)          -- XT_RTOS_INT_EXIT

  Called with a C environment (PS.INTLEVEL = 1) after the IDF dispatcher has
  run every handler for this level. Never returns to its caller: either it
  resumes the interrupted thread, or it resumes a different one.

  Nested: sp already points at the nested frame on the ISR stack; just restore.
  Outermost: the core's `frame` slot holds the FP block of the preempted thread.
  If a switch was requested (XT_CORE_OFS_SW_FLAG, set through
  rt_hw_context_switch_interrupt), store that sp into *from and load *to.
-------------------------------------------------------------------------------
*/
    .align  4
    .global _rtt_int_exit
    .type   _rtt_int_exit, @function
_rtt_int_exit:
    /*
     * Mask every maskable interrupt until the final rfe.
     *
     * This sequence manipulates per-CORE state - the nest counter, the cached
     * frame pointer, and this core's ISR stack pointer - and it does so across a
     * call into C (rt_xt_irq_switch), where interrupts are enabled. Between the
     * nest decrement below and the FP restore at .Lrtt_exit_restore the counter
     * reads 0, so an interrupt arriving in that window takes the OUTERMOST path,
     * overwrites XT_CORE_OFS_FRAME with its own FP block, restores, and returns.
     * The interrupted exit then reads FRAME and gets the *nested* interrupt's FP
     * block - 80 bytes away from its own.
     *
     * Measured symptom of exactly that misalignment:
     *
     *   PC = 0x46505530   (= XT_RT_FP_FRAME_MARK, "FPU0")
     *   A0 = xPortSysTickHandler
     *
     * i.e. the exit did `l32i a0, sp, XT_STK_EXIT` while sp still pointed at an
     * FP block, so it read the marker and jumped to it.
     *
     * The PS that rfe installs comes from the exception frame, so masking here
     * does not change the interrupted thread's interrupt state.
     */
    rsil    a2, XCHAL_EXCM_LEVEL
    XT_CORE_BASE a2, a3
    l32i    a3, a2, XT_CORE_OFS_NEST
    addi    a3, a3, -1
    s32i    a3, a2, XT_CORE_OFS_NEST

    /* Decrement the KERNEL counter for the same reason as the increment in
     * _rtt_int_enter: rt_schedule() reads it to choose the switch path. Doing
     * it unconditionally is correct because the increment is unconditional
     * too - every entry that gets here incremented exactly once. */
    XT_NEST_ADD a4, a5, -1

    bnez    a3, .Lrtt_exit_restore

#ifdef RT_USING_SMP
    /*
     * Outermost exit on this core: let the kernel perform the pending switch.
     *
     * Guarded, because rt_scheduler_do_irq_switch() dereferences
     * pcpu->current_thread unconditionally (SCHEDULER_ENTER_CRITICAL(current))
     * and this core can reach the exit with that pointer still NULL - measured:
     *
     *   [SM] iexit cpu0 cur=3fcb90e0 flag=1 null=0
     *   [SM] iexit cpu1 cur=3fcb71b0 flag=1 null=2
     *
     * The "null=2" is why the unguarded call faulted with A0=0 and
     * EXCVADDR=0xffffffe0 (a store through NULL at offset -32): entering the
     * function increments critical_lock_nest through a NULL thread pointer.
     *
     * A trivial call8 at this same spot runs 6005 times without incident, so a
     * windowed call here is legal - only the NULL dereference was ever wrong.
     *
     * Condition: current_thread != NULL. When the flag is 0 the hook returns
     * on its own (scheduler_mp.c:1037-1043), so testing it here would only save
     * a call.
     */
    l32i    a6, a2, XT_CORE_OFS_CUR_PTR
    l32i    a6, a6, 0                   /* a6 = pcpu->current_thread */
    beqz    a6, 2f

    addi    sp, sp, -32
    movi    a2, 0
    call8   rt_xt_irq_switch
    addi    sp, sp, 32
    XT_CORE_BASE a2, a3
2:
#endif

    l32i    a2, a2, XT_CORE_OFS_FRAME       /* the preempted thread's FP block */

    /* Consume the switch request, if this core recorded one. */
    XT_CORE_BASE a3, a4
    l32i    a5, a3, XT_CORE_OFS_SW_FLAG
    beqz    a5, .Lrtt_exit_self

    movi    a5, 0
    s32i    a5, a3, XT_CORE_OFS_SW_FLAG
    l32i    a4, a3, XT_CORE_OFS_SW_FROM
    s32i    a2, a4, 0                   /* *from = this thread's sp */
    l32i    a4, a3, XT_CORE_OFS_SW_TO
    l32i    sp, a4, 0                   /* sp   = to_thread->sp */
    j       _rtt_resume

.Lrtt_exit_self:
    mov     sp, a2
    /* CP0 must be on before the first FP register is touched again. */
    movi    a3, 1
    wsr     a3, CPENABLE
    rsync

    l32i    a3, sp, XT_RT_FP_FCR
    wur.fcr a3
    l32i    a3, sp, XT_RT_FP_FSR
    wur.fsr a3
    lsi     f0,  sp, XT_RT_FP_F0 + 0
    lsi     f1,  sp, XT_RT_FP_F0 + 4
    lsi     f2,  sp, XT_RT_FP_F0 + 8
    lsi     f3,  sp, XT_RT_FP_F0 + 12
    lsi     f4,  sp, XT_RT_FP_F0 + 16
    lsi     f5,  sp, XT_RT_FP_F0 + 20
    lsi     f6,  sp, XT_RT_FP_F0 + 24
    lsi     f7,  sp, XT_RT_FP_F0 + 28
    lsi     f8,  sp, XT_RT_FP_F0 + 32
    lsi     f9,  sp, XT_RT_FP_F0 + 36
    lsi     f10, sp, XT_RT_FP_F0 + 40
    lsi     f11, sp, XT_RT_FP_F0 + 44
    lsi     f12, sp, XT_RT_FP_F0 + 48
    lsi     f13, sp, XT_RT_FP_F0 + 52
    lsi     f14, sp, XT_RT_FP_F0 + 56
    lsi     f15, sp, XT_RT_FP_F0 + 60

    addi    sp, sp, XT_RT_FP_BLOCK_SIZE /* sp = XtExcFrame base */

.Lrtt_exit_restore:
    call0   _xt_context_restore
    /* _xt_user_exit pops the frame, writes PS/EPC_1/A0 and does rfe. */
    l32i    a0, sp, XT_STK_EXIT
    ret

/*
-------------------------------------------------------------------------------
  void _rtt_resume(void)             -- resume the thread whose sp is in sp

  Reached by `j` from _rtt_int_exit, rt_hw_context_switch and
  rt_hw_context_switch_to, always with interrupts masked by PS.INTLEVEL.

  The first word of the blob decides the kind of frame (cpuport.h):
    0                     -> solicited; unwind with retw (like IDF's
                             _frxt_dispatch_sol)
    XT_RT_FP_FRAME_MARK   -> FP block; re-enable CP0, restore FP state, step to
                             the XtExcFrame, _xt_context_restore, then rfe via
                             _xt_user_exit

  The rfe variant is the only way to start a thread that has never run: UM, OWB
  and CALLINC are write-masked in PS while PS.INTLEVEL is raised, and rfe
  installs them from the frame. a2/a3 are scratch throughout;
  _xt_context_restore reloads them from the frame.
-------------------------------------------------------------------------------
*/
    .align  4
    .global _rtt_resume
    .type   _rtt_resume, @function
_rtt_resume:
    /*
     * Publish the switch to the kernel BEFORE restoring anything.
     *
     * rt_sched_unlock_n_resched() (src/scheduler_mp.c:884) calls
     * rt_hw_context_switch() and does NOT clear SCHEDULER_LOCK_FLAG afterwards -
     * it delegates that to the port, via rt_sched_post_ctx_switch(). The RISC-V
     * port does it through rt_cpus_lock_status_restore(). If the port does not,
     * the flag stays 1 forever and the next rt_sched_lock() trips
     * RT_ASSERT(SCHEDULER_LOCK_FLAG(percpu) == 0)
     * (scheduler_mp.c:572) - which is exactly the assertion this port started
     * hitting once the atomics recursion was fixed.
     *
     * rt_sched_post_ctx_switch() releases the outgoing thread's scheduler lock,
     * zeroes its critical_lock_nest and installs pcpu->current_thread. It must run
     * with interrupts masked, which is guaranteed here: every caller reaches
     * _rtt_resume from a `j` inside a PS.INTLEVEL-masked region.
     *
     * The thread pointer is taken from this core's next_thread slot, cached by
     * rt_xt_cpu_init(), because the argument registers still belong to whichever
     * frame kind is about to be restored.
     *
     * sp is already the incoming thread's stack, so a 32-byte frame here is
     * safe, and `call8` needs one: the callers use `call0`, which does not
     * rotate a window, and a windowed-ABI callee cannot be entered without it.
     * The frame is released before the `ret`.
     */
#ifdef RT_USING_SMP
    /*
     * Publish through rt_xt_stat_post_switch(to_thread).
     *
     * THE ARGUMENT GOES IN a10, NOT a2. call8 rotates the register window by 8,
     * so the callee's a2..a7 are the caller's a10..a15. Writing the pointer to
     * a2 and then calling passes whatever happened to be in a10 - measured
     * consequence: pcpu->current_thread was set to 1 (the value rt_hw_spin_lock
     * stores in a lock's owner word), and the next rt_critical_level() faulted
     * at offsetof(critical_lock_nest) + 1 = 0x41.
     *
     * The stack slot only keeps a2 alive across the call; it is not the
     * argument.
     */
    XT_CORE_BASE a2, a3
    l32i    a10, a2, XT_CORE_OFS_NEXT_THREAD
    beqz    a10, 1f
    addi    sp, sp, -32
    s32i    a10, sp, 0
    call8   rt_xt_stat_post_switch
    l32i    a10, sp, 0
    addi    sp, sp, 32
1:
#endif
    l32i    a2, sp, 0
    beqz    a2, .Lrtt_resume_solicited


    /* CP0 first: the loads below are coprocessor instructions. */
    movi    a3, 1
    wsr     a3, CPENABLE
    rsync

    l32i    a3, sp, XT_RT_FP_FCR
    wur.fcr a3
    l32i    a3, sp, XT_RT_FP_FSR
    wur.fsr a3
    lsi     f0,  sp, XT_RT_FP_F0 + 0
    lsi     f1,  sp, XT_RT_FP_F0 + 4
    lsi     f2,  sp, XT_RT_FP_F0 + 8
    lsi     f3,  sp, XT_RT_FP_F0 + 12
    lsi     f4,  sp, XT_RT_FP_F0 + 16
    lsi     f5,  sp, XT_RT_FP_F0 + 20
    lsi     f6,  sp, XT_RT_FP_F0 + 24
    lsi     f7,  sp, XT_RT_FP_F0 + 28
    lsi     f8,  sp, XT_RT_FP_F0 + 32
    lsi     f9,  sp, XT_RT_FP_F0 + 36
    lsi     f10, sp, XT_RT_FP_F0 + 40
    lsi     f11, sp, XT_RT_FP_F0 + 44
    lsi     f12, sp, XT_RT_FP_F0 + 48
    lsi     f13, sp, XT_RT_FP_F0 + 52
    lsi     f14, sp, XT_RT_FP_F0 + 56
    lsi     f15, sp, XT_RT_FP_F0 + 60

    addi    sp, sp, XT_RT_FP_BLOCK_SIZE
    call0   _xt_context_restore
    l32i    a0, a1, XT_STK_EXIT
    ret

.Lrtt_resume_solicited:
    /* Same sequence as IDF's _frxt_dispatch_sol: threadptr, PS, PC, then retw.
     * The window rotation in retw restores a0-a3 from the base-save area the
     * spill left above the frame. */
    movi    a4, 1
    wsr     a4, CPENABLE
    rsync
#if XCHAL_HAVE_THREADPTR
    l32i    a2, sp, XT_SOL_THREADPTR
    wur.threadptr a2
#endif
    l32i    a3, sp, XT_SOL_PS
    l32i    a0, sp, XT_SOL_PC
    /* As soon as PS is written interrupts can happen again; no sync needed. */
    wsr     a3, PS
    retw

/*
-------------------------------------------------------------------------------
  rt_hw_context_switch(from, to [, to_thread])

  Cooperative switch. IDF's vPortYield with the FreeRTOS bookkeeping replaced
  by RT-Thread's: leave a solicited frame, publish it through *from, load *to.

  Two ABIs, matching rthw.h:
    UP:  (rt_ubase_t from, rt_ubase_t to)
    SMP: (rt_ubase_t from, rt_ubase_t to, struct rt_thread *to_thread)

  The third argument is the thread being switched to; see
  rt_hw_context_switch_to.

  `from` and `to` are parked in this core's pair slot first, because
  xthal_window_spill_nw walks and rotates the register windows and no argument
  register survives that.

  The spill runs with PS.WOE cleared and PS.INTLEVEL raised to XCHAL_EXCM_LEVEL,
  which is what makes it safe against an interrupt arriving mid-spill.
-------------------------------------------------------------------------------
*/
    .align  4
    .global rt_hw_context_switch
    .type   rt_hw_context_switch, @function
rt_hw_context_switch:
    entry   sp, XT_SOL_FRMSZ

    XT_CORE_BASE a6, a7
    s32i    a2, a6, XT_CORE_OFS_PAIR_FROM
    s32i    a3, a6, XT_CORE_OFS_PAIR_TO
#ifdef RT_USING_SMP
    /* Third argument: the thread this switch is going to. Only SMP passes it. */
    s32i    a4, a6, XT_CORE_OFS_NEXT_THREAD
#endif

    rsr     a2, PS
    s32i    a0, sp, XT_SOL_PC
    s32i    a2, sp, XT_SOL_PS
#if XCHAL_HAVE_THREADPTR
    rur.threadptr a3
    s32i    a3, sp, XT_SOL_THREADPTR
#endif

    /* Clear WOE, set INTLEVEL to EXCM_LEVEL, then spill every live window. */
    movi    a3, ~(PS_WOE_MASK | PS_INTLEVEL_MASK)
    and     a2, a2, a3
    addi    a2, a2, XCHAL_EXCM_LEVEL
    wsr     a2, PS
    rsync
    call0   xthal_window_spill_nw
    l32i    a2, sp, XT_SOL_PS
    wsr     a2, PS

    rsil    a2, XCHAL_EXCM_LEVEL        /* nothing may preempt the swap */

    movi    a3, XT_RT_SOL_YIELDED
    s32i    a3, sp, XT_SOL_EXIT         /* 0 marks a solicited frame */

    XT_CORE_BASE a4, a3
    l32i    a3, a4, XT_CORE_OFS_PAIR_FROM
    s32i    sp, a3, 0                   /* *from = solicited frame base */
    l32i    a3, a4, XT_CORE_OFS_PAIR_TO
    l32i    sp, a3, 0                   /* sp = to_thread->sp */
    j       _rtt_resume

/*
-------------------------------------------------------------------------------
  rt_hw_context_switch_to(to)

  First switch out of rt_system_scheduler_start(), and the entry CPU1 uses on
  its way to its own idle thread.

  Two ABIs depending on RT_USING_SMP, matching rthw.h:
    UP: (rt_ubase_t to)
    SMP: (rt_ubase_t to, struct rt_thread *to_thread)

  The extra `to_thread` is not decoration: the SMP scheduler does not consider a
  thread current until the port says so. XT_SET_CURRENT publishes it inline, with
  a3 still live - no call, so no frame discipline to get wrong.
-------------------------------------------------------------------------------
*/
    .align  4
    .global rt_hw_context_switch_to
    .type   rt_hw_context_switch_to, @function
rt_hw_context_switch_to:
    entry   sp, 32
    rsil    a5, XCHAL_EXCM_LEVEL
#ifdef RT_USING_SMP
    /* Second argument: the thread being started. CPU1's first switch, and the
     * first switch out of rt_system_scheduler_start(). Publishing it here, with
     * a3 still live, is what makes the first tick work. */
    XT_SET_CURRENT a3, a4, a5
#endif
    l32i    sp, a2, 0
    j       _rtt_resume

/*
-------------------------------------------------------------------------------
  rt_hw_context_switch_interrupt(...)

  Records the pending pair for _rtt_int_exit. Two ABIs, matching rthw.h:

    UP:  (from, to, from_thread, to_thread)
    SMP: (context, from, to, to_thread)

  Both are handled in one body by reading the argument registers by name, which
  is what the RISC-V port does too. UP: a2=from a3=to. SMP: a3=from a4=to (a2 is
  the exception frame's sp, which this port does not need because
  `_rtt_int_exit` already knows where the frame is).

  Nested rt_schedule() calls inside one interrupt must not overwrite the
  interrupted thread, so the first from wins and the latest to is kept - the
  same rule the RISC-V port applies.
-------------------------------------------------------------------------------
*/
#ifdef RT_USING_SMP
#define SW_ARG_FROM a3
#define SW_ARG_TO   a4
#define SW_ARG_TCB  a5
#else
#define SW_ARG_FROM a2
#define SW_ARG_TO   a3
#endif

    .align  4
    .global rt_hw_context_switch_interrupt
    .type   rt_hw_context_switch_interrupt, @function
rt_hw_context_switch_interrupt:
    entry   sp, 32
    /*
     * The core base must go into a register that is NOT one of the arguments.
     *
     * SW_ARG_FROM/TO/TCB are a3/a4/a5 in the SMP ABI, so using a5 as the
     * destination for XT_CORE_BASE destroyed the to_thread argument - and then
     * `s32i SW_ARG_TCB, a5, XT_CORE_OFS_NEXT_THREAD` stored the base of
     * rt_xt_core[] into the next_thread slot instead of the incoming TCB.
     *
     * _rtt_resume hands that slot to rt_sched_post_ctx_switch(), which makes it
     * pcpu->current_thread. Measured: rt_thread_self() then returned
     * 0x3fca2a80 (_soft_timer_sem, inside rt_xt_core[]'s neighbourhood) and
     * rt_thread_suspend_to_list() asserted
     * `rt_object_get_type(thread) == RT_Object_Class_Thread`. It also explains
     * the `InstrFetchProhibited PC : 0x0` panics: the first `callx` through the
     * poisoned current_thread.
     *
     * Use a8/a9, which no ABI of this function carries an argument in.
     */
    XT_CORE_BASE a8, a9
#ifdef RT_USING_SMP
    /* Record the target thread before the window is clobbered; _rtt_int_exit
     * publishes it once the switch has actually happened. */
    s32i    SW_ARG_TCB, a8, XT_CORE_OFS_NEXT_THREAD
#endif
    l32i    a7, a8, XT_CORE_OFS_SW_FLAG
    bnez    a7, .Lrtt_swrec_to
    s32i    SW_ARG_FROM, a8, XT_CORE_OFS_SW_FROM
.Lrtt_swrec_to:
    s32i    SW_ARG_TO, a8, XT_CORE_OFS_SW_TO
    movi    a7, 1
    s32i    a7, a8, XT_CORE_OFS_SW_FLAG
    retw

/*
-------------------------------------------------------------------------------
  rt_base_t rt_hw_interrupt_disable(void);  void rt_hw_interrupt_enable(level)

  The token handed from disable to enable is the *whole* PS, not just the
  interrupt level. RSIL returns the old INTLEVEL only, and writing that back
  with WSR.PS would install PS = 0..6, clearing UM/OWB/WOE; with WOE off the
  next window overflow/underflow faults as an illegal instruction. So the
  snapshot is taken first and RSIL is used only for its masking effect.

  XCHAL_EXCM_LEVEL (3 on LX7) masks every interrupt that can reach the level-1
  dispatcher, which is what a kernel critical section needs; levels 4/5 belong
  to IDF's high-priority handlers and are left alone on purpose.
-------------------------------------------------------------------------------
*/
    .align  4
    .global rt_hw_interrupt_disable
    .type   rt_hw_interrupt_disable, @function
rt_hw_interrupt_disable:
    /* 32, not 16: the windowed ABI's minimum frame. These are call8 targets
     * from C, so the caller's 16-byte incoming argument area must not overlap
     * this frame's spill save area. */
    entry   sp, 32
    rsr     a3, PS
    rsil    a2, XCHAL_EXCM_LEVEL
    mov     a2, a3
    retw

    .align  4
    .global rt_hw_interrupt_enable
    .type   rt_hw_interrupt_enable, @function
rt_hw_interrupt_enable:
    entry   sp, 32
    wsr     a2, PS
    rsync
    retw

/*
-------------------------------------------------------------------------------
  Under RT_USING_SMP the kernel asks for the *local* pair by name:
  rthw.h:139-153 defines rt_hw_interrupt_disable/enable as rt_cpus_lock/unlock
  (which take the global scheduler lock) and expects the CPU port to provide
  rt_hw_local_irq_disable/enable for "mask interrupts on this core only".

  They are the same two instructions as above: PS is a per-core special
  register, so raising this core's INTLEVEL cannot affect the other one. The
  aliases exist to satisfy the name, not to add behaviour - and they are
  aliases, not wrappers, so the disable/enable pair cannot drift apart.

  Without them the SMP link fails with 11+ undefined references from
  cpu_mp.c, scheduler_mp.c, irq.c and the port's own secondary entry.
-------------------------------------------------------------------------------
*/
#ifdef RT_USING_SMP
    .align  4
    .global rt_hw_local_irq_disable
    .type   rt_hw_local_irq_disable, @function
rt_hw_local_irq_disable:
    j       rt_hw_interrupt_disable

    .align  4
    .global rt_hw_local_irq_enable
    .type   rt_hw_local_irq_enable, @function
rt_hw_local_irq_enable:
    j       rt_hw_interrupt_enable
#endif /* RT_USING_SMP */

/*
-------------------------------------------------------------------------------
  Coprocessor callouts (CP0 only; CP3 is never enabled).

  ESP-IDF's xtensa_vectors.S asks the RTOS two questions when a
  CoprocessorDisabled exception fires:

    XT_RTOS_CP_STATE       "whose save area should own the coprocessor now?"
    XT_RTOS_CP_EXC_HOOK    "notify the RTOS first"

  This port saves FP state eagerly in the exception frame, so no thread owns a
  lazy save area and the correct answer to both is "nothing to do". The hooks
  must still exist: the vector calls them with call0, so only the registers the
  comments name may be touched. XT_RTOS_CP_STATE must leave 0 in a15, which the
  vector turns into a panic - the right outcome, because no per-thread save
  area exists to hand it.
-------------------------------------------------------------------------------
*/
#if XCHAL_CP_NUM > 0

    .align  4
    .global _rtt_coproc_state
    .type   _rtt_coproc_state, @function
_rtt_coproc_state:
    movi    a15, 0
    ret

    .align  4
    .global _rtt_coproc_exc_hook
    .type   _rtt_coproc_exc_hook, @function
_rtt_coproc_exc_hook:
    ret

#endif /* XCHAL_CP_NUM > 0 */
