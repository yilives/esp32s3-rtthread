/*
 * SPDX-FileCopyrightText: 2026 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * RT-Thread scons build shim for <sdkconfig.h>.
 *
 * The ESP BSPs ship a hand written bsp/ESP/libraries/drivers/sdkconfig.h that
 * was generated for ESP-IDF v5.1.  ESP-IDF v6.1 added Kconfig symbols and
 * reworked the component set, so the v5.1 header no longer describes the tree
 * that is actually compiled here.
 *
 * Rather than duplicating (and having to keep re-duplicating) the generated
 * header, chain to the BSP one and append the v6.1 defaults that the compiled
 * sources need.  This directory is searched first (see the Env.Prepend() in
 * the root SConscript), so #include_next finds the BSP header below.
 */
#ifndef RT_THREAD_SCONS_SDKCONFIG_H
#define RT_THREAD_SCONS_SDKCONFIG_H

/* the BSP's hand written ESP-IDF v5.1 header, searched after this directory */
#include_next <sdkconfig.h>

/* --------------------------------------------------------------------------
 * Symbols new in ESP-IDF v6.1.
 * -------------------------------------------------------------------------- */

/* new in v6.1 (components/freertos/Kconfig): the old CONFIG_FREERTOS_UNICORE
   is kept for compatibility, but the kernel and the porting layer now take the
   core count from this one.  Same default as the Kconfig. */
#ifndef CONFIG_FREERTOS_NUMBER_OF_CORES
#  ifdef CONFIG_FREERTOS_UNICORE
#    define CONFIG_FREERTOS_NUMBER_OF_CORES 1
#  else
#    define CONFIG_FREERTOS_NUMBER_OF_CORES 2
#  endif
#endif

/* new in v6.1 (components/esp_stdio/Kconfig): esp_rom's UART helpers need the
   ROM console port number, which used to be identical to the UART number. */
#ifndef CONFIG_ESP_CONSOLE_ROM_SERIAL_PORT_NUM
#  define CONFIG_ESP_CONSOLE_ROM_SERIAL_PORT_NUM CONFIG_ESP_CONSOLE_UART_NUM
#endif

/* new in v6.1 (components/esp_timer/Kconfig): the timer task and the timer ISR
   can be pinned to a core.  Only CPU0 is possible on the unicore targets the
   scons BSPs build, and CONFIG_FREERTOS_NO_AFFINITY is already defined by the
   BSP header for targets where pinning is not available. */
#ifndef CONFIG_ESP_TIMER_TASK_AFFINITY
#  if defined(CONFIG_FREERTOS_UNICORE) || defined(CONFIG_FREERTOS_NO_AFFINITY)
#    define CONFIG_ESP_TIMER_TASK_AFFINITY CONFIG_FREERTOS_NO_AFFINITY
#  else
#    define CONFIG_ESP_TIMER_TASK_AFFINITY 0
#  endif
#endif
#ifndef CONFIG_ESP_TIMER_ISR_AFFINITY
#  if defined(CONFIG_FREERTOS_UNICORE) || defined(CONFIG_FREERTOS_NO_AFFINITY)
#    define CONFIG_ESP_TIMER_ISR_AFFINITY CONFIG_FREERTOS_NO_AFFINITY
#  else
#    define CONFIG_ESP_TIMER_ISR_AFFINITY 0
#  endif
#endif

/* new in v6.1 (components/esp_system/Kconfig): components/esp_system moved the
   task watchdog behind an abstraction layer (esp_task_wdt_impl_*.c), so
   esp_private/esp_task_wdt.h only declares its API when this is set.  These are
   the Kconfig defaults for a target with a timer group (everything but C2). */
#ifndef CONFIG_ESP_TASK_WDT_EN
#  define CONFIG_ESP_TASK_WDT_EN 1
#endif
#ifndef CONFIG_ESP_TASK_WDT_USE_ESP_TIMER
#  define CONFIG_ESP_TASK_WDT_USE_ESP_TIMER 0
#endif
#ifndef CONFIG_ESP_TASK_WDT_INIT
#  define CONFIG_ESP_TASK_WDT_INIT 1
#endif
#ifndef CONFIG_ESP_TASK_WDT_PANIC
#  define CONFIG_ESP_TASK_WDT_PANIC 0
#endif
#ifndef CONFIG_ESP_TASK_WDT_TIMEOUT_S
#  define CONFIG_ESP_TASK_WDT_TIMEOUT_S 5
#endif
#ifndef CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
#  define CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 1
#endif

/* new in v6.1: mbedTLS 4.x (components/mbedtls/mbedtls) replaced the individual
   MBEDTLS_X509_CRT_PARSE_C / MBEDTLS_X509_CSR_PARSE_C feature macros behind a
   common MBEDTLS_X509_USE_C umbrella; the parse macros still exist but are now
   rejected by mbedtls_check_config.h unless the umbrella is enabled. */
#ifndef CONFIG_MBEDTLS_VER_4_X_SUPPORT
#  define CONFIG_MBEDTLS_VER_4_X_SUPPORT 1
#endif
#ifndef CONFIG_MBEDTLS_SHA1_C
#  define CONFIG_MBEDTLS_SHA1_C 1
#endif
#ifndef CONFIG_MBEDTLS_CIPHER_MODE_CBC
#  define CONFIG_MBEDTLS_CIPHER_MODE_CBC 1
#endif
#ifndef CONFIG_MBEDTLS_CIPHER_MODE_CTR
#  define CONFIG_MBEDTLS_CIPHER_MODE_CTR 1
#endif
#ifndef CONFIG_MBEDTLS_X509_USE_C
#  define CONFIG_MBEDTLS_X509_USE_C 1
#endif
#ifndef CONFIG_MBEDTLS_X509_CRT_PARSE_C
#  define CONFIG_MBEDTLS_X509_CRT_PARSE_C 1
#endif
/* The BSP header still enables MBEDTLS_KEY_EXCHANGE_ECDHE_RSA /
   MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA.  In mbedTLS 4.x those key exchanges are
   checked against the PSA configuration, which esp_config.h derives from the
   generic public-key wrapper options below, so the wrappers they need have to
   be enabled as well. */
#ifndef CONFIG_MBEDTLS_PK_C
#  define CONFIG_MBEDTLS_PK_C 1
#endif
#ifndef CONFIG_MBEDTLS_RSA_C
#  define CONFIG_MBEDTLS_RSA_C 1
#endif
#ifndef CONFIG_MBEDTLS_PKCS1_V15
#  define CONFIG_MBEDTLS_PKCS1_V15 1
#endif
/* ... and the parsers X.509 and the TLS handshake hash are built from. */
#ifndef CONFIG_MBEDTLS_ASN1_PARSE_C
#  define CONFIG_MBEDTLS_ASN1_PARSE_C 1
#endif
#ifndef CONFIG_MBEDTLS_PK_PARSE_C
#  define CONFIG_MBEDTLS_PK_PARSE_C 1
#endif
#ifndef CONFIG_MBEDTLS_PK_WRITE_C
#  define CONFIG_MBEDTLS_PK_WRITE_C 1
#endif
#ifndef CONFIG_MBEDTLS_ASN1_WRITE_C
#  define CONFIG_MBEDTLS_ASN1_WRITE_C 1
#endif
#ifndef CONFIG_MBEDTLS_SHA256_C
#  define CONFIG_MBEDTLS_SHA256_C 1
#endif
/* PEM (and with it every X.509 certificate path) is Base64. */
#ifndef CONFIG_MBEDTLS_BASE64_C
#  define CONFIG_MBEDTLS_BASE64_C 1
#endif
/* Hardware interrupt priority of the AES / bignum helpers.  Only reached when
   the BSP enables CONFIG_MBEDTLS_AES_USE_INTERRUPT /
   CONFIG_MBEDTLS_MPI_USE_INTERRUPT; the Kconfig default is level 0. */
#ifndef CONFIG_MBEDTLS_AES_INTERRUPT_LEVEL
#  define CONFIG_MBEDTLS_AES_INTERRUPT_LEVEL 0
#endif
#ifndef CONFIG_MBEDTLS_MPI_INTERRUPT_LEVEL
#  define CONFIG_MBEDTLS_MPI_INTERRUPT_LEVEL 0
#endif

/* new in v6.1 (components/spi_flash/Kconfig): the flash driver may either run
   its functions from IRAM or put the flash into suspend mode while they run;
   esp_flash_spi_init.c rejects having both disabled. */
#ifndef CONFIG_SPI_FLASH_PLACE_FUNCTIONS_IN_IRAM
#  define CONFIG_SPI_FLASH_PLACE_FUNCTIONS_IN_IRAM 1
#endif

/* new in v6.1 (components/log/Kconfig.level_settings): the choice that decides
   how per-tag log levels are looked up.  components/log/CMakeLists.txt picks
   the linked-list back end for this choice; the array/binary-heap caches are
   only built for LOG_TAG_LEVEL_IMPL_CACHE_ARRAY. */
#ifndef CONFIG_LOG_TAG_LEVEL_IMPL_CACHE_AND_LINKED_LIST
#  define CONFIG_LOG_TAG_LEVEL_IMPL_CACHE_AND_LINKED_LIST 1
#endif

/* --------------------------------------------------------------------------
 * ESP-IDF v6.1 renamed the Wi-Fi Kconfig prefix from ESP32_WIFI_* to
 * ESP_WIFI_*.  The BSP header predates the rename and still defines the old
 * names, so chain every renamed option the compiled sources use over to its
 * old value (or the v6.1 Kconfig default when the BSP header has none).
 * -------------------------------------------------------------------------- */
#if defined(BSP_USING_WIFI) || defined(BSP_USING_BLE)
#ifndef CONFIG_ESP_WIFI_ENABLED
#  define CONFIG_ESP_WIFI_ENABLED 1
#endif
#ifndef CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM
#  ifdef CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM
#    define CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM
#  else
#    define CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM 10
#  endif
#endif
#ifndef CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM
#  ifdef CONFIG_ESP32_WIFI_DYNAMIC_RX_BUFFER_NUM
#    define CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM CONFIG_ESP32_WIFI_DYNAMIC_RX_BUFFER_NUM
#  else
#    define CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM 32
#  endif
#endif
#ifndef CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM
#  ifdef CONFIG_ESP32_WIFI_DYNAMIC_TX_BUFFER_NUM
#    define CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM CONFIG_ESP32_WIFI_DYNAMIC_TX_BUFFER_NUM
#  else
#    define CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM 32
#  endif
#endif
#ifndef CONFIG_ESP_WIFI_TX_BUFFER_TYPE
#  ifdef CONFIG_ESP32_WIFI_TX_BUFFER_TYPE
#    define CONFIG_ESP_WIFI_TX_BUFFER_TYPE CONFIG_ESP32_WIFI_TX_BUFFER_TYPE
#  else
#    define CONFIG_ESP_WIFI_TX_BUFFER_TYPE 1
#  endif
#endif
#ifndef CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUF
#  define CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUF 0
#endif
#ifndef CONFIG_ESP_WIFI_AMPDU_TX_ENABLED
#  ifdef CONFIG_ESP32_WIFI_AMPDU_TX_ENABLED
#    define CONFIG_ESP_WIFI_AMPDU_TX_ENABLED CONFIG_ESP32_WIFI_AMPDU_TX_ENABLED
#  else
#    define CONFIG_ESP_WIFI_AMPDU_TX_ENABLED 1
#  endif
#endif
#ifndef CONFIG_ESP_WIFI_AMPDU_RX_ENABLED
#  ifdef CONFIG_ESP32_WIFI_AMPDU_RX_ENABLED
#    define CONFIG_ESP_WIFI_AMPDU_RX_ENABLED CONFIG_ESP32_WIFI_AMPDU_RX_ENABLED
#  else
#    define CONFIG_ESP_WIFI_AMPDU_RX_ENABLED 1
#  endif
#endif
#ifndef CONFIG_ESP_WIFI_RX_BA_WIN
#  ifdef CONFIG_ESP32_WIFI_RX_BA_WIN
#    define CONFIG_ESP_WIFI_RX_BA_WIN CONFIG_ESP32_WIFI_RX_BA_WIN
#  else
#    define CONFIG_ESP_WIFI_RX_BA_WIN 6
#  endif
#endif
#ifndef CONFIG_ESP_WIFI_NVS_ENABLED
#  ifdef CONFIG_ESP32_WIFI_NVS_ENABLED
#    define CONFIG_ESP_WIFI_NVS_ENABLED CONFIG_ESP32_WIFI_NVS_ENABLED
#  else
#    define CONFIG_ESP_WIFI_NVS_ENABLED 1
#  endif
#endif
#ifndef CONFIG_ESP_WIFI_SOFTAP_BEACON_MAX_LEN
#  ifdef CONFIG_ESP32_WIFI_SOFTAP_BEACON_MAX_LEN
#    define CONFIG_ESP_WIFI_SOFTAP_BEACON_MAX_LEN CONFIG_ESP32_WIFI_SOFTAP_BEACON_MAX_LEN
#  else
#    define CONFIG_ESP_WIFI_SOFTAP_BEACON_MAX_LEN 752
#  endif
#endif
#ifndef CONFIG_ESP_WIFI_MGMT_SBUF_NUM
#  ifdef CONFIG_ESP32_WIFI_MGMT_SBUF_NUM
#    define CONFIG_ESP_WIFI_MGMT_SBUF_NUM CONFIG_ESP32_WIFI_MGMT_SBUF_NUM
#  else
#    define CONFIG_ESP_WIFI_MGMT_SBUF_NUM 32
#  endif
#endif
#ifndef CONFIG_ESP_WIFI_IRAM_OPT
#  ifdef CONFIG_ESP32_WIFI_IRAM_OPT
#    define CONFIG_ESP_WIFI_IRAM_OPT CONFIG_ESP32_WIFI_IRAM_OPT
#  else
#    define CONFIG_ESP_WIFI_IRAM_OPT 1
#  endif
#endif
#ifndef CONFIG_ESP_WIFI_RX_IRAM_OPT
#  ifdef CONFIG_ESP32_WIFI_RX_IRAM_OPT
#    define CONFIG_ESP_WIFI_RX_IRAM_OPT CONFIG_ESP32_WIFI_RX_IRAM_OPT
#  else
#    define CONFIG_ESP_WIFI_RX_IRAM_OPT 1
#  endif
#endif
#ifndef CONFIG_ESP_WIFI_ENABLE_WPA3_SAE
#  ifdef CONFIG_ESP32_WIFI_ENABLE_WPA3_SAE
#    define CONFIG_ESP_WIFI_ENABLE_WPA3_SAE CONFIG_ESP32_WIFI_ENABLE_WPA3_SAE
#  else
#    define CONFIG_ESP_WIFI_ENABLE_WPA3_SAE 1
#  endif
#endif
#ifndef CONFIG_ESP_WIFI_CSI_ENABLED
#  define CONFIG_ESP_WIFI_CSI_ENABLED 0
#endif
#ifndef CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM
#  define CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM 7
#endif
/* v6.1 (components/esp_wifi/Kconfig): the defaults of the Wi-Fi power-save
   minimum / maximum active-time knobs wifi_init.c reads.  The C3 (no WiFi
   power save support in the BSP header) keeps them at the Kconfig defaults. */
#ifndef CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME
#  define CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME 50
#endif
#ifndef CONFIG_ESP_WIFI_SLP_DEFAULT_MAX_ACTIVE_TIME
#  define CONFIG_ESP_WIFI_SLP_DEFAULT_MAX_ACTIVE_TIME 10
#endif
#ifndef CONFIG_ESP_WIFI_SLP_DEFAULT_WAIT_BROADCAST_DATA_TIME
#  define CONFIG_ESP_WIFI_SLP_DEFAULT_WAIT_BROADCAST_DATA_TIME 15
#endif
/* v6.1 (components/lwip/Kconfig): wifi_init.c sizes the tcpip accept box
   from it even in the netif-free RT-Thread build. */
#ifndef CONFIG_LWIP_TCP_ACCEPTMBOX_SIZE
#  define CONFIG_LWIP_TCP_ACCEPTMBOX_SIZE 6
#endif

/* new in v6.1 (components/esp_phy / components/esp_coex Kconfig): the PHY and
   coex sources (and with them the bt controller, which guards its
   esp_phy_init.h / esp_coex_internal.h includes on these) are their own
   components now; both default to enabled whenever the radio stack is. */
#ifndef CONFIG_ESP_PHY_ENABLED
#  define CONFIG_ESP_PHY_ENABLED 1
#endif
#ifndef CONFIG_ESP_COEX_ENABLED
#  define CONFIG_ESP_COEX_ENABLED 1
#endif
/* v6.1 (components/esp_phy/Kconfig): calibration mode, 0 = partial RF cal,
   the Kconfig default. */
#ifndef CONFIG_ESP_PHY_CALIBRATION_MODE
#  define CONFIG_ESP_PHY_CALIBRATION_MODE 0
#endif
#ifndef CONFIG_ESP_PHY_PLL_TRACK_PERIOD_MS
#  define CONFIG_ESP_PHY_PLL_TRACK_PERIOD_MS 1000
#endif

/* v6.1 (components/bt/host/nimble/Kconfig.in) renamed the HCI transport
   buffer options; chain them to the old BSP names (ACL_BUF_COUNT /
   ACL_BUF_SIZE / HCI_EVT_BUF_SIZE / HCI_EVT_HI_BUF_COUNT) or the v6.1
   defaults where the old name is missing. */
#if defined(BSP_USING_BLE)
#ifndef CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT
#  define CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT CONFIG_BT_NIMBLE_ACL_BUF_COUNT
#endif
#ifndef CONFIG_BT_NIMBLE_TRANSPORT_ACL_SIZE
#  define CONFIG_BT_NIMBLE_TRANSPORT_ACL_SIZE CONFIG_BT_NIMBLE_ACL_BUF_SIZE
#endif
#ifndef CONFIG_BT_NIMBLE_TRANSPORT_EVT_SIZE
#  define CONFIG_BT_NIMBLE_TRANSPORT_EVT_SIZE CONFIG_BT_NIMBLE_HCI_EVT_BUF_SIZE
#endif
#ifndef CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT
#  define CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT CONFIG_BT_NIMBLE_HCI_EVT_HI_BUF_COUNT
#endif
#ifndef CONFIG_BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT
#  define CONFIG_BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT CONFIG_BT_NIMBLE_HCI_EVT_LO_BUF_COUNT
#endif
#ifndef CONFIG_BT_NIMBLE_L2CAP_COC_SDU_BUFF_COUNT
#  define CONFIG_BT_NIMBLE_L2CAP_COC_SDU_BUFF_COUNT 1
#endif
#ifndef CONFIG_BT_NIMBLE_EATT_CHAN_NUM
#  define CONFIG_BT_NIMBLE_EATT_CHAN_NUM 0
#endif
#ifndef CONFIG_BT_NIMBLE_ATT_MAX_PREP_ENTRIES
#  define CONFIG_BT_NIMBLE_ATT_MAX_PREP_ENTRIES 64
#endif
/* v6.1 (components/bt/host/nimble/Kconfig.in): GATT client / server default
   to enabled with the central / peripheral roles; the BSP header predates
   them and esp_nimble_cfg.h would compile the whole GATT layer out. */
#ifndef CONFIG_BT_NIMBLE_GATT_CLIENT
#  define CONFIG_BT_NIMBLE_GATT_CLIENT 1
#endif
#ifndef CONFIG_BT_NIMBLE_GATT_SERVER
#  define CONFIG_BT_NIMBLE_GATT_SERVER 1
#endif
#endif /* defined(BSP_USING_BLE) */
#endif /* defined(BSP_USING_WIFI) || defined(BSP_USING_BLE) */

#ifndef CONFIG_LWIP_IPV4
#  define CONFIG_LWIP_IPV4 1
#endif
#ifndef CONFIG_LWIP_IPV6
#  define CONFIG_LWIP_IPV6 1
#endif
#ifndef CONFIG_LWIP_DHCPS
#  define CONFIG_LWIP_DHCPS 1
#endif
#ifndef CONFIG_LWIP_DHCP_DOES_ACD_CHECK
#  define CONFIG_LWIP_DHCP_DOES_ACD_CHECK 1
#endif

#ifndef CONFIG_LWIP_MAX_SOCKETS
#  define CONFIG_LWIP_MAX_SOCKETS 10
#endif
#ifndef CONFIG_LWIP_MAX_RAW_PCBS
#  define CONFIG_LWIP_MAX_RAW_PCBS 16
#endif
#ifndef CONFIG_LWIP_MAX_ACTIVE_TCP
#  define CONFIG_LWIP_MAX_ACTIVE_TCP 16
#endif
#ifndef CONFIG_LWIP_MAX_LISTENING_TCP
#  define CONFIG_LWIP_MAX_LISTENING_TCP 16
#endif
#ifndef CONFIG_LWIP_MAX_UDP_PCBS
#  define CONFIG_LWIP_MAX_UDP_PCBS 16
#endif
#ifndef CONFIG_LWIP_IP_REASS_MAX_PBUFS
#  define CONFIG_LWIP_IP_REASS_MAX_PBUFS 10
#endif
#ifndef CONFIG_LWIP_IP_DEFAULT_TTL
#  define CONFIG_LWIP_IP_DEFAULT_TTL 64
#endif
#ifndef CONFIG_LWIP_DHCP_MAX_NTP_SERVERS
#  define CONFIG_LWIP_DHCP_MAX_NTP_SERVERS 1
#endif
#ifndef CONFIG_LWIP_DHCP_OPTIONS_LEN
#  define CONFIG_LWIP_DHCP_OPTIONS_LEN 69
#endif
#ifndef CONFIG_LWIP_DHCP_DISABLE_VENDOR_CLASS_ID
#  define CONFIG_LWIP_DHCP_DISABLE_VENDOR_CLASS_ID 1
#endif
#ifndef CONFIG_LWIP_DHCP_COARSE_TIMER_SECS
#  define CONFIG_LWIP_DHCP_COARSE_TIMER_SECS 1
#endif
#ifndef CONFIG_LWIP_DNS_MAX_HOST_IP
#  define CONFIG_LWIP_DNS_MAX_HOST_IP 1
#endif
#ifndef CONFIG_LWIP_DNS_MAX_SERVERS
#  define CONFIG_LWIP_DNS_MAX_SERVERS 3
#endif
#ifndef CONFIG_LWIP_TCP_WND_DEFAULT
#  define CONFIG_LWIP_TCP_WND_DEFAULT 5760
#endif
#ifndef CONFIG_LWIP_TCP_MAXRTX
#  define CONFIG_LWIP_TCP_MAXRTX 12
#endif
#ifndef CONFIG_LWIP_TCP_SYNMAXRTX
#  define CONFIG_LWIP_TCP_SYNMAXRTX 6
#endif
#ifndef CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS
#  define CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS 4
#endif
#ifndef CONFIG_LWIP_TCP_OOSEQ_TIMEOUT
#  define CONFIG_LWIP_TCP_OOSEQ_TIMEOUT 6
#endif
#ifndef CONFIG_LWIP_TCP_MSS
#  define CONFIG_LWIP_TCP_MSS 1440
#endif
#ifndef CONFIG_LWIP_TCP_SND_BUF_DEFAULT
#  define CONFIG_LWIP_TCP_SND_BUF_DEFAULT 5760
#endif
#ifndef CONFIG_LWIP_TCP_TMR_INTERVAL
#  define CONFIG_LWIP_TCP_TMR_INTERVAL 250
#endif
#ifndef CONFIG_LWIP_TCP_MSL
#  define CONFIG_LWIP_TCP_MSL 60000
#endif
#ifndef CONFIG_LWIP_TCP_FIN_WAIT_TIMEOUT
#  define CONFIG_LWIP_TCP_FIN_WAIT_TIMEOUT 20000
#endif
#ifndef CONFIG_LWIP_TCP_RTO_TIME
#  define CONFIG_LWIP_TCP_RTO_TIME 3000
#endif
#ifndef CONFIG_LWIP_BRIDGEIF_MAX_PORTS
#  define CONFIG_LWIP_BRIDGEIF_MAX_PORTS 7
#endif
#ifndef CONFIG_LWIP_LOOPBACK_MAX_PBUFS
#  define CONFIG_LWIP_LOOPBACK_MAX_PBUFS 8
#endif
#ifndef CONFIG_LWIP_TCPIP_RECVMBOX_SIZE
#  define CONFIG_LWIP_TCPIP_RECVMBOX_SIZE 32
#endif
/* v6.1 (components/lwip/Kconfig): the TCP/IP task priority, read through
   esp_task.h's ESP_TASK_TCPIP_PRIO -> TCPIP_THREAD_PRIO.  The BSP header
   predates the symbol; keep the Kconfig default. */
#ifndef CONFIG_LWIP_TCPIP_TASK_PRIO
#  define CONFIG_LWIP_TCPIP_TASK_PRIO 18
#endif
/* v6.1 (components/lwip/Kconfig, choice LWIP_HOOK_DHCP_EXTRA_OPTION): the
   generated sdkconfig always defines exactly one arm of the choice; the
   default is NONE.  lwip_default_hooks.c only skips the weak stub / the call
   when NONE is defined, so the BSP header (which predates this choice) has to
   be completed here or the call is compiled without a declaration. */
#ifndef CONFIG_LWIP_HOOK_DHCP_EXTRA_OPTION_NONE
#  define CONFIG_LWIP_HOOK_DHCP_EXTRA_OPTION_NONE 1
#endif
#ifndef CONFIG_LWIP_UDP_RECVMBOX_SIZE
#  define CONFIG_LWIP_UDP_RECVMBOX_SIZE 6
#endif
#ifndef CONFIG_LWIP_TCP_RECVMBOX_SIZE
#  define CONFIG_LWIP_TCP_RECVMBOX_SIZE 6
#endif
#ifndef CONFIG_LWIP_GARP_TMR_INTERVAL
#  define CONFIG_LWIP_GARP_TMR_INTERVAL 60
#endif
#ifndef CONFIG_LWIP_IPV6_NUM_ADDRESSES
#  define CONFIG_LWIP_IPV6_NUM_ADDRESSES 3
#endif
#ifndef CONFIG_LWIP_IPV6_RDNSS_MAX_DNS_SERVERS
#  define CONFIG_LWIP_IPV6_RDNSS_MAX_DNS_SERVERS 0
#endif
#ifndef CONFIG_LWIP_IPV6_MEMP_NUM_ND6_QUEUE
#  define CONFIG_LWIP_IPV6_MEMP_NUM_ND6_QUEUE 3
#endif
#ifndef CONFIG_LWIP_IPV6_ND6_NUM_NEIGHBORS
#  define CONFIG_LWIP_IPV6_ND6_NUM_NEIGHBORS 5
#endif
#ifndef CONFIG_LWIP_IPV6_ND6_NUM_PREFIXES
#  define CONFIG_LWIP_IPV6_ND6_NUM_PREFIXES 5
#endif
#ifndef CONFIG_LWIP_IPV6_ND6_NUM_ROUTERS
#  define CONFIG_LWIP_IPV6_ND6_NUM_ROUTERS 3
#endif
#ifndef CONFIG_LWIP_IPV6_ND6_NUM_DESTINATIONS
#  define CONFIG_LWIP_IPV6_ND6_NUM_DESTINATIONS 10
#endif
#ifndef CONFIG_LWIP_IPV6_DUP_DETECT_ATTEMPTS
#  define CONFIG_LWIP_IPV6_DUP_DETECT_ATTEMPTS 1
#endif
#ifndef CONFIG_LWIP_IPV6_ND6_NUM_ROUTE_INFO
#  define CONFIG_LWIP_IPV6_ND6_NUM_ROUTE_INFO 10
#endif

#endif /* RT_THREAD_SCONS_SDKCONFIG_H */
