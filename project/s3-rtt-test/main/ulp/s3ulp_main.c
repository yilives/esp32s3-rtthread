/*
 * ULP-RISC-V program for the ESP32-S3 RT-Thread image.
 *
 * This runs on the ULP co-processor, NOT on either Xtensa core and NOT under
 * RT-Thread. It is a bare-metal program linked against ulp_riscv.ld, started by
 * the main cores through ulp_riscv_run(), and it lives entirely in RTC slow
 * memory (8 KB on this chip, of which CONFIG_ULP_COPROC_RESERVE_MEM is reserved
 * for it).
 *
 * The protocol
 * ------------
 * The ULP cannot be scheduled the way a thread can: it has no TCB, no priority,
 * and no way to call into the kernel. What it can do is interrupt a main core.
 * So the shape is the classic split between a producer that cannot block and a
 * consumer that can:
 *
 *   main core                       ULP
 *   ---------                       ---
 *   write s3ulp_cmd, wait           poll s3ulp_cmd
 *                                   do the work, write s3ulp_result
 *                                   s3ulp_ack++ , wakeup_main_processor()
 *   ISR fires, releases semaphore
 *   thread wakes, reads s3ulp_result
 *
 * The command word is the handshake: the main core writes a value the ULP has
 * not handled yet, and the acknowledgement is what tells it the work is done. No
 * locking is possible across the two processors, so the protocol is kept to
 * single 32-bit words that are individually atomic on both sides.
 *
 * Why not just let the ULP loop forever and read counters
 * ------------------------------------------------------
 * That is what this program did first, and it proves the co-processor runs but
 * cannot demonstrate the thing that matters: that an RT-Thread thread can block
 * on ULP work and be woken by it. A command/acknowledge round trip does.
 */

#include <stdint.h>

#include "ulp_riscv_utils.h"

/*
 * Exported to the main core through the generated ulp_main.h. `volatile` because
 * two independent processors write and read them with no cache coherency
 * protocol - RTC slow memory is shared, but the compiler must not keep a copy in
 * a register across the loop.
 */
volatile uint32_t s3ulp_magic;      /* written once at start-up */
volatile uint32_t s3ulp_cmd;        /* main -> ULP: what to do */
volatile uint32_t s3ulp_seq;        /* main -> ULP: which request this is (monotonic) */
volatile uint32_t s3ulp_ack;        /* ULP -> main: incremented per completed request */
volatile uint32_t s3ulp_result;     /* ULP -> main: payload for the last request */

#define S3ULP_MAGIC 0x55334C50u /* "U3LP" */

#define S3ULP_CMD_NOOP      0u
#define S3ULP_CMD_COUNT     1u
#define S3ULP_CMD_DOUBLE    2u

static uint32_t s3ulp_work(uint32_t cmd)
{
    uint32_t acc = 0;
    uint32_t i;

    if (cmd == S3ULP_CMD_NOOP)
    {
        return 0;
    }

    /*
     * ~2 ms at 20 MHz. Deliberately not instant: the main-side thread has to be
     * genuinely blocked when the interrupt arrives, otherwise the test cannot
     * tell "woken by the ULP" from "never slept".
     */
    for (i = 0; i < 40000u; i++)
    {
        acc += i;
    }

    if (cmd == S3ULP_CMD_DOUBLE)
    {
        return acc * 2u;
    }

    return acc;
}

int main(void)
{
    uint32_t handled_seq = 0;

    s3ulp_magic = S3ULP_MAGIC;

    while (1)
    {
        uint32_t seq = s3ulp_seq;

        /*
         * The handshake is on a SEQUENCE NUMBER, not on the command value.
         *
         * Comparing the command itself works exactly once per distinct value:
         * issuing the same command twice in a row leaves `cmd == handled` and the
         * ULP silently does nothing. Measured - a worker thread repeating
         * CMD_COUNT timed out on every iteration after the first, which looked
         * like a broken interrupt and was a broken protocol.
         *
         * A monotonic sequence is also robust against the true race: the main core
         * updating the request while the ULP is between reading the word and
         * recording it. With a command compare, that transition is
         * indistinguishable from idle; with a sequence compare it is not.
         */
        if (seq != handled_seq)
        {
            s3ulp_result = s3ulp_work(s3ulp_cmd);
            handled_seq = seq;

            /*
             * Tell the main core. This raises RTC_CNTL_SW_CPU_INT, which the RTC
             * ISR picks up and turns into a semaphore release - the whole
             * mechanism by which an RT-Thread thread waits on ULP work.
             *
             * The ack is set BEFORE the wakeup on purpose: the ISR runs on the
             * main core, and if it released the semaphore first, the woken thread
             * could read a stale result.
             */
            s3ulp_ack++;
            ulp_riscv_wakeup_main_processor();
        }
        else
        {
            /*
             * Idle. A short delay rather than a spin: RTC slow memory is not
             * cached and polling it flat out at 20 MHz burns power for nothing.
             */
            ulp_riscv_delay_us(1000);
        }
    }

    return 0;
}
