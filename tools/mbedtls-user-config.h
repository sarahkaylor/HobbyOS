/*
 * HobbyOS WebKit fork — mbedTLS user config (WK-4b / D-7).
 *
 * Included at the end of mbedtls_config.h via -DMBEDTLS_USER_CONFIG_FILE,
 * so the trims/additions below adjust the defaults for the bare-metal
 * sysroot:
 *
 *   MBEDTLS_NET_C                — mbedTLS' own BSD-socket client/server
 *                                  helpers; the curl backend owns all I/O
 *                                  (and the sysroot has no POSIX
 *                                  connect()/getaddrinfo spelled that way).
 *   MBEDTLS_TIMING_C             — timing helper used only by the upstream
 *                                  test programs; nothing in the curl
 *                                  backend calls it.
 *   MBEDTLS_NO_PLATFORM_ENTROPY  — no /dev/urandom on HobbyOS.
 *   MBEDTLS_ENTROPY_HARDWARE_ALT — entropy via mbedtls_hardware_poll()
 *                                  (libc getentropy; see HobbyOS/lib/
 *                                  mbedtls_alt.c).
 *   MBEDTLS_PLATFORM_MS_TIME_ALT — mbedtls_ms_time() via gettimeofday
 *                                  (ditto).
 *
 * Everything else (crypto, x509, TLS 1.2/1.3, PSA, FS_IO, time) compiles
 * against the sysroot: fopen/rename/remove, gmtime_r, time(), gettimeofday
 * are in libc.a.
 */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C

#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
