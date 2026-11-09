/*
 * SPDX-FileCopyrightText: 2026 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * RT-Thread scons build shim for <freertos/portmacro.h>.
 *
 * The ESP BSPs ship their own idf_port/include/freertos/portmacro.h with the
 * RT-Thread kernel port; it is found through #include_next below.  ESP-IDF
 * v6.1 components (the bt controller) additionally reference vPortYield as a
 * plain function, while the BSP header only provides it as a macro for
 * vPortYield() call sites - declare the function here so such references
 * compile.  The definition lives in components/rtthread/scons/port_compat.c.
 */
#ifndef RT_THREAD_SCONS_PORTMACRO_H
#define RT_THREAD_SCONS_PORTMACRO_H

#include_next <freertos/portmacro.h>

/* the BSP portmacro.h provides vPortYield only as a function-like macro for
   vPortYield() call sites; ESP-IDF v6.1 components (the bt controller) take
   it by pointer instead.  Replace the macro with a plain function
   declaration - the scons build has no other vPortYield() call sites (they
   all use portYIELD()), and the definition lives in
   components/rtthread/scons/port_compat.c. */
#undef vPortYield

#ifdef __cplusplus
extern "C" {
#endif

void vPortYield(void);

#ifdef __cplusplus
}
#endif

#endif /* RT_THREAD_SCONS_PORTMACRO_H */
