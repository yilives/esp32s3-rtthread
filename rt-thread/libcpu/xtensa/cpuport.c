/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 (Xtensa LX7) port
 *
 * ESP32-S3 (Xtensa LX7) C glue for context_gcc.S.
 *
 * Everything architectural - the vector table, the window overflow/underflow
 * handlers, `_xt_context_save`/`_xt_context_restore`, the interrupt dispatch
 * table, `_xt_panic` and the coprocessor exception path - comes from ESP-IDF's
 * `components/xtensa` unmodified. This file supplies what those need from an
 * RTOS:
 *
 *   rt_xt_core[]            per-core nest / frame / ISR stack / switch record
 *   rt_hw_stack_init()      first context of a thread
 *   rt_hw_interrupt_is_disabled()
 *   rt_hw_spin_lock/unlock  S32C1I, SMP only
 *   rt_hw_cpu_id()          PRID, SMP only
 *
 * and the FPU policy (see cpuport.h).
 */

#include <rtthread.h>
#include <rthw.h>

#include <xtensa/config/core.h>
#include <xtensa/corebits.h>
#include <xtensa_context.h>


#include "cpuport.h"

/* Defined in ESP-IDF's components/xtensa/xtensa_vectors.S. Declared here rather
 * than pulled in through a header because no public header declares it - it is
 * the exit dispatcher the vector stores in XT_STK_EXIT. */
extern void _xt_user_exit(void);

/* Dedicated ISR stack, one per core. Every level-1 entry runs its C dispatch
 * here instead of on the interrupted thread's frame: a windowed-ABI call spills
 * up to 64 bytes below sp, and an XtExcFrame has no room for those links.
 *
 * One per core is required, not optional: both cores can be inside a level-1
 * handler at the same time, each at PS.INTLEVEL = 1, and a shared stack would
 * have them overwrite each other's frames.
 *
 * RT_ALIGN(16) is not usable here: it takes a size and an alignment, and this
 * needs a data-section alignment. _Alignas is the C11 spelling that means what
 * is intended. */
/* Number of schedulers this port has brought up, across all cores. Zero means
 * none yet; ft_task_scheduler_state() and ft_ipc_take() both consult it so a
 * lock taken before the first scheduler starts stays a no-op, and a lock taken
 * afterwards really locks. Deliberately NOT keyed on
 * pcpu->current_thread: on the dual-core bring-up that word stays NULL long
 * after threads are running. */
volatile rt_uint32_t rt_xt_running_count;

/* Not static: context_gcc.S computes this core's stack top from the symbol
 * itself (movi + mul by core id) rather than loading a pointer out of
 * rt_xt_core[], because that array is what a bad interrupt path corrupts
 * first. See the note at the stack switch in _rtt_int_enter. */
rt_uint8_t
rt_xt_isr_stack[RT_CPUS_NR][XT_RT_ISR_STACK_SIZE] __attribute__((aligned(16)));



/* ---- per-core state, shared with context_gcc.S --------------------------- */

/*
 * One block per core, indexed by PRID with a fixed stride so the assembly can
 * reach it with a shift and an add; see XT_CORE_BASE in context_gcc.S.
 *
 * The ISR stack top is a STATIC initializer, not something an init call fills
 * in. That is not tidiness: `_rtt_int_enter` moves it straight into sp, so a
 * zero there is an immediate crash, and the first interrupt can arrive before
 * any C init function of this port has run. Measured - an init-only version
 * produced "Core 0 panic'ed (Double exception)" right after the RT-Thread
 * banner, with A1 = 0x60100000 (a plausible stack pointer stored into sp and
 * then dereferenced).
 *
 * `address of a static array element` is a constant expression, so this costs
 * nothing at run time.
 */
#if !defined(RT_USING_SMP)
/* src/irq.c defines `volatile rt_atomic_t rt_interrupt_nest = 0;` with external
 * linkage. That file also has a macro of the same name (mapping it to
 * rt_cpu_self()->irq_nest under SMP), but a macro is translation-unit local, so
 * from here the name is an ordinary symbol and its address is a link-time
 * constant - which is what lets the initializer below be static. */
extern volatile rt_atomic_t rt_interrupt_nest;
#endif

struct rt_xt_core rt_xt_core[RT_CPUS_NR] =
{
    {
        .isr_stack_base = rt_xt_isr_stack[0],
        .isr_stack_top  = rt_xt_isr_stack[0] + XT_RT_ISR_STACK_SIZE,
#if !defined(RT_USING_SMP)
        /* UP: rt_interrupt_nest is an ordinary global, so its address is a
         * link-time constant and the vector can update the kernel's counter
         * from the very first tick. Under SMP it is a macro over
         * rt_cpu_self() and must be resolved at run time - see
         * rt_xt_cpu_init(). */
        .irq_nest       = (volatile rt_atomic_t *)&rt_interrupt_nest,
#endif
    },
#if RT_CPUS_NR > 1
    {
        .isr_stack_base = rt_xt_isr_stack[1],
        .isr_stack_top  = rt_xt_isr_stack[1] + XT_RT_ISR_STACK_SIZE,
    },
#endif
};

#define XT_ASSERT_OFS(field, macro)                                           \
    _Static_assert(offsetof(struct rt_xt_core, field) == (macro),             \
                   "context_gcc.S hard-codes " #macro " for " #field)

XT_ASSERT_OFS(nest,           XT_CORE_OFS_NEST);
XT_ASSERT_OFS(frame,          XT_CORE_OFS_FRAME);
XT_ASSERT_OFS(isr_stack_top,  XT_CORE_OFS_ISR_TOP);
XT_ASSERT_OFS(isr_stack_base, XT_CORE_OFS_ISR_BASE);
XT_ASSERT_OFS(sw_flag,        XT_CORE_OFS_SW_FLAG);
XT_ASSERT_OFS(sw_from,        XT_CORE_OFS_SW_FROM);
XT_ASSERT_OFS(sw_to,          XT_CORE_OFS_SW_TO);
XT_ASSERT_OFS(irq_nest,       XT_CORE_OFS_IRQ_NEST);
XT_ASSERT_OFS(pair_from,      XT_CORE_OFS_PAIR_FROM);
XT_ASSERT_OFS(pair_to,        XT_CORE_OFS_PAIR_TO);
XT_ASSERT_OFS(next_thread,    XT_CORE_OFS_NEXT_THREAD);
XT_ASSERT_OFS(cur_thread_ptr, XT_CORE_OFS_CUR_PTR);

_Static_assert(sizeof(struct rt_xt_core) == XT_RT_CORE_STRIDE,
               "struct rt_xt_core must be exactly one stride; XT_CORE_BASE "
               "in context_gcc.S assumes it");

/* The pending-switch globals the UP scheduler still declares extern. Under SMP
 * the scheduler keeps its own per-cpu copies and does not reference these. */
#ifndef RT_USING_SMP
volatile rt_ubase_t  rt_interrupt_from_thread;
volatile rt_ubase_t  rt_interrupt_to_thread;
volatile rt_uint32_t rt_thread_switch_interrupt_flag;
#endif

/* ---- SMP: per-core and cross-core plumbing ------------------------------- */

#if defined(RT_USING_SMP)

/*
 * Resolve this core's cached `irq_nest` pointer, once, from C, on the core that
 * will use it.
 *
 * Only `irq_nest` needs this. Under RT_USING_SMP the kernel defines
 * `rt_interrupt_nest` as `rt_cpu_self()->irq_nest` (src/irq.c:68) - a function
 * call plus a struct offset, neither available in the EXCM window
 * `_rtt_int_enter` runs in - so the address is resolved here and the assembly
 * writes through it. The ISR stack pointers deliberately do NOT need this: they
 * are static initializers, because the first interrupt can arrive before this
 * runs.
 *
 * Called from rt_hw_board_init() on CPU0 and from the secondary entry on CPU1,
 * i.e. once per core, on that core.
 */
void rt_xt_cpu_init(void)
{
    int cpu_id = rt_hw_cpu_id();

    if (cpu_id < 0 || cpu_id >= RT_CPUS_NR)
    {
        return;
    }

    rt_xt_core[cpu_id].nest    = 0;
    rt_xt_core[cpu_id].frame   = RT_NULL;
    rt_xt_core[cpu_id].sw_flag = 0;

    /* Loaded and stored as a plain word by the assembly, so take the address of
     * the underlying object rather than of a copy. rt_cpu_index() is the
     * kernel's accessor for exactly this. */
    rt_xt_core[cpu_id].irq_nest =
        (volatile rt_atomic_t *)&(rt_cpu_index(cpu_id)->irq_nest);

    /* Same trick for the running-thread pointer: resolving it here means the
     * switch path is two stores instead of a C call. */
    rt_xt_core[cpu_id].cur_thread_ptr =
        (rt_ubase_t)&(rt_cpu_index(cpu_id)->current_thread);

    /*
     * Tell the shim that a scheduler exists on this core.
     *
     * rt_xt_cpu_init() runs from rt_hw_board_init() on CPU0 and from the
     * secondary entry on CPU1, i.e. immediately before each core's
     * rt_system_scheduler_start(). Bumping the counter here - rather than
     * waiting for the first published switch - closes the window in which
     * ft_task_scheduler_state() reported NOT_STARTED and esp_libc therefore
     * turned every libc lock into a no-op.
     *
     * Measured with only the post-switch bump in place: 16 mutex takes on the
     * boot path, 15 of them still saw "not started" - so the window is real and
     * wide enough to matter.
     */
    if (rt_xt_running_count < 0xFFFFFFFFu)
    {
        rt_xt_running_count++;
    }
}
/*
 * Publish the thread this core is now running.
 *
 * Called from `_rtt_post_switch` (context_gcc.S) on every switch path, with
 * interrupts masked and sp already moved to the new thread.
 *
 * Deliberately NOT rt_sched_post_ctx_switch():
 *
 *   That function also releases the outgoing thread's scheduler lock and zeroes
 *   its critical_lock_nest, and it asserts `critical_lock_nest == 1` while doing
 *   so. On this port the lock is taken and released by rt_system_scheduler_start()
 *   and by SCHEDULER_CONTEXT_UNLOCK() in the normal switch path, so calling it a
 *   second time from the port's own switch ends up releasing a lock nobody holds
 *   and tripping the assert in the middle of the first context switch. Measured:
 *   the boot reached "[boot] starting scheduler" and then stopped - no panic, no
 *   reset, no shell, and it did so with CPU1 parked as well, which is what proves
 *   the fault is in CPU0's first switch and not in the second core.
 *
 * The one thing this port actually needs from it is `pcpu->current_thread`.
 * Everything the kernel asks about the running thread - rt_thread_self(),
 * rt_critical_level(), rt_enter_critical(), the rt_thread_close() self-close
 * assert - reads that pointer, and it was never being updated.
 */
void rt_xt_set_current_thread(void *thread)
{
    rt_cpu_self()->current_thread = (struct rt_thread *)thread;
}

/*
 * Xtensa atomics: S32C1I is the only read-modify-write the ISA offers, and it
 * is a *store* conditional - it writes the new value only if the word at the
 * address still holds SCOMPARE1.
 *
 * The sequence below is the one IDF uses (xt_asm_utils.h spinlock_take and
 * components/esp_hw_support/include/spinlock.h), reproduced here so this port
 * does not have to include a FreeRTOS header:
 *
 *   wsr  SCOMPARE1, 0        the value that means "free"
 *   rsync                    SCOMPARE1 must be committed before s32c1i reads it
 *   rsr  tmp, PRID           who we are
 *   s32c1i tmp, ptr, 0       if (*ptr == 0) *ptr = tmp, tmp = 0
 *                          else             tmp = *ptr
 *
 * `rsync` after the wsr is not decorative: without it the s32c1i can observe
 * the *old* SCOMPARE1, which turns the compare against the wrong value and
 * makes the lock either free-for-all or permanently stuck. IDF's macro has the
 * same rsync with the same comment.
 */
void rt_hw_spin_lock_init(rt_hw_spinlock_t *lock)
{
    lock->owner = 0;
}

void rt_hw_spin_lock(rt_hw_spinlock_t *lock)
{
    rt_uint32_t tmp;

    do
    {
        __asm__ volatile (
            "movi    %0, 0          \n"
            "wsr     %0, SCOMPARE1  \n"
            "rsync                  \n"
            "rsr     %0, PRID       \n"
            "extui   %0, %0, 13, 1  \n"
            "addi    %0, %0, 1      \n"
            "s32c1i  %0, %1, 0      \n"
            : "=&r"(tmp)
            : "r"(lock)
            : "memory");
    }
    while (tmp != 0);
}

void rt_hw_spin_unlock(rt_hw_spinlock_t *lock)
{
    /* Only the owner writes here, and a plain store is enough: the S32C1I in
     * the acquirer is the serialisation point, and Xtensa's memory model makes
     * an aligned 32-bit store visible to the other core's compare. */
    __asm__ volatile ("" ::: "memory");
    lock->owner = 0;
    __asm__ volatile ("" ::: "memory");
}

/*
 * Which core is this?
 *
 * PRID is NOT a small integer. On ESP32-S3 the whole register is 0x6000 on the
 * PRO core and 0x0000 on the APP core: bit 13 is the core number and the rest
 * is a "this is a processor identification register" marker. Returning the raw
 * value gives 24576, and rt_cpu_self() then computes &_cpus[24576] - a wild
 * address that faults on the first field access:
 *
 *   Guru Meditation Error: Core 0 panic'ed (LoadStoreError)
 *   PC       : 0x4038b760   rt_sched_lock (scheduler_mp.c:571)
 *   EXCVADDR : 0x40ab8c7c
 *   0x40ab8c78 == &_cpus + 24576 * sizeof(struct rt_cpu)
 *
 * IDF extracts the same bit with `extui %0, %0, 13, 1`
 * (components/xtensa/include/xt_utils.h:37-40) and says so: "Read and extract
 * bit 13 of special register PRID".
 *
 * `extui` is a plain instruction, not a coprocessor one, so this is legal on
 * every path that calls it - including the EXCM window, where the port's
 * per-core state is addressed.
 */
int rt_hw_cpu_id(void)
{
    rt_uint32_t id;

    __asm__ volatile ("rsr.prid %0 \n"
                      "extui    %0, %0, 13, 1 \n"
                      : "=r"(id));

    return (int)id;
}

#endif /* RT_USING_SMP */

/* ---- interrupt state ----------------------------------------------------- */

rt_bool_t rt_hw_interrupt_is_disabled(void)
{
    rt_uint32_t ps;

    __asm__ volatile ("rsr %0, ps" : "=r"(ps));

    /* CINTLEVEL = max(PS.INTLEVEL, PS.EXCM ? XCHAL_EXCM_LEVEL : 0), so either
     * field being raised means a kernel critical section is in effect. */
    return ((ps & 0xf) != 0) || ((ps & PS_EXCM) != 0);
}

/* ---- FPU enable ---------------------------------------------------------- */

/*
 * CPENABLE is set here rather than left to the reset value, because ESP-IDF's
 * startup and the ROM may both have changed it. Only CP0 (the single-precision
 * FPU) is enabled; CP3 (AI/SIMD) stays off, so an `ee.*` instruction traps
 * instead of silently corrupting a register file this port does not save.
 *
 * "Enabled" is not the same as "in use": a coprocessor bit set in CPENABLE only
 * means the thread may execute that coprocessor's instructions without taking
 * CoprocessorDisabled. _rtt_int_enter clears it for the duration of the C ISR
 * code, which is the Xtensa rule that coprocessors are thread-only.
 */
void rt_hw_xtensa_enable_fpu(void)
{
    __asm__ volatile ("movi a2, 1\n wsr a2, cpenable\n rsync" ::: "a2", "memory");
}

/* ---- bring-up counters ---------------------------------------------------- */

/*
 * Counted from _rtt_resume, per core. Exposed so the shell can report them
 * without a debugger: the question they answer is "did this core ever go
 * through rt_sched_post_ctx_switch()", which is the only place that clears the
 * outgoing thread's critical_lock_nest. A core whose counter stays 0 keeps
 * critical_lock_nest == 1 forever, and every blocking call from its threads is
 * then refused by the FreeRTOS shim's ft_block_allowed().
 */
volatile rt_uint32_t rt_xt_stat_switch[RT_CPUS_NR];

/* rt_system_scheduler_start() rendezvous: [cpu] = 1 once that core has arrived. */
volatile rt_uint32_t rt_xt_start_ready[RT_CPUS_NR];
/* How often the switch path handed us something that is not a TCB, and the last
 * such value. Non-zero means the next_thread slot was corrupted. */
volatile rt_uint32_t rt_xt_bad_publish[RT_CPUS_NR];
volatile rt_ubase_t  rt_xt_bad_value[RT_CPUS_NR];




/* Compile-time report of the two frame constants the resume path assumes. */
typedef char rt_xt_assert_framsz_equals_192[(XT_STK_FRMSZ == 192) ? 1 : -1];
/* Offset of sched_thread_ctx.critical_lock_nest inside struct rt_thread: the
 * fault seen when a NULL current_thread reaches SCHEDULER_ENTER_CRITICAL is at
 * exactly this offset. */

typedef char rt_xt_assert_fpblock_equals_80[(XT_RT_FP_BLOCK_SIZE == 80) ? 1 : -1];







/*
 * Interrupt-exit scheduling, called from _rtt_int_exit (SMP only).
 *
 * rt_scheduler_do_irq_switch() dereferences pcpu->current_thread immediately,
 * so a core that has not published a thread yet must not call it. Measured:
 * CPU1 reached this point with current_thread still NULL and the unguarded call
 * faulted with A0 = 0 and EXCVADDR = 0xffffffe0.
 *
 * The nest is NOT touched here. An earlier version cleared a lingering 1 to get
 * past the hook's "already locked" test; that made switches happen but broke the
 * critical section's accounting (the resumed thread then decremented past zero
 * and the mutex lists came apart). The real cause of the stuck nest was a broken
 * S32C1I in atomic_xtensa.c, fixed there.
 */
void rt_xt_irq_switch(void)
{
    struct rt_cpu *pcpu = rt_cpu_self();

    if (pcpu->current_thread != RT_NULL)
    {
        rt_scheduler_do_irq_switch(RT_NULL);
    }
}


/*
 * Called from _rtt_resume with the incoming thread; just counts and delegates,
 * so the counters cost one extra call and change no behaviour.
 */
/*
 * Reject anything that cannot be a thread control block before handing it to
 * the kernel.
 *
 * The value comes from this core's cached `next_thread` slot, which the switch
 * path fills from the caller's argument register. That slot has been observed to
 * hold 1 - i.e. a core id, the same value rt_hw_spin_lock() stores into a
 * lock's owner word - and publishing it makes pcpu->current_thread = 1. The
 * next rt_critical_level() then does `l16ui a2, 1, 64` and faults at 0x41,
 * which is exactly the LoadProhibited this port kept hitting at
 * ft_block_allowed -> rt_critical_level.
 *
 * A real TCB is always in DRAM (0x3FC...) or, for a static thread, in
 * internal RAM - never in the low megabytes. Checking that one property is
 * enough to stop the corruption, and refusing to publish leaves the previous,
 * valid pointer in place instead of replacing it with garbage.
 */
static rt_bool_t rt_xt_tcb_looks_valid(const void *thread)
{
    rt_ubase_t p = (rt_ubase_t)thread;

    if (p < 0x3F000000u || p >= 0x60000000u)
    {
        return RT_FALSE;
    }

    return ((p & 0x3) == 0) ? RT_TRUE : RT_FALSE;
}

void rt_xt_stat_post_switch(void *thread)
{
    int cpu = rt_hw_cpu_id();

    if (cpu >= 0 && cpu < RT_CPUS_NR)
    {
        rt_xt_stat_switch[cpu]++;
    }

    /* A thread is now running on some core. ft_task_scheduler_state() uses this
     * instead of pcpu->current_thread, which can still be NULL during bring-up -
     * see the long note there. Bumped before the validity check on purpose: even
     * a rejected publish means scheduling has begun. */
    if (rt_xt_running_count < 0xFFFFFFFFu)
    {
        rt_xt_running_count++;
    }

    if (!rt_xt_tcb_looks_valid(thread))
    {
        struct rt_cpu *pcpu = rt_cpu_self();

        if (cpu >= 0 && cpu < RT_CPUS_NR)
        {
            rt_xt_bad_publish[cpu]++;
            rt_xt_bad_value[cpu] = (rt_ubase_t)thread;
        }

        /*
         * Keep whatever this core was already running instead of the garbage,
         * but STILL run the hook.
         *
         * Returning early here was a deadlock: rt_sched_post_ctx_switch() is
         * what releases the outgoing thread's scheduler lock and zeroes its
         * critical_lock_nest. Skipping it leaves SCHEDULER_LOCK_FLAG set on this
         * CPU for good, the next rt_sched_lock() spins on _mp_scheduler_lock
         * forever, and ft_task_start_pending() never returns - measured as
         * "banner, then nothing" with no panic at all.
         *
         * Pass the thread this core already has: then from_thread == thread,
         * the lock is released exactly once, and current_thread stays valid.
         */
        rt_sched_post_ctx_switch(pcpu->current_thread);
        return;
    }

    rt_sched_post_ctx_switch((struct rt_thread *)thread);
}

/* ---- thread startup frame ------------------------------------------------ */

/*
 * Trampoline every thread starts in.
 *
 * The windowed ABI hands the first context to its code with PS.CALLINC=1, i.e.
 * as if a `call4` had just returned control. With CALLINC=1 the argument
 * registers of that call are the frame's a6..a8 (see the frame builder below
 * and ESP-IDF's uxInitialiseStackFrame, which this mirrors). A `retw` from the
 * entry function would have no caller window to unwind to, so the frame's pc
 * points here instead: this function calls the real entry and then the thread
 * exit handler.
 */
static void __attribute__((noinline, noreturn))
rt_xt_thread_start(void (*entry)(void *), void *parameter, void (*texit)(void))
{
    entry(parameter);
    texit();

    for (;;)
    {
        /* texit is _thread_exit, which switches away and never returns. */
    }
}

/*
 * Build the first context of a thread.
 *
 * Layout, from thread->sp upward:
 *
 *   FP block (XT_RT_FP_BLOCK_SIZE)   marker, FCR, FSR, f0..f15 - all zeroed
 *   XtExcFrame (XT_STK_FRMSZ)        exit, pc, ps, a6/a7/a8, a1, + extra save
 *   TLS area (16-byte aligned)       the thread's thread-local storage
 *
 * and thread->sp points at the FP block, exactly like a thread that an
 * interrupt preempted. That is deliberate: the resume path is then the same
 * for both, and `rfe` is the only way to install UM/CALLINC/OWB on a context
 * that has never had a window chain.
 *
 * It cannot be a solicited frame: resuming one is a `retw` whose register
 * refill walks the chain that the yielding thread spilled. A thread that has
 * never run has no such chain.
 *
 * THE TLS AREA IS NOT OPTIONAL. This is the one piece of per-thread state that
 * is easy to forget and impossible to notice until something touches errno:
 *
 *   - Xtensa keeps THREADPTR (the TLS base) in the "extra" saved state, which
 *     XtExcFrame already has room for at offset XT_STK_EXTRA, and which
 *     _xt_context_restore reloads through xthal_restore_extra_nw.
 *   - This build uses picolibc (CONFIG_LIBC_PICOLIBC=y), whose errno is a TLS
 *     variable: `#define set_errno(err) do { if (err) { errno = (err); } }
 *     while(0)` in lwip's sockets_priv.h. So every lwIP error path writes
 *     through THREADPTR.
 *   - Zeroing the frame - which is otherwise the right thing, since every slot
 *     is read back - sets THREADPTR to 0. The write then goes to a low address.
 *
 * Measured before this was fixed, `s3net example.com` repeated a few times:
 *
 *   Guru Meditation Error: Core 0 panic'ed (StoreProhibited)
 *   PC  : 0x420444d1   A3  : 0x000000fd
 *   -> lwip_recv_tcp at lwip/src/api/sockets.c:1003   (set_errno)
 *
 * 0xfd in A3 is the errno value being stored; the store went nowhere because
 * THREADPTR was 0. The fault only appears on a socket that reports an error, so
 * a successful recv() hides it - which is exactly why it took a repeated run.
 *
 * The layout mirrors ESP-IDF's FreeRTOS port (portable/xtensa/port.c,
 * uxInitialiseStackTLS + uxInitialiseStackFrame): the TLS area sits ABOVE the
 * frame so the thread's own stack, which grows down from below the frame, can
 * never reach it, and THREADPTR is computed with the ABI's implicit 8-byte TCB.
 */
rt_uint8_t *rt_hw_stack_init(void *tentry, void *parameter,
                             rt_uint8_t *stack_addr, void *texit)
{
    static int fpu_enabled;
    static rt_size_t   tls_area_size;
    static rt_uint32_t tls_base;
    static int         tls_ready;
    rt_ubase_t top = (rt_ubase_t)stack_addr + sizeof(rt_ubase_t);
    rt_ubase_t tls_area;
    rt_ubase_t frame;
    rt_ubase_t fp_block;
    XtExcFrame *f;

    /* The TLS image size is a link-time constant, so it is computed once. */
    if (!tls_ready)
    {
        tls_ready = 1;
        extern char _thread_local_data_start[], _thread_local_data_end[];
        extern char _thread_local_bss_start[], _thread_local_bss_end[];
        extern int  _tls_section_alignment;

        rt_size_t data_size = (rt_size_t)((rt_ubase_t)_thread_local_data_end -
                                          (rt_ubase_t)_thread_local_data_start);
        rt_size_t bss_size  = (rt_size_t)((rt_ubase_t)_thread_local_bss_end -
                                          (rt_ubase_t)_thread_local_bss_start);

        /* Xtensa's TLS ABI adds an implicit, 8-byte TCB to the offset the
         * linker computes ("TCB_SIZE is hardcoded to 8", elf32-xtensa.c in
         * BFD), aligned up to the section's own alignment. */
        rt_size_t align = (rt_size_t)(rt_ubase_t)&_tls_section_alignment;

        tls_area_size = RT_ALIGN(data_size + bss_size, 16);
        tls_base      = (rt_uint32_t)RT_ALIGN(align, 8);
    }

    /* TLS area at the very top, then the frame below it. */
    tls_area = (top - tls_area_size) & ~(rt_ubase_t)15;
    frame    = (tls_area - XT_STK_FRMSZ) & ~(rt_ubase_t)15;
    fp_block = frame - XT_RT_FP_BLOCK_SIZE;

    if (!fpu_enabled)
    {
        fpu_enabled = 1;
        rt_hw_xtensa_enable_fpu();
    }

    /* Every slot is read by _xt_context_restore / _rtt_resume / _xt_user_exit,
     * so nothing may be left uninitialized. */
    rt_memset((void *)frame, 0, XT_STK_FRMSZ);
    rt_memset((void *)fp_block, 0, XT_RT_FP_BLOCK_SIZE);

    /* Seed this thread's TLS image from the linker's copy, the same way a
     * freshly created task is seeded on FreeRTOS. */
    if (tls_area_size != 0)
    {
        extern char _thread_local_data_start[], _thread_local_data_end[];

        rt_size_t data_size = (rt_size_t)((rt_ubase_t)_thread_local_data_end -
                                          (rt_ubase_t)_thread_local_data_start);

        rt_memcpy((void *)tls_area, _thread_local_data_start, data_size);
        rt_memset((void *)(tls_area + data_size), 0, tls_area_size - data_size);
    }

    /* The word _xt_context_restore reloads into THREADPTR. */
    *(rt_uint32_t *)(frame + XT_STK_EXTRA) = (rt_uint32_t)tls_area - tls_base;

    /* Marker at the FP block's first word is what tells _rtt_resume this is an
     * exception frame and not a solicited one (whose first word is 0). */
    *(rt_uint32_t *)fp_block = XT_RT_FP_FRAME_MARK;

    f = (XtExcFrame *)frame;
    f->exit = (rt_ubase_t)_xt_user_exit;   /* resume path returns through it */
    f->pc   = (rt_ubase_t)rt_xt_thread_start;
    f->a0   = 0;                           /* terminate a backtrace */
    f->a1   = frame + XT_STK_FRMSZ;        /* the thread's own sp, above the frame */
    f->a6   = (rt_ubase_t)tentry;          /* call4 argument 0: the thread entry */
    f->a7   = (rt_ubase_t)parameter;       /* call4 argument 1: its parameter */
    f->a8   = (rt_ubase_t)texit;           /* call4 argument 2: the exit handler */
    f->ps   = PS_UM | PS_EXCM | PS_WOE | PS_CALLINC(1);

    return (rt_uint8_t *)fp_block;
}

/* ---- misc ---------------------------------------------------------------- */

/*
 * How the chip is actually restarted is board policy, not architecture: ESP32
 * wants esp_restart(), a QEMU target may want a semihosting call, a bare-metal
 * one a watchdog write. So the port declares a hook and the BSP fills it in.
 *
 * Deliberately not a weak definition here: a weak function that silently does
 * nothing would make `rt_hw_cpu_reset()` return into the caller on a board that
 * forgot to implement it, which is the worst possible behaviour for a reset.
 * The linker error says exactly what is missing.
 */
extern void rt_hw_board_reset(void);

void rt_hw_cpu_reset(void)
{
    rt_hw_board_reset();

    /* The board hook is not expected to return; if it does, do not run any
     * further code from a half-torn-down state. */
    while (1)
    {
    }
}

void rt_hw_cpu_shutdown(void)
{
    while (1)
    {
    }
}

/*
 * The kernel declares this in rthw.h and falls back to a weak "unknown" in
 * kservice.c. Every other libcpu port names its architecture here, and it is
 * what `msh` prints in some package diagnostics.
 */
const char *rt_hw_cpu_arch(void)
{
    return "xtensa";
}

/*
 * rthw.h declares an exception-handler hook. On Xtensa the exception path runs
 * entirely through the vendor's vector table (`_xt_panic`, the load/store error
 * handlers, the coprocessor exception), and there is no per-vector trap
 * dispatcher for a C callback to hang off, so nothing here can honour the
 * contract.
 *
 * The return type is void, matching rthw.h, so "not supported" cannot be
 * communicated through a value. It is logged once instead: a caller that
 * installs a handler and waits for it to fire would otherwise wait forever with
 * no explanation.
 */
void rt_hw_exception_install(rt_err_t (*exception_handle)(void *context))
{
    RT_UNUSED(exception_handle);

    rt_kprintf("xtensa: rt_hw_exception_install() is not supported on this "
               "architecture; exceptions are handled by the vendor vector "
               "table (_xt_panic)\n");
}

/* ------------------------------------------------------------------------- */
/* Stack-size contract                                                        */
/* ------------------------------------------------------------------------- */

/*
 * The kernel's default stacks are a few hundred bytes; an Xtensa thread needs
 * two frames plus the FP block before it runs a single line of its own code.
 * Turning that into a build error is much better than the alternative, which is
 * a thread that starts with its frame below thread->stack_addr and quietly
 * overwrites whatever the heap allocator parked there.
 */
_Static_assert(IDLE_THREAD_STACK_SIZE >= XT_RT_STACK_MIN,
               "IDLE_THREAD_STACK_SIZE is below the ESP32-S3 Xtensa minimum");
_Static_assert(FINSH_THREAD_STACK_SIZE >= XT_RT_STACK_MIN,
               "FINSH_THREAD_STACK_SIZE is below the ESP32-S3 Xtensa minimum");
