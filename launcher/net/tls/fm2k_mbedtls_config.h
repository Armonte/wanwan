// mbedTLS 3.6 configuration for the FM2K launcher's HTTPS / WSS client.
//
// Why a bundled TLS stack at all: Windows 7's schannel only speaks TLS 1.2
// after KB3140245 plus a registry edit, never speaks TLS 1.3, and lacks the
// AEAD suites modern servers insist on. WinHTTP's WebSocket API does not
// exist before Windows 8. Bundling mbedTLS gives one TLS code path on every
// Windows version (and later Linux).
//
// Why mbedTLS 3.6 LTS rather than BearSSL: BearSSL has no TLS 1.3 and has not
// had a release since 2018; mbedTLS 3.6 is an LTS branch (supported into
// 2027), Apache-2.0, does TLS 1.2 + 1.3, and builds cleanly for i686 mingw.
//
// Scope: TLS CLIENT only. ECDHE key exchange (P-256, P-384, X25519) with
// RSA or ECDSA certificates, AES-GCM and ChaCha20-Poly1305, SHA-2, X.509
// parsing and chain verification. No server, no PSK, no CBC, no MD5 (SHA-1 only
// to parse old roots, see below),
// no renegotiation, no filesystem, no session tickets.
#ifndef FM2K_MBEDTLS_CONFIG_H
#define FM2K_MBEDTLS_CONFIG_H

// ---- platform ----
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE          // certificate validity periods
#define MBEDTLS_DEPRECATED_REMOVED
#define MBEDTLS_PLATFORM_C              // required on Windows (snprintf shims for old CRTs)
#if defined(_WIN32)
#  define MBEDTLS_THREADING_C
#  define MBEDTLS_THREADING_ALT          // CRITICAL_SECTION, see threading_alt.h
#else
#  define MBEDTLS_THREADING_C
#  define MBEDTLS_THREADING_PTHREAD
#endif

// ---- randomness ----
#define MBEDTLS_ENTROPY_C               // platform source: BCryptGenRandom (Vista+)
#define MBEDTLS_CTR_DRBG_C

// ---- symmetric ----
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C

// ---- hashes ----
#define MBEDTLS_MD_C
// SHA-1 is compiled in ONLY so that SHA-1-self-signed roots parse (DigiCert
// Global Root CA, GlobalSign Root CA and many Windows store roots): a root's
// own signature is never checked, but mbedTLS rejects a certificate whose
// signature OID it cannot map. The default X.509 profile still refuses SHA-1
// anywhere in a verified chain, and TLS signature_algorithms never offer it.
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_HKDF_C                  // TLS 1.3 key schedule (via PSA)

// ---- public key ----
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21               // RSA-PSS, mandatory for TLS 1.3 + RSA certs
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED  // a few roots in the store use P-521
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

// ---- PSA (TLS 1.3 requires it) ----
#define MBEDTLS_PSA_CRYPTO_C

// ---- X.509 ----
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C             // FM2K_CA_FILE override may be PEM

// ---- TLS ----
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE   // middlebox-friendly framing
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE        // required by TLS 1.3 code
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ALPN
#define MBEDTLS_SSL_ALL_ALERT_MESSAGES
#define MBEDTLS_X509_CHECK_KEY_USAGE
#define MBEDTLS_X509_CHECK_EXTENDED_KEY_USAGE

// ---- diagnostics ----
#define MBEDTLS_ERROR_C                 // mbedtls_strerror for the launcher log

#endif  // FM2K_MBEDTLS_CONFIG_H
