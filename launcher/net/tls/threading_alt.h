// mbedTLS MBEDTLS_THREADING_ALT mutex type for Windows. mbedTLS includes this
// by name ("threading_alt.h") when fm2k_mbedtls_config.h selects
// MBEDTLS_THREADING_ALT; the implementation is registered with
// mbedtls_threading_set_alt() in tls_global.cpp before psa_crypto_init().
// CRITICAL_SECTION exists on every Windows version we support.
#ifndef FM2K_MBEDTLS_THREADING_ALT_H
#define FM2K_MBEDTLS_THREADING_ALT_H

typedef struct mbedtls_threading_mutex_t {
    void* cs;       // CRITICAL_SECTION*, heap-allocated so this header stays windows.h-free
    char  is_valid;
} mbedtls_threading_mutex_t;

#endif  // FM2K_MBEDTLS_THREADING_ALT_H
