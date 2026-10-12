#pragma once
// Shared state for the launcher's bundled TLS stack (mbedTLS 3.6): one-time
// library init, the trust-anchor sets, randomness and error text. Used by
// net_stream.cpp; implemented in tls_global.cpp and ca_store.cpp.

#include <cstddef>
#include <cstdint>
#include <string>

#include "mbedtls/x509_crt.h"

namespace fm2k::net::tls {

// Registers the mbedTLS threading callbacks and runs psa_crypto_init().
// Idempotent and thread-safe. False only if PSA could not seed its RNG.
bool GlobalInit();

// Cryptographically secure random bytes (PSA DRBG). Requires GlobalInit().
bool RandomBytes(void* out, size_t len);

enum class Anchors {
    Primary,      // Windows ROOT store (bundle only if the store gave nothing)
    WithBundle,   // ROOT store + bundled Mozilla set (retry after NOT_TRUSTED)
};

// Lazily loads both sets on first use (thread-safe). Never null; an empty
// chain simply fails every verification.
mbedtls_x509_crt* TrustAnchors(Anchors which);

// Process-wide memory of "the store was missing a root we needed": once a
// WithBundle retry succeeds, later connections start with WithBundle.
bool PreferBundle();
void SetPreferBundle();

std::string ErrText(int mbedtls_code);           // "SSL - ... (-0x7780)"
std::string VerifyText(uint32_t verify_flags);   // "certificate has expired; ..."

}  // namespace fm2k::net::tls
