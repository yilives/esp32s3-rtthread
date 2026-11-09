/*
 * SPDX-FileCopyrightText: 2026 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * The RT-Thread scons build shim for <arpa/inet.h>.
 *
 * mbedTLS 4.x (ESP-IDF v6.1) probes <sys/socket.h> and <arpa/inet.h> with
 * __has_include in x509_crt.c.  A Wi-Fi configuration runs on RT-Thread's
 * own lwIP: without the SAL layer, <sys/socket.h> resolves to the lwIP port
 * while <arpa/inet.h> would resolve to the netdev wrapper, whose copy of the
 * IP address types collides with lwIP's.  Keep both probes on the lwIP copy.
 */
#ifndef RT_THREAD_SCONS_INET_H
#define RT_THREAD_SCONS_INET_H

#include <rtconfig.h>

#if defined(RT_USING_LWIP) && !defined(RT_USING_SAL)
#include "lwip/inet.h"
#elif defined(__has_include_next) && __has_include_next(<arpa/inet.h>)
/* every other configuration keeps whatever <arpa/inet.h> it had, but only
 * when there actually is one later on the search path: the toolchain newlib
 * has none, and mbedTLS probes this header with __has_include, so an
 * unconditional #include_next would turn every non-network build into a hard
 * error. */
#include_next <arpa/inet.h>
#endif

#endif /* RT_THREAD_SCONS_INET_H */
