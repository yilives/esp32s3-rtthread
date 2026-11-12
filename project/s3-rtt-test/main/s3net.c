/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-04     RT-Thread    first version, ESP32-S3 TCP/DNS check
 *
 * `s3net` - prove the network stack actually moves bytes.
 *
 * Getting an IP from DHCP (which `s3wifi` reports) and being able to talk to
 * another host are two different claims. DHCP only proves that lwIP answered a
 * broadcast on the local segment; it says nothing about routing, ARP, the TCP
 * state machine or whether a socket call can block and wake a thread created
 * through the FreeRTOS shim. This command answers the second claim:
 *
 *   DNS      gethostbyname() through lwIP's netdb      (if a name was given)
 *   TCP      socket + connect to host:port
 *   TX       an HTTP GET, i.e. a write into the stack
 *   RX       the response, i.e. a read that had to wait for the peer
 *   close    so a repeated run does not leak descriptors
 *
 * Every step prints its own result and its own errno, so a failure names the
 * layer that refused rather than reporting "network broken".
 *
 * The default target is the first DNS server lwIP learned from DHCP, if one was
 * configured; otherwise the gateway. Port 80 is used because an AP's
 * administration page is the most likely thing to answer on a captive-ish
 * network where the internet itself may be blocked.
 */

/* MSH_CMD_EXPORT casts the handler to `long (*)(void)`, which is upstream's
 * design and trips -Werror=cast-function-type. */
#pragma GCC diagnostic ignored "-Wcast-function-type"

#include <rtthread.h>

#include "sdkconfig.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"

#define S3_NET_RECV_MAX     512
#define S3_NET_TIMEOUT_MS   5000

static int s3_net_errno_step(const char *what, int rc)
{
    if (rc >= 0)
    {
        rt_kprintf("  %-22s ok (%d)\n", what, rc);
        return 0;
    }

    rt_kprintf("  %-22s FAILED errno=%d (%s)\n", what, errno, strerror(errno));
    return -1;
}

static int cmd_s3net(int argc, char **argv)
{
    struct sockaddr_in addr;
    struct timeval tv;
    char  request[160];
    char *rx;
    const char *host;
    int   port;
    int   sock;
    int   rc;
    int   total = 0;

    host = (argc >= 2) ? argv[1] : RT_NULL;
    port = (argc >= 3) ? atoi(argv[2]) : 80;

    if (port <= 0 || port > 65535)
    {
        rt_kprintf("s3net: port %d is out of range\n", port);
        return -1;
    }

    rt_memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((rt_uint16_t)port);

    /*
     * Resolve. A dotted quad is parsed directly so the common case does not
     * depend on DNS being reachable - which matters on a network where the DHCP
     * lease is fine but the resolver is not.
     */
    if (host != RT_NULL && inet_aton(host, &addr.sin_addr) != 0)
    {
        rt_kprintf("s3net: target %s:%d (literal)\n", host, port);
    }
    else
    {
        struct hostent *he;
        /* Declared here, not beside the casts, so both branches share it. */
        esp_ip4_addr_t gw_addr = { .addr = 0 };

        /*
         * Check for a lease before touching lwIP at all.
         *
         * gethostbyname() on an uninitialised stack asserts deep inside lwIP:
         *
         *   assert failed: tcpip_send_msg_wait_sem
         *     /IDF/components/lwip/lwip/src/api/tcpip.c:454 (Invalid mbox)
         *
         * The guard below this used to cover only the no-argument path (aim at
         * the gateway), so `s3net example.com` before `s3wifi` reset the board
         * while `s3net` alone printed a friendly message. The check has to come
         * first, not inside one branch.
         */
        {
            esp_netif_t *pre = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t pre_ip;

            if (pre == RT_NULL || esp_netif_get_ip_info(pre, &pre_ip) != ESP_OK ||
                pre_ip.ip.addr == 0)
            {
                rt_kprintf("s3net: no IP yet - run `s3wifi <ssid> <pass>` first\n");
                return -1;
            }
        }

        if (host == RT_NULL)
        {
            /* No argument: aim at the gateway, which is the one address that
             * must exist on any working lease. */
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t ip;

            if (netif == RT_NULL || esp_netif_get_ip_info(netif, &ip) != ESP_OK ||
                ip.gw.addr == 0)
            {
                rt_kprintf("s3net: no gateway yet - run `s3wifi <ssid> <pass>` first\n");
                return -1;
            }

            addr.sin_addr.s_addr = ip.gw.addr;
            /*
             * Print from a local esp_ip4_addr_t rather than casting
             * &addr.sin_addr. struct in_addr and esp_ip4_addr_t have the same
             * shape but are different types, and reading one through a pointer
             * to the other is a strict-aliasing violation. It compiled at -Og
             * and was rejected at -O2 with -Werror=strict-aliasing, which is the
             * compiler correctly catching undefined behaviour rather than being
             * pedantic.
             */
            gw_addr.addr = ip.gw.addr;
            rt_kprintf("s3net: target gateway " IPSTR ":%d\n",
                       IP2STR(&gw_addr), port);
        }
        else
        {
            rt_kprintf("s3net: resolving %s ...\n", host);

            he = gethostbyname(host);
            if (he == RT_NULL)
            {
                rt_kprintf("  %-22s FAILED (h_errno=%d)\n", "gethostbyname",
                           h_errno);
                rt_kprintf("s3net: DNS did not answer. Try a literal IP address.\n");
                return -1;
            }

            rt_memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
            rt_kprintf("  %-22s ok -> " IPSTR "\n", "gethostbyname",
                       IP2STR(&gw_addr));
        }
    }

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s3_net_errno_step("socket", sock) != 0)
    {
        return -1;
    }

    /* A receive/send timeout, so a peer that accepts but never answers cannot
     * park this thread forever. Without it the command would look like a hang
     * rather than a failure. */
    tv.tv_sec  = S3_NET_TIMEOUT_MS / 1000;
    tv.tv_usec = (S3_NET_TIMEOUT_MS % 1000) * 1000;
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (s3_net_errno_step("connect", rc) != 0)
    {
        close(sock);
        return -1;
    }

    rt_snprintf(request, sizeof(request),
                "GET / HTTP/1.0\r\n"
                "Host: %s\r\n"
                "User-Agent: rtthread-s3\r\n"
                "Connection: close\r\n"
                "\r\n",
                (host != RT_NULL) ? host : "gateway");

    rc = send(sock, request, rt_strlen(request), 0);
    if (s3_net_errno_step("send", rc) != 0)
    {
        close(sock);
        return -1;
    }

    rx = (char *)rt_malloc(S3_NET_RECV_MAX + 1);
    if (rx == RT_NULL)
    {
        rt_kprintf("s3net: no memory for the receive buffer\n");
        close(sock);
        return -1;
    }

    /* One recv is enough: this proves a read completed, and a peer that answers
     * HTTP answers immediately. Looping would also work but tells us nothing
     * extra about the port. */
    rc = recv(sock, rx, S3_NET_RECV_MAX, 0);
    if (rc < 0)
    {
        rt_kprintf("  %-22s FAILED errno=%d (%s)\n", "recv", errno,
                   strerror(errno));
        rt_kprintf("s3net: TCP connected but the peer sent nothing "
                   "(closed port, or a firewall answering with a reset)\n");
        rt_free(rx);
        close(sock);
        return -1;
    }

    total = rc;
    rx[total] = '\0';
    rt_kprintf("  %-22s ok (%d bytes)\n", "recv", total);

    close(sock);

    /* Print the status line and nothing more: an HTTP response is mostly
     * headers, and the first line is what says whether the peer really spoke
     * HTTP. Everything is replaced with '.' if it is not printable, because
     * this console is UTF-8 and a binary body would corrupt the shell. */
    {
        int i;
        int shown = 0;

        rt_kprintf("\n--- first %d bytes ---\n", total);

        for (i = 0; i < total && shown < 240; i++)
        {
            char c = rx[i];

            rt_kprintf("%c", (c >= 32 && c < 127) || c == '\n' ? c : '.');
            shown++;
        }

        rt_kprintf("\n----------------------\n");
    }

    rt_kprintf("s3net: PASS, %d byte(s) received over TCP\n", total);

    rt_free(rx);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_s3net, s3net, DNS + TCP + HTTP check over the live link);
