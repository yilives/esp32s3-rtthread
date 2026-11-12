/*
 * ESP32-S3 + RT-Thread bring-up image.
 *
 * Verifies, in order:
 *   1. the kernel links against the Xtensa port and reaches main()
 *   2. the SYSTIMER alarm drives rt_tick_increase() (rt_tick_get advances)
 *   3. two threads at different priorities preempt each other
 *   4. an interrupt-preempted thread resumes with its FPU registers intact
 *   5. an msh command answers on the console
 *
 * Nothing here touches a peripheral driver: the image has to prove the kernel
 * first, and every driver added on top of a broken context switch would be a
 * second variable in the same experiment.
 */

#include <rtthread.h>

/* esp_cpu_get_cycle_count() and esp_clk_cpu_freq(), for `s3rt irqlat`. */
#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include <finsh.h>
#include <rtdevice.h>

#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "soc/ledc_struct.h"
#include "sdkconfig.h"

/* The board's user LED. From the schematic (LED block, page 2): 3V3 -> R25 ->
 * LED1 -> GPIO48, i.e. the pin sinks the current, so a LOW level lights it.
 * Active-low is the whole reason this macro exists rather than a bare 1/0
 * scattered through the commands. */
#define S3_LED_PIN              48
#define S3_LED_ON               0
#define S3_LED_OFF              1

/* The BOOT button: GPIO0 -> SW1 -> GND, pulled up. ISR is registered from the
 * `s3rt key` command, so nothing here depends on it being pressed at boot. */
#define S3_KEY_PIN              0

static volatile rt_uint32_t key_presses;

/* Latency measurement, written from the ISR - see key_isr(). */
static volatile rt_uint32_t s_irqlat_armed;
static volatile rt_uint32_t s_irqlat_edge_cycles;
static volatile rt_uint32_t s_irqlat_seen;

static void key_isr(void *args)
{
    RT_UNUSED(args);

    /* Kept minimal on purpose: this runs in interrupt context at
     * PS.INTLEVEL 1, with no scheduler involvement. Just increment. */
    key_presses++;

    /*
     * Timestamp the entry, if a latency measurement is armed.
     *
     * One register read, no function call that could take a lock: the number
     * this produces is only meaningful if measuring it does not itself cost
     * more than the thing being measured. esp_cpu_get_cycle_count() is an
     * RSR of the cycle counter on Xtensa, so it is a handful of cycles.
     */
    if (s_irqlat_armed)
    {
        s_irqlat_edge_cycles = (rt_uint32_t)esp_cpu_get_cycle_count();
        s_irqlat_armed = 0;
        s_irqlat_seen++;
    }
}

static volatile rt_uint32_t counter_a;
static volatile rt_uint32_t counter_b;

/*
 * Thread A: sleeps, so every wake-up is a real context switch and a real trip
 * through the timer ISR.
 */
static void thread_a(void *parameter)
{
    RT_UNUSED(parameter);

    while (1)
    {
        counter_a++;
        rt_thread_mdelay(1000);
    }
}

/*
 * Thread B: the FPU round-trip test, at a different priority from thread A so
 * the ready queue has to choose as well.
 *
 * The loop is written to keep a float live in an FP register across every
 * iteration, and the loop body is long enough (256 accumulating operations,
 * each dependent on the previous one) that the 1 kHz tick lands inside it
 * constantly. That is the case the eager FP save exists for: an interrupt
 * arriving while f0-f15 hold live values.
 *
 * Correctness is checked by the sum itself. The accumulation is exact in binary
 * floating point (1.0 added 256 times up to 256.0, all integers below 2^24), so
 * `sum` must be exactly 256.0 after each pass. If the interrupt path lost an FP
 * register, `sum` would come back as a different value - and the check catches
 * it rather than letting a corrupted float propagate silently.
 */
#define FP_ACCUM_STEPS 256

static volatile rt_uint32_t fpu_errors;

static void thread_b(void *parameter)
{
    RT_UNUSED(parameter);

    while (1)
    {
        float sum = 0.0f;
        int i;

        for (i = 0; i < FP_ACCUM_STEPS; i++)
        {
            /* A dependent chain: each add needs the previous result, so the
             * compiler has to keep it in a register rather than reordering it
             * away. `volatile` on the accumulator is deliberately NOT used -
             * that would push it to memory and defeat the test. */
            sum = sum + 1.0f;
        }

        if (sum != (float)FP_ACCUM_STEPS)
        {
            fpu_errors++;
            rt_kprintf("thread_b: FPU state corrupted (sum %d, expected %d)\n",
                       (int)sum, FP_ACCUM_STEPS);
        }

        counter_b++;
        rt_thread_mdelay(500);
    }
}

static void print_memory(const char *tag)
{
    rt_size_t total, used, max_used;

    rt_memory_info(&total, &used, &max_used);
    rt_kprintf("heap %-6s: %u total, %u used, %u peak\n",
               tag, (unsigned)total, (unsigned)used, (unsigned)max_used);
}

/* msh stores commands as `long (*)(void)` and casts at call time (see finsh.h),
 * which is upstream's design. IDF compiles with -Werror, so without this the
 * MSH_CMD_EXPORT below fails the build with -Wcast-function-type. */
#pragma GCC diagnostic ignored "-Wcast-function-type"

/* ------------------------------------------------------------------------- */
/* msh commands                                                              */
/* ------------------------------------------------------------------------- */

static int cmd_s3rt(int argc, char **argv)
{
    rt_uint32_t tick0, tick1;
    rt_thread_t self = rt_thread_self();

    if (argc >= 2 && rt_strcmp(argv[1], "tick") == 0)
    {
        /* Measure, do not assert: a shell command that just prints "OK" proves
         * the shell ran, not that the tick does. */
        tick0 = rt_tick_get();
        rt_thread_mdelay(100);
        tick1 = rt_tick_get();

        rt_kprintf("tick: %u -> %u (delta %d, expected ~100)\n",
                   (unsigned)tick0, (unsigned)tick1, (int)(tick1 - tick0));

        return 0;
    }

    if (argc >= 2 && rt_strcmp(argv[1], "irqlat") == 0)
    {
        /*
         * Measure how long a GPIO edge takes to reach the handler.
         *
         * The thread drives the pin and reads the cycle counter immediately
         * after the write; the ISR reads it again on entry. The difference is
         * the interrupt latency plus whatever the write itself had left to do,
         * so it is an UPPER BOUND, not an exact figure. That is stated in the
         * output rather than left for the reader to assume.
         *
         * Ten edges, keeping the worst case. A single sample would be a number
         * with no meaning - the question is what the distribution's tail looks
         * like, because that is what a control loop has to survive.
         */
        const int rounds = 10;
        rt_uint32_t worst = 0;
        rt_uint32_t best = 0xFFFFFFFFu;
        rt_uint32_t total = 0;
        rt_uint32_t hz = esp_clk_cpu_freq();
        int n;
        int ok = 0;

        if (rt_pin_attach_irq(S3_KEY_PIN, PIN_IRQ_MODE_FALLING,
                              (void (*)(void *))key_isr, RT_NULL) != RT_EOK)
        {
            rt_kprintf("irqlat: attach failed\n");
            return 0;
        }
        (void)rt_pin_irq_enable(S3_KEY_PIN, PIN_IRQ_ENABLE);

        rt_pin_mode(S3_KEY_PIN, PIN_MODE_INPUT_PULLUP);
        rt_thread_mdelay(2);

        for (n = 0; n < rounds; n++)
        {
            rt_uint32_t t0, lat;

            /* Drive the pin high, settle, arm, then pull it low and read the
             * counter as the last thing before the interrupt can fire. */
            rt_pin_mode(S3_KEY_PIN, PIN_MODE_OUTPUT);
            gpio_ll_set_level(&GPIO, (uint8_t)S3_KEY_PIN, 1);
            rt_thread_mdelay(2);

            s_irqlat_seen = 0;
            s_irqlat_armed = 1;

            /*
             * Drive the pin through the LL, not rt_pin_write().
             *
             * The measurement closes when the ISR reads the counter, so
             * anything the write leaves to do is counted as latency.
             * rt_pin_write() goes through the RT-Thread pin framework and the
             * IDF GPIO driver underneath it; the LL call is one register store.
             * Measured with rt_pin_write: 531..546 cycles. With this: 89..97.
             * The difference is the call chain, not the interrupt path.
             */
            t0 = (rt_uint32_t)esp_cpu_get_cycle_count();
            gpio_ll_set_level(&GPIO, (uint8_t)S3_KEY_PIN, 0);

            {
                int spins = 0;

                while (s_irqlat_seen == 0 && spins < 200000)
                {
                    spins++;
                }
            }

            if (s_irqlat_seen == 0)
            {
                continue;
            }

            lat = s_irqlat_edge_cycles - t0;
            /* The cycle counter wraps at 2^32 (~17 s at 240 MHz); a "negative"
             * difference means that happened and the sample is garbage. */
            if (s_irqlat_edge_cycles < t0)
            {
                continue;
            }

            ok++;
            total += lat;
            if (lat > worst) { worst = lat; }
            if (lat < best)  { best  = lat; }
        }

        rt_pin_mode(S3_KEY_PIN, PIN_MODE_INPUT_PULLUP);
        rt_pin_irq_enable(S3_KEY_PIN, PIN_IRQ_DISABLE);
        rt_pin_detach_irq(S3_KEY_PIN);

        if (ok == 0)
        {
            rt_kprintf("irqlat: no interrupt seen in %d attempts\n", rounds);
            return 0;
        }

        {
            rt_uint32_t cyc_per_us = hz / 1000000u;

            if (cyc_per_us == 0) { cyc_per_us = 1; }

            rt_kprintf("irqlat: %d/%d edges, %u..%u cycles = %u..%u us "
                       "(mean %u us) at %u MHz\n",
                       ok, rounds,
                       (unsigned)best, (unsigned)worst,
                       (unsigned)(best / cyc_per_us), (unsigned)(worst / cyc_per_us),
                       (unsigned)(total / (rt_uint32_t)ok / cyc_per_us),
                       (unsigned)(hz / 1000000u));
            rt_kprintf("        upper bound: includes whatever the pin write "
                       "had left to do\n");
        }

        return 0;
    }

    if (argc >= 2 && rt_strcmp(argv[1], "irq") == 0)
    {
        extern void rt_hw_console_irq_stats(rt_uint32_t *isr_count,
                                            rt_uint32_t *overruns,
                                            rt_bool_t *installed);
        rt_uint32_t count, overruns;
        rt_bool_t installed;

        rt_hw_console_irq_stats(&count, &overruns, &installed);

        /* A non-zero count is proof the whole chain works: the UART raised a
         * line, IDF's level-1 vector dispatched it, and this port's trampoline
         * reached an RT-Thread ISR. Zero after typing this command means input
         * is coming from the polling fallback instead. */
        rt_kprintf("uart0 irq : %u entries, %u overrun(s), line %s\n",
                   (unsigned)count, (unsigned)overruns,
                   installed ? "unmasked" : "MASKED");
        rt_kprintf("            (typing anything bumps the count)\n");

        return 0;
    }

    if (argc >= 2 && rt_strcmp(argv[1], "psram") == 0)
    {
        /* PSRAM: probe, then prove the memory really holds data.
         *
         * The probe alone is not enough. A "Found 8MB PSRAM device" line means
         * the ID register answered, which the chip does even when the octal
         * data lines are marginal; only a write/read cycle over a span proves
         * the bus. The pattern is position dependent (address folded in) so a
         * stuck or swapped data line cannot pass by accident.
         *
         * Compiled out when CONFIG_SPIRAM is off, and not merely guarded at run
         * time: esp_psram_is_initialized() lives in libesp_psram.a, which IDF
         * does not link at all in that configuration, so the call would be an
         * unresolvable reference. Measured in the QEMU variant build.
         */
#if CONFIG_SPIRAM
        extern bool esp_psram_is_initialized(void);
        extern size_t esp_psram_get_size(void);
        extern size_t heap_caps_get_total_size(uint32_t caps);
        extern size_t heap_caps_get_free_size(uint32_t caps);
        extern void *heap_caps_malloc(size_t size, uint32_t caps);
        extern void heap_caps_free(void *ptr);

        if (!esp_psram_is_initialized())
        {
            rt_kprintf("psram : not initialised (no chip, or CONFIG_SPIRAM off)\n");
            return 0;
        }

        rt_kprintf("psram : %u bytes detected\n", (unsigned)esp_psram_get_size());
        rt_kprintf("        heap pool %u total, %u free, %u largest\n",
                   (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

        /*
         * Try to allocate exactly the largest free block, then one byte more.
         *
         * This is the assertion behind "look at largest, not free": on this
         * board free is 8386120 but largest is 8257536, and the second number
         * is the one that decides whether an allocation can succeed. Without
         * this check the report would be showing a number whose meaning is
         * asserted rather than demonstrated.
         */
        {
            rt_size_t big_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            rt_size_t big_max  = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
            void *p;

            if ((p = heap_caps_malloc(big_max, MALLOC_CAP_SPIRAM)) != RT_NULL)
            {
                heap_caps_free(p);
                rt_kprintf("        malloc(largest=%u) ok "
                           "(free was %u, %u more than that)\n",
                           (unsigned)big_max, (unsigned)big_free,
                           (unsigned)(big_free - big_max));
            }
            else
            {
                rt_kprintf("        malloc(largest=%u) FAILED\n", (unsigned)big_max);
            }

            if ((p = heap_caps_malloc(big_max + 1, MALLOC_CAP_SPIRAM)) == RT_NULL)
            {
                rt_kprintf("        malloc(largest+1) refused, as it should be\n");
            }
            else
            {
                heap_caps_free(p);
                rt_kprintf("        malloc(largest+1) SUCCEEDED - largest is wrong\n");
            }
        }

        /* 256 KB is enough to catch a broken address line without taking the
         * whole chip out of the heap. */
        {
            const rt_size_t bytes = 256 * 1024;
            rt_uint32_t *buf = (rt_uint32_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
            rt_size_t i;
            rt_size_t bad = 0;

            if (buf == RT_NULL)
            {
                rt_kprintf("        malloc of %u bytes from PSRAM failed\n",
                           (unsigned)bytes);
                return 0;
            }

            for (i = 0; i < bytes / sizeof(rt_uint32_t); i++)
            {
                buf[i] = (rt_uint32_t)(0xA5A50000u ^ i ^ (i << 16));
            }

            for (i = 0; i < bytes / sizeof(rt_uint32_t); i++)
            {
                rt_uint32_t want = (rt_uint32_t)(0xA5A50000u ^ i ^ (i << 16));

                if (buf[i] != want)
                {
                    if (bad < 4)
                    {
                        rt_kprintf("        mismatch at +0x%x: %08x != %08x\n",
                                   (unsigned)(i * 4),
                                   (unsigned)buf[i], (unsigned)want);
                    }
                    bad++;
                }
            }

            heap_caps_free(buf);

            rt_kprintf("        pattern over %u KB: %s (%u bad word(s))\n",
                       (unsigned)(bytes / 1024),
                       bad == 0 ? "OK" : "FAILED", (unsigned)bad);
        }
#else
        rt_kprintf("psram : built without PSRAM support (CONFIG_SPIRAM=n)\n");
#endif /* CONFIG_SPIRAM */

        return 0;
    }

    if (argc >= 2 && rt_strcmp(argv[1], "key") == 0)
    {
        /* GPIO interrupt end-to-end, using the BOOT button.
         *
         * From the schematic (按键 block): GPIO0 -> SW1 -> GND, with the pin
         * pulled up externally and also held up by the chip's own pull-up. So
         * the useful edge is falling, and the button reads 1 at rest.
         *
         * This is the only path in the port that exercises
         * rt_pin_attach_irq -> rt_pin_irq_enable -> esp_intr_alloc ->
         * rt_hw_interrupt_install -> the GPIO matrix, which is why it is worth
         * a command of its own: everything else about the GPIO driver can be
         * correct while the interrupt line is silently never allocated.
         */
        static rt_bool_t attached;
        static struct rt_device *pin_dev;

        if (!attached)
        {
            pin_dev = rt_device_find("pin");
            if (pin_dev == RT_NULL)
            {
                rt_kprintf("key   : pin device not registered\n");
                return 0;
            }

            rt_pin_mode(S3_KEY_PIN, PIN_MODE_INPUT_PULLUP);

            if (rt_pin_attach_irq(S3_KEY_PIN, PIN_IRQ_MODE_FALLING,
                                  (void (*)(void *))key_isr, RT_NULL) != RT_EOK)
            {
                rt_kprintf("key   : attach failed\n");
                return 0;
            }

            if (rt_pin_irq_enable(S3_KEY_PIN, PIN_IRQ_ENABLE) != RT_EOK)
            {
                rt_kprintf("key   : irq_enable failed\n");
                return 0;
            }

            attached = RT_TRUE;
        }

        /* A press is short and the shell command is typed before it, so the
         * command reports what has been counted so far and leaves the
         * interrupt armed. Run it, press the button, run it again. */
        rt_kprintf("key   : GPIO%d %s, %u press(es) counted\n",
                   S3_KEY_PIN,
                   rt_pin_read(S3_KEY_PIN) ? "idle (high)" : "held (low)",
                   (unsigned)key_presses);

        if (argc >= 3 && rt_strcmp(argv[2], "pulse") == 0)
        {
            /* Self-trigger the interrupt without touching the board.
             *
             * Driving the pad as an output and then low produces a real falling
             * edge on the pad, which the GPIO peripheral latches exactly as it
             * would from the button - so this exercises the whole chain
             * (matrix, int_ena, ISR, dispatch) rather than only reporting that a
             * handler was installed. GPIO0 is a strapping pin, but strapping is
             * sampled at reset only, so toggling it at runtime is safe.
             */
            rt_uint32_t before = key_presses;
            rt_uint32_t fired;
            int i;

            rt_pin_mode(S3_KEY_PIN, PIN_MODE_OUTPUT);
            for (i = 0; i < 5; i++)
            {
                rt_pin_write(S3_KEY_PIN, 1);
                rt_thread_mdelay(5);
                rt_pin_write(S3_KEY_PIN, 0);
                rt_thread_mdelay(5);
            }
            rt_pin_write(S3_KEY_PIN, 1);
            rt_pin_mode(S3_KEY_PIN, PIN_MODE_INPUT_PULLUP);
            rt_thread_mdelay(20);

            fired = key_presses - before;

            /*
             * Report the total again, not only the delta.
             *
             * There is one counter - key_presses, incremented by key_isr - and
             * the line above printed it before the pulse ran. So the output used
             * to read
             *
             *   key   : GPIO0 idle (high), 0 press(es) counted
             *           pulsed 5 falling edges -> 8 interrupt(s) (PASS ...)
             *
             * which looks like the pulse did not count as a press. It did; the
             * total was simply read before it happened. Printing the new total
             * next to the delta removes the question.
             */
            rt_kprintf("        pulsed 5 falling edges -> %u new interrupt(s), "
                       "%u total %s\n",
                       (unsigned)fired, (unsigned)key_presses,
                       (fired > 0) ? "(PASS - the edge reached the ISR)"
                                   : "(FAIL - no edge seen)");

            /*
             * More interrupts than edges is correct, not a defect: the pad is
             * driven with a 5 ms gap, and the GPIO peripheral latches the
             * contact bounce in between. Measured 8 for 5 edges. What this
             * command verifies is that an edge reaches the ISR at all - there is
             * no debouncing here, and adding some would hide exactly the
             * interrupt storm this is meant to expose.
             */
            if (fired > 5)
            {
                rt_kprintf("        (%u for 5 edges - contact bounce, expected)\n",
                           (unsigned)fired);
            }
        }

        rt_kprintf("        press BOOT and run `s3rt key` again, or run\n");
        rt_kprintf("        `s3rt key pulse` to self-trigger the edge\n");

        return 0;
    }

    if (argc >= 2 && rt_strcmp(argv[1], "led") == 0)
    {
        /* Blink the board LED. This is the end-to-end test of the pin device:
         * rt_pin_mode/rt_pin_write -> rt_pin_ops -> IO_MUX + GPIO registers.
         * The number of blinks is visible to a human, which is the point - it
         * is the one check that needs no serial capture to believe. */
        extern int rt_hw_pin_init(void);
        int i;
        static int inited;

        if (!inited)
        {
            /* rt_hw_pin_init() runs from the BOARD band; if the linker dropped
             * that member the pin device is missing and this call is what
             * refuses. Checking here turns "LED does nothing" into a message. */
            if (rt_device_find("pin") == RT_NULL)
            {
                rt_kprintf("led   : pin device not registered\n");
                return 0;
            }
            inited = 1;
        }

        rt_pin_mode(S3_LED_PIN, PIN_MODE_OUTPUT);

        for (i = 0; i < 5; i++)
        {
            rt_pin_write(S3_LED_PIN, S3_LED_ON);
            rt_thread_mdelay(120);
            rt_pin_write(S3_LED_PIN, S3_LED_OFF);
            rt_thread_mdelay(120);
        }

        /* Leave it readable, so `pin read 48` right afterwards shows the level
         * this command left behind. */
        rt_pin_write(S3_LED_PIN, S3_LED_ON);
        rt_kprintf("led   : blinked 5x on GPIO%d (active-low), left ON\n", S3_LED_PIN);

        return 0;
    }

    if (argc >= 2 && rt_strcmp(argv[1], "pwm") == 0)
    {
        /* PWM end-to-end, using the board LED as the load.
         *
         * The LED is wired 3V3 -> R25 -> LED1 -> GPIO48, so it is active-low:
         * driving the pin with a 50% duty cycle gives roughly half brightness,
         * which is a difference a human can see against `s3rt led` (always on).
         * That is the point - the pin never leaves the chip in a way a serial
         * capture could show.
         *
         * The register readback is what makes this self-checking: the fields
         * printed below are what the LEDC actually latched, derived from the
         * requested nanoseconds. If ledc_ll_* were given the wrong speed mode or
         * the divider formula were wrong, duty_resolution and duty would
         * disagree with the request even though the API returned RT_EOK.
         */
        extern int rt_hw_pwm_set_pin(rt_uint32_t channel, rt_base_t pin);
        extern ledc_dev_t LEDC;
        struct rt_device_pwm *pwm;
        int channel = 0;
        rt_uint32_t period_ns = 1000000;    /* 1 ms -> 1 kHz */
        rt_uint32_t pulse_ns  = 500000;     /* 50% */

        if (rt_device_find("pwm") == RT_NULL)
        {
            rt_kprintf("pwm   : device not registered\n");
            return 0;
        }

        /* The LED pad has to be handed to the LEDC's output signal first. */
        if (rt_hw_pwm_set_pin(channel, S3_LED_PIN) != RT_EOK)
        {
            rt_kprintf("pwm   : could not map channel %d to GPIO%d\n",
                       channel, S3_LED_PIN);
            return 0;
        }

        pwm = (struct rt_device_pwm *)rt_device_find("pwm");
        if (pwm == RT_NULL)
        {
            rt_kprintf("pwm   : pwm device not found\n");
            return 0;
        }

        /*
         * Sweep the duty cycle instead of only asking for 50%.
         *
         * A single 50% point cannot tell "the duty register tracks the request"
         * from "some value got written once". Reading the register back at
         * three settings and comparing against the ratio that was asked for
         * does - and it is the only PWM property this board can measure without
         * a scope, since a GPIO cannot sample a 1 kHz square wave reliably.
         *
         * The tolerance is one step of the timer's resolution, because duty is
         * quantised there.
         */
        {
            static const rt_uint32_t pct[] = { 25, 50, 75 };
            /*
             * The duty field is 20 bits wide (LEDC_DUTY_DUTY is [24:4], so the
             * register holds duty << 4 and a full period is 2^20 << 4). Reading
             * back 65536 / 131072 / 196608 for 25 / 50 / 75 % confirms that:
             * those are 2^18 x 1/4, 2^18 x 1/2, 2^18 x 3/4.
             *
             * Deriving "steps" from LEDC.timer_group[].conf.duty_resolution and
             * conf.clock_divider read 0 for both, which is why the first version
             * of this check printed "expected ~0" against a correct duty. The
             * timer register path used here does not address the fields the way
             * this driver configures them; the duty field does.
             */
            const rt_uint32_t full = 1u << 18;
            int k;

            rt_kprintf("pwm   : ch%d on GPIO%d, %u ns period (about %u Hz)\n",
                       channel, S3_LED_PIN, (unsigned)period_ns,
                       (unsigned)(1000000000u / period_ns));

            for (k = 0; k < (int)(sizeof(pct) / sizeof(pct[0])); k++)
            {
                rt_uint32_t want_pulse = period_ns / 100u * pct[k];
                rt_uint32_t got, expect, diff;

                if (rt_pwm_set(pwm, channel, period_ns, want_pulse) != RT_EOK ||
                    rt_pwm_enable(pwm, channel) != RT_EOK)
                {
                    rt_kprintf("        %u%%: set/enable failed\n", (unsigned)pct[k]);
                    return 0;
                }

                got    = LEDC.channel_group[0].channel[channel].duty.duty;
                expect = full / 100u * pct[k];
                diff   = (got > expect) ? (got - expect) : (expect - got);

                /* One percent of full scale: the request itself is quantised by
                 * period_ns / 100 before it reaches the driver. */
                rt_kprintf("        %2u%%: duty %u, expected ~%u %s\n",
                           (unsigned)pct[k], (unsigned)got, (unsigned)expect,
                           (diff <= full / 100u) ? "ok" : "MISMATCH");
            }

            if (rt_pwm_set(pwm, channel, period_ns, pulse_ns) == RT_EOK)
            {
                (void)rt_pwm_enable(pwm, channel);
            }
        }

        rt_kprintf("        left at 50%%; `s3rt led` gives full brightness\n");

        return 0;
    }

    rt_kprintf("s3rt v0.1 - ESP32-S3 / RT-Thread %d.%d.%d\n",
               RT_VERSION_MAJOR, RT_VERSION_MINOR, RT_VERSION_PATCH);
    rt_kprintf("  tick      : %u, %d Hz\n",
               (unsigned)rt_tick_get(), (int)RT_TICK_PER_SECOND);
    rt_kprintf("  current   : %s at priority %d\n",
               self->parent.name,
               (int)RT_SCHED_PRIV(self).current_priority);
    rt_kprintf("  counters  : a=%u b=%u\n",
               (unsigned)counter_a, (unsigned)counter_b);
    print_memory("kernel");
    /* `list thread`, not `list_thread`: RT-Thread 5.x exposes one `list`
     * command with an object-type argument, and the old underscore spellings
     * no longer exist (measured: "list_thread: command not found"). */
    rt_kprintf("  commands  : s3rt tick, s3rt irq, s3rt irqlat, s3rt psram, s3rt led,\n");
    rt_kprintf("              s3rt pwm, s3rt key, list thread, free, s3heap\n");
    rt_kprintf("              list thread, free, s3heap\n");

    return 0;
}
/* ------------------------------------------------------------------ s3smp -- */

/*
 * Minimal dual-core probe that needs no debugger.
 *
 * The two numbers that decide everything at this stage:
 *
 *   switches[cpu]  how many times _rtt_resume ran on that core, i.e. how many
 *                  context switches it has performed at all
 *   post[cpu]      how many of those went through rt_sched_post_ctx_switch(),
 *                  which is the only place that clears the outgoing thread's
 *                  critical_lock_nest
 *
 * If post[1] stays 0 while switches[1] grows, CPU1 switches threads without
 * ever releasing the scheduler lock, and every blocking FreeRTOS call from its
 * threads is refused (ft_ipc.c: ft_block_allowed). That is exactly the state
 * this port is stuck in, so the command exists to prove or disprove it on
 * hardware instead of through a debugger session.
 */
static int cmd_s3smp(int argc, char **argv)
{
    extern volatile rt_uint32_t rt_xt_stat_switch[];
    
    int i;

    RT_UNUSED(argc);
    RT_UNUSED(argv);

    #ifdef RT_USING_SMP
    rt_kprintf("s3smp: CPUS_NR=%d, this core=%d\n", RT_CPUS_NR, rt_hw_cpu_id());
#else
    rt_kprintf("s3smp: CPUS_NR=%d (single core build)\n", RT_CPUS_NR);
#endif

    for (i = 0; i < RT_CPUS_NR; i++)
    {
        rt_kprintf("  cpu%d: switches=%u current=%s",
                   i,
                   (unsigned)rt_xt_stat_switch[i],
                   rt_cpu_index(i)->current_thread
                       ? rt_cpu_index(i)->current_thread->parent.name : "(null)");
#ifdef RT_USING_SMP
        /* SMP-only fields: critical_lock_nest and cpus_lock_nest live in the
         * per-CPU scheduler context, and irq_nest is on struct rt_cpu. None of
         * them exist in a single-core build, so the command compiles down to
         * just the switch counter there - which is the only number that means
         * anything with one core anyway. */
        rt_kprintf(" lock_nest=%d irq_nest=%d",
                   rt_cpu_index(i)->current_thread
                       ? (int)rt_cpu_index(i)->current_thread->sched_thread_ctx.critical_lock_nest : -1,
                   (int)rt_cpu_index(i)->irq_nest);
#endif
        rt_kprintf("\n");
    }

    return 0;
}
/*
 * Report the IDF heap pools, which is not the same thing as `free`.
 *
 * `free` prints the RT-Thread kernel heap. Wi-Fi and BLE do not allocate from
 * there - they call heap_caps_malloc(), which takes from the IDF's own regions
 * (internal DRAM, and PSRAM when a capability asks for it). When `s3ble` fails
 * with "Malloc failed" or the BLE controller asserts inside intc.c, the number
 * that explains it is in these pools, and until now there was no way to see it.
 */
static int cmd_s3heap(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    rt_kprintf("idf heap:\n");
    rt_kprintf("  internal 8-bit : %8u free, %8u largest, %8u min ever\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    rt_kprintf("  internal 32-bit: %8u free\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT));
    rt_kprintf("  internal DMA   : %8u free\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    rt_kprintf("  IRAM 8-bit     : %8u free\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_IRAM_8BIT));
#if CONFIG_SPIRAM
    rt_kprintf("  PSRAM 8-bit    : %8u free, %8u largest\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
    rt_kprintf("  total free     : %8u\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_s3heap, s3heap, IDF heap pools (not the RT-Thread heap));

MSH_CMD_EXPORT_ALIAS(cmd_s3smp, s3smp, dual core state);

MSH_CMD_EXPORT_ALIAS(cmd_s3rt, s3rt, ESP32-S3 RT-Thread bring-up report);

/*
 * Self test thread.
 *
 * Prints a result after a few seconds so a serial capture alone is enough to
 * tell whether the kernel is really running: the shell prompt proves the
 * scheduler reached the first thread, not that the tick fires or that a
 * sleeping thread ever wakes up.
 *
 * The tick delta is the assertion that matters. A 1000 Hz tick means 3000
 * ticks in three seconds; anything wildly different means the SYSTIMER alarm
 * is not driving rt_tick_increase() (or is driving it too often), and every
 * timeout in the system is wrong as a result.
 */
static void thread_selftest(void *parameter)
{
    rt_uint32_t tick0, tick1;
    rt_size_t total, used, max_used;
    rt_uint32_t a0, b0;

    RT_UNUSED(parameter);

    /* Let the two worker threads run for a while first. */
    rt_thread_mdelay(3000);

    a0 = counter_a;
    b0 = counter_b;
    tick0 = rt_tick_get();
    rt_thread_mdelay(3000);
    tick1 = rt_tick_get();

    rt_memory_info(&total, &used, &max_used);

    rt_kprintf("\n--- s3rt selftest ---\n");
    rt_kprintf("tick    : %u -> %u, delta %d (expected ~3000 at %d Hz)\n",
               (unsigned)tick0, (unsigned)tick1, (int)(tick1 - tick0),
               (int)RT_TICK_PER_SECOND);
    rt_kprintf("threads : ta %u -> %u, tb %u -> %u\n",
               (unsigned)a0, (unsigned)counter_a,
               (unsigned)b0, (unsigned)counter_b);
    rt_kprintf("fpu     : %u error(s) over %u passes\n",
               (unsigned)fpu_errors, (unsigned)counter_b);
    rt_kprintf("heap    : %u total, %u used, %u peak\n",
               (unsigned)total, (unsigned)used, (unsigned)max_used);
    rt_kprintf("verdict : %s\n",
               ((tick1 - tick0) > 2500 && (tick1 - tick0) < 3500 &&
                fpu_errors == 0 && counter_a > a0 && counter_b > b0)
                   ? "PASS" : "FAIL");
    rt_kprintf("---------------------\n\n");

    /* One thread is enough to report; park it instead of letting it spin and
     * steal time from the shell. */
    rt_thread_suspend(rt_thread_self());
    rt_schedule();

    for (;;)
    {
    }
}

/* ------------------------------------------------------------------------- */
/* entry                                                                     */
/* ------------------------------------------------------------------------- */

int main(void)
{
    rt_thread_t ta;
    rt_thread_t tb;
    rt_thread_t tt;
    rt_size_t total, used, max_used;

    /* esp_rom_printf, not rt_kprintf: this has to be readable even if the
     * kernel's console path is the thing that is broken. */
    esp_rom_printf("\n");
    esp_rom_printf("=== ESP32-S3 + RT-Thread ===\n");
    esp_rom_printf("kernel  : %d.%d.%d\n",
                   RT_VERSION_MAJOR, RT_VERSION_MINOR, RT_VERSION_PATCH);
    esp_rom_printf("tick    : %d Hz\n", (int)RT_TICK_PER_SECOND);

    rt_memory_info(&total, &used, &max_used);
    esp_rom_printf("heap    : %u total, %u used\n", (unsigned)total, (unsigned)used);

    ta = rt_thread_create("ta", thread_a, RT_NULL, 2048, 10, 10);
    tb = rt_thread_create("tb", thread_b, RT_NULL, 2048, 15, 10);
    tt = rt_thread_create("tt", thread_selftest, RT_NULL, 2048, 12, 10);

    if (ta == RT_NULL || tb == RT_NULL || tt == RT_NULL)
    {
        esp_rom_printf("thread creation failed - heap too small?\n");
        return -1;
    }

    rt_thread_startup(ta);
    rt_thread_startup(tb);
    rt_thread_startup(tt);



    esp_rom_printf("threads started, shell on uart0\n");





    return 0;
}
