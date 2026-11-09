/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 (Xtensa LX7) port
 *
 * ESP32-S3 (Xtensa LX7) port constants shared by the assembly and the C glue.
 *
 * This port does NOT own the interrupt/exception vector table. It reuses
 * ESP-IDF's `components/xtensa/xtensa_vectors.S`, which is OS agnostic and
 * calls out to the RTOS through the XT_RTOS_* macros in xtensa_rtos.h. The
 * RT-Thread definitions of those macros point at the `_rtt_*` routines in
 * context_gcc.S, so window overflow/underflow, the level 2..6 vectors, the
 * debug/NMI vectors, `_xt_panic`, the load/store error handlers and the
 * coprocessor exception path all come from Espressif unmodified.
 *
 * Frame discipline
 * ----------------
 * Two frame kinds can be sitting at `thread->sp`, and the first word decides
 * which one it is:
 *
 *   XtSolFrame      first word = 0 (XT_RT_SOL_YIELDED)
 *                   Written by rt_hw_context_switch() when a thread gives the
 *                   CPU away from C code. Resumed with `retw`: the thread owns
 *                   a live register-window chain that the switch spilled, and
 *                   the refill walks that chain.
 *
 *   FP block        first word = XT_RT_FP_FRAME_MARK
 *                   An FPU save area that `_rtt_int_enter` puts below the
 *                   XtExcFrame, so `thread->sp` points at the block and the
 *                   exception frame sits XT_RT_FP_BLOCK_SIZE bytes above it.
 *                   Used for a thread an interrupt preempted, and for a thread
 *                   that has never run (rt_hw_stack_init builds one). Resumed
 *                   through `_xt_context_restore` + `_xt_user_exit`, i.e. `rfe`,
 *                   which is the only way to install UM/OWB/CALLINC on a
 *                   context that has no window chain yet.
 *
 * Why the FP block exists at all: ESP32-S3 has a single-precision FPU as
 * coprocessor 0 (XCHAL_CP_NUM == 2, XCHAL_CP0_SA_SIZE == 72), and f0-f15 are
 * *caller* saved in the windowed ABI. A cooperative switch therefore needs no
 * FP save (GCC has already spilled whatever was live), but an interrupt does:
 * the interrupted instruction stream may hold live values in f0-f15 that no
 * frame slot covers. Saving them eagerly on every level-1 entry costs 16
 * `ssi`/`lsi` and two `rur`/`wur` pairs, and it removes the need for the lazy
 * coprocessor ownership machinery entirely - CPENABLE keeps CP0 enabled for
 * thread code (see rt_hw_board_init), so no CoprocessorDisabled exception is
 * ever raised for the FPU.
 *
 * AI/SIMD (CP3) is left disabled. An `ee.*` instruction raises
 * CoprocessorDisabled, `_xt_coproc_exc` asks XT_RTOS_CP_STATE for a save area,
 * this port answers "none" (a15 = 0) and the vector reports a panic. That is
 * deliberate: there is no per-thread save area to give it.
 */

#ifndef __CPU_PORT_H__
#define __CPU_PORT_H__

/*
 * This header is reached from rthw.h (RT_HW_INCLUDE_CPUPORT) and from
 * rttypes.h, i.e. from translation units that know nothing about Xtensa. It
 * uses XT_STK_FRMSZ and the XCHAL_* configuration macros, so it has to bring
 * in the Xtensa definitions itself instead of assuming the includer did.
 *
 * xtensa_context.h handles __ASSEMBLER__ internally (coreasm.h when assembling,
 * config/core.h when compiling C), which is why the same include serves
 * context_gcc.S and cpuport.c.
 */
#include <xtensa_context.h>

#ifndef XT_RT_FP_BLOCK_SIZE
#define XT_RT_FP_BLOCK_SIZE 80
#endif

/* What rt_hw_context_switch() writes into a yielding thread's frame at
 * XT_SOL_EXIT, to mark it as the solicited kind. */
#define XT_RT_SOL_YIELDED 0

/* The solicited frame is exactly IDF's XtSolFrame (XT_SOL_FRMSZ == 32) and is
 * used unmodified: 16 bytes of header and the 16-byte base-save area that the
 * window spill writes at sp+0x10..0x1F. That area is therefore NOT free space,
 * which is why a cooperative switch does not save FCR/FSR.
 *
 * Consequence, stated plainly: the FP rounding mode and the FP exception flags
 * are process-wide on this port, not per-thread. f0-f15 need no save (they are
 * caller-saved, so GCC has already spilled whatever was live before calling
 * into the kernel); FCR/FSR do, and the interrupt path saves them because that
 * is where they can actually be live. A thread that changes the rounding mode
 * and then blocks will hand that mode to the next thread - the same trade-off
 * IDF's own FreeRTOS port makes for the FPU control registers unless the lazy
 * coprocessor path is enabled. */

/* FP save block, below every XtExcFrame that an interrupt built.
 * 4 (marker) + 4 (fcr) + 4 (fsr) + 16 * 4 (f0-f15) = 76, padded to 80 so both
 * thread->sp and the frame above it stay 16-byte aligned. */
#define XT_RT_FP_MARKER_OFS 0
#define XT_RT_FP_FCR        4
#define XT_RT_FP_FSR        8
#define XT_RT_FP_F0         12
#define XT_RT_FP_FRAME_MARK 0x46505530  /* "FPU0" */

/* Interrupt stack for C-level ISR code, and the word that remembers the
 * outermost exception frame while C runs elsewhere. A windowed-ABI function
 * spills up to 64 bytes below its own sp, and an XtExcFrame is only
 * XT_STK_FRMSZ (192) bytes with most of that holding saved registers: running
 * C ISR code directly on the frame turns frame slots into spill links. IDF
 * makes the same move in _frxt_int_enter (TCB -> port_IntStack).
 *
 * Single core and everything runs at PS.INTLEVEL = 1, which masks every
 * level-1 source, so one stack and one frame slot are enough. Both live in
 * cpuport.c so nothing has to be added to the linker script. */
/* 16 KB. One nesting level costs a full XtExcFrame (~1.5 KB here) plus the
 * 256-byte window-spill window that _xt_context_save needs above sp, so 4 KB
 * cannot survive a nested interrupt - measured as a DoubleException inside
 * _xt_context_save with sp 0x30 below the top of the 4 KB stack. */
#define XT_RT_ISR_STACK_SIZE 16384

/* ---- SMP: per-core state ------------------------------------------------ */

/*
 * Every piece of port state that used to be a single global is per-core, and
 * every one of them is reached through the same base: `rt_xt_core` indexed by a
 * fixed 64-byte stride. A power of two is not padding - it is what lets
 * context_gcc.S get from PRID to the core's block in two instructions (slli,
 * add), at a dozen sites where no spare register exists for a multiply.
 *
 * UP builds still go through this table (RT_CPUS_NR is 1 and PRID is 0), so
 * there is one code path, not two. The cost is one slli and one add.
 */
#define XT_RT_CORE_SHIFT 6
#define XT_RT_CORE_STRIDE (1u << XT_RT_CORE_SHIFT)

/* Byte offsets the assembly hard-codes. Kept outside the __ASSEMBLER__ guard so
 * one definition serves both sides; cpuport.c asserts every one of them against
 * offsetof(), so a layout change fails the build instead of corrupting state. */
#define XT_CORE_OFS_NEST        0
#define XT_CORE_OFS_FRAME       4
#define XT_CORE_OFS_ISR_TOP     8
#define XT_CORE_OFS_ISR_BASE    12
#define XT_CORE_OFS_SW_FLAG     16
#define XT_CORE_OFS_SW_FROM     20
#define XT_CORE_OFS_SW_TO       24
#define XT_CORE_OFS_IRQ_NEST    28
#define XT_CORE_OFS_PAIR_FROM   32
#define XT_CORE_OFS_PAIR_TO     36
#define XT_CORE_OFS_NEXT_THREAD 40
#define XT_CORE_OFS_CUR_PTR     44

#ifndef __ASSEMBLER__

struct rt_xt_core
{
    /* interrupt nesting for this core, counted by _rtt_int_enter/_exit */
    volatile rt_uint32_t nest;          /* +0  */

    /* FP block of the level-0..n walk whose outermost entry is still on this
     * core's thread stack. Written by _rtt_int_enter, read by _rtt_int_exit. */
    void                *frame;         /* +4  */

    /* top / bottom of this core's C-level ISR stack */
    void                *isr_stack_top; /* +8  */
    void                *isr_stack_base;/* +12 */

    /* pending switch recorded by rt_hw_context_switch_interrupt, consumed by
     * _rtt_int_exit. Per-core: two cores can be in an ISR at the same time and
     * must not overwrite each other's request. */
    volatile rt_uint32_t sw_flag;       /* +16 */
    rt_ubase_t           sw_from;       /* +20 */
    rt_ubase_t           sw_to;         /* +24 */

    /* &rt_cpu_index(cpu)->irq_nest. The kernel reads that in rt_schedule() to
     * decide between the cooperative and the interrupt switch path; it is NOT
     * the same counter as `nest` above, and both have to be maintained. Held as
     * a pointer so assembly never needs to know struct rt_cpu's layout.
     * NULL until rt_xt_cpu_init(), and the assembly tolerates NULL. */
    volatile rt_atomic_t *irq_nest;     /* +28 */

    /* *from / *to park here between the two halves of a cooperative switch:
     * xthal_window_spill_nw rotates the register windows, so no argument
     * register survives it. */
    rt_ubase_t           pair_from;     /* +32 */
    rt_ubase_t           pair_to;       /* +36 */

    /* struct rt_thread * of the thread this core is switching TO, as passed to
     * rt_hw_context_switch[_to] / rt_hw_context_switch_interrupt. The SMP
     * scheduler needs it after the switch, to publish pcpu->current_thread -
     * see _rtt_int_exit, _rtt_self_switch and _rtt_first_switch. Stored here
     * because the assembly cannot call rt_sched_post_ctx_switch() with the
     * thread pointer still in a register once the frame has been swapped. */
    rt_ubase_t           next_thread;   /* +40 */

    /* &rt_cpu_index(cpu)->current_thread, resolved once per core in C.
     *
     * The kernel asks rt_thread_self() / rt_critical_level() / rt_enter_critical()
     * about the running thread, and all three read this one pointer. The port has
     * to publish it on every switch - but doing that by calling C from the switch
     * path is where the dual-core bring-up kept breaking (call0 into a body that
     * needs call8, with the frame discipline wrong in a way that fails silently).
     * With the address cached, publishing is two instructions and no call at
     * all. */
    rt_ubase_t           cur_thread_ptr;/* +44 */

    rt_ubase_t           pad[XT_RT_CORE_STRIDE / sizeof(rt_ubase_t) - 12];
};

#endif /* !__ASSEMBLER__ */

/* Everything below is C. The assembler reaches this header for the XT_CORE_OFS_*
 * constants and the frame sizes only, and would choke on a typedef - measured as
 * "unknown opcode or format name 'void'" at this exact line the first time
 * context_gcc.S included rtconfig.h and RT_USING_SMP became visible to it. */
#if defined(RT_USING_SMP) && !defined(__ASSEMBLER__)
/* Per-core bring-up, called once on each core by the BSP. */
void rt_xt_cpu_init(void);

/*
 * Xtensa spinlock: a word holding the owning core's PRID, zero when free.
 *
 * `aligned(16)` is a hardware requirement, not a hint: S32C1I (store
 * conditional) raises AlignmentException when its operand is not 16-byte
 * aligned. Getting it wrong turns every acquisition into an exception, which at
 * least fails loudly instead of silently.
 *
 * The encoding deliberately matches IDF's `spinlock_t`
 * (components/esp_hw_support/include/spinlock.h): same instruction, same free
 * value, same owner value. Two lock flavours that disagree about what "free"
 * means cannot share a word, and `_xt_coproc_owner_sa_lock` in
 * xtensa_vectors.S is exactly such a word - it is taken with IDF's
 * spinlock_take macro from the coprocessor exception path.
 */
typedef struct
{
    volatile rt_uint32_t owner;
} __attribute__((aligned(16))) rt_hw_spinlock_t;

/*
 * Override the kernel's static initializer for struct rt_spinlock.
 *
 * rttypes.h:176 documents this as an extension point ("can be overridden by
 * cpuport.h"), and it needs overriding as soon as RT_USING_DEBUG is on: the
 * kernel default is `{{0}}`, which initializes only the first member, while
 * RT_USING_DEBUG adds `critical_level` after it (rttypes.h:166-168). Every
 * RT_SPINLOCK_INIT in the kernel then trips
 *
 *   src/object.c:97: error: missing initializer for field 'critical_level' of
 *   'struct rt_spinlock' [-Werror=missing-field-initializers]
 *
 * twice per entry in _object_container and once per RT_DEFINE_SPINLOCK. That is
 * 10 errors from one macro, all of them noise: the field is genuinely meant to
 * start at zero.
 *
 * This header is only reached from rttypes.h inside `#ifdef RT_USING_SMP`
 * (rttypes.h:161), so the field order here is the SMP one: the lock, then the
 * debug counter.
 */
#ifdef RT_USING_DEBUG
#undef RT_SPINLOCK_INIT
#define RT_SPINLOCK_INIT { { 0 }, 0 }
#else
#undef RT_SPINLOCK_INIT
#define RT_SPINLOCK_INIT { { 0 } }
#endif

#endif /* RT_USING_SMP && !__ASSEMBLER__ */

/* Headroom a thread stack needs on top of the frame it starts with.
 * The initial context is released the moment the thread first runs - dispatch
 * restores sp above it - so it does not stack with anything. What must fit:
 * one exception frame + FP block for a preemption that lands on top of the
 * initial frame, the initial frame + FP block itself (the first switch reads
 * them before sp moves), and spill space for the windowed save area. The
 * kernel's own threads default to a few hundred bytes, in which case the
 * initial frame lands *below* thread->stack_addr and whatever the heap
 * allocator parked there gets overwritten. cpuport.c turns that into a build
 * error. */
#define XT_RT_STACK_MIN (2 * (XT_STK_FRMSZ + XT_RT_FP_BLOCK_SIZE) + 128)

#endif /* __CPU_PORT_H__ */
