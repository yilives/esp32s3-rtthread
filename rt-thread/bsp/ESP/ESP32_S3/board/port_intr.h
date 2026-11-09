/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 (Xtensa LX7) port
 *
 * ESP32-S3 interrupt helpers, Xtensa flavour.
 *
 * Only the parts that differ from the RISC-V port are declared here; the
 * counterpart for that architecture is port/esp32s31/port_intr.h, and the two
 * deliberately do not share a header because the API really is different:
 * on Xtensa the level of a line is fixed by the vectors and cannot be
 * programmed, so there is no rt_hw_interrupt_set_priority().
 */

#ifndef __PORT_INTR_H__
#define __PORT_INTR_H__

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Route a peripheral interrupt source onto a CPU interrupt line.
 *
 * Wraps esp_rom_route_intr_matrix(0, source, vector). Safe to call repeatedly.
 * Only needed by drivers that install through rt_hw_interrupt_install();
 * esp_intr_alloc() performs the same step itself.
 *
 * @param source ETS_*_INTR_SOURCE (interrupt matrix input index)
 * @param vector ETS_*_INUM (CPU interrupt line, 0..XCHAL_NUM_INTERRUPTS-1)
 * @return 0 on success, -RT_EINVAL if a number is out of range
 */
int rt_hw_intr_route(int source, int vector);

/**
 * Whether a CPU line's interrupt-enable bit is currently clear.
 *
 * On Xtensa this reads INTENABLE directly; there is no separate level field to
 * confuse it with.
 */
rt_bool_t rt_hw_interrupt_is_masked(int vector);

#ifdef __cplusplus
}
#endif

#endif /* __PORT_INTR_H__ */
