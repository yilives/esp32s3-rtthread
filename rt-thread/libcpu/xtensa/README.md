# Xtensa (LX6/LX7) porting layer

The architecture layer for Xtensa cores, used by the ESP32 family. It is
deliberately small: the interrupt/exception vector table is **not** part of it.

## What this port does not build

Xtensa's vector table is large and entirely mechanical - four window
overflow/underflow vectors, six interrupt levels, the debug and NMI vectors, the
load/store error handler, the coprocessor exception path and `_xt_panic`. Writing
it again would be ~2000 lines of assembly whose only interesting property is that
it matches the ISA exactly.

Instead this port builds on **Espressif's `xtensa_vectors.S` and
`xtensa_context.S`**, which are OS agnostic. They talk to an RTOS through exactly
five macros, declared in `xtensa_rtos.h`:

| Macro | Where it is defined | What this port points it at |
| --- | --- | --- |
| `XT_RTOS_INT_ENTER` | `xtensa_rtos.h` | `_rtt_int_enter` in `context_gcc.S` |
| `XT_RTOS_INT_EXIT` | `xtensa_rtos.h` | `_rtt_int_exit` in `context_gcc.S` |
| `XT_RTOS_CP_STATE` | `xtensa_rtos.h` | `_rtt_coproc_state` in `context_gcc.S` |
| `XT_RTOS_CP_EXC_HOOK` | `xtensa_rtos.h` | `_rtt_coproc_exc_hook` in `context_gcc.S` |
| `XT_RTOS_TIMER_INT` | left undefined | the BSP supplies the tick (see below) |

`xtensa_vectors.S` references no RTOS symbol directly, so nothing about the
vector table has to change. What this directory supplies is the other half: the
context switch, the first context of a thread, and the C glue.

## Files

| File | Contents |
| --- | --- |
| `cpuport.h` | frame constants, the two frame kinds, stack size floors |
| `context_gcc.S` | `_rtt_int_enter`/`_rtt_int_exit`, `rt_hw_context_switch*`, `rt_hw_interrupt_disable/enable` |
| `cpuport.c` | `rt_hw_stack_init`, the ISR stack, the globals the kernel externs |
| `backtrace.c` | `rt_hw_backtrace_frame_get`/`_unwind` over the windowed ABI's base-save area |

## FPU policy

Xtensa LX7 has a single-precision FPU as coprocessor 0, and `f0-f15` are
*caller* saved in the windowed ABI. That gives two different rules:

- **Cooperative switch**: no FP save. GCC has already spilled whatever was live
  before calling into the kernel.
- **Interrupt**: eager save. The interrupted instruction stream may hold live
  values in `f0-f15` that no `XtExcFrame` slot covers.

So `_rtt_int_enter` stores `FCR`/`FSR`/`f0-f15` into an 80-byte block below the
exception frame and `thread->sp` points at that block. Sixteen `ssi` on entry and
sixteen `lsi` on the way out buy the removal of the lazy coprocessor ownership
machinery entirely.

Consequence, stated plainly: `FCR`/`FSR` are effectively process-wide for
cooperative switches - a thread that changes the rounding mode and then blocks
hands that mode to the next thread.

Coprocessor 3 (AI/SIMD on some LX7 variants) is left disabled. `ee.*`
instructions raise `CoprocessorDisabled`, `_xt_coproc_exc` asks
`XT_RTOS_CP_STATE` for a save area, this port answers "none", and the vector
panics. That is deliberate: there is no per-thread save area to give it.

## Configuration

The port is architecture-only: it takes no BSP policy. A BSP has to provide

| Symbol | Meaning |
| --- | --- |
| `SOC_XTENSA_FP_BLOCK_SIZE` | override the 80-byte FP block (rarely needed) |
| tick source | `XT_RTOS_TIMER_INT` stays undefined; the BSP drives `rt_tick_increase()` |
| `RT_HW_INCLUDE_CPUPORT` | already selected by `ARCH_XTENSA` |
| toolchain headers | this port includes `<xtensa_context.h>` and `<xtensa_api.h>`, which come from the chip vendor's SDK (Espressif ships them in `components/xtensa`) |

The last row is why this port is not in the same shape as `libcpu/risc-v`: those
headers are generated per chip by `xtensa-config`, so no portable copy can exist.
A BSP must put its chip's `xtensa/include` directory on the include path.
