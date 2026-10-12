// One-time mbedTLS / PSA initialisation and shared helpers. See tls_internal.h.
#include "tls_internal.h"

#include <SDL3/SDL_log.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include "mbedtls/error.h"
#include "mbedtls/threading.h"
#include "mbedtls/x509.h"
#include "psa/crypto.h"

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace fm2k::net::tls {

namespace {

#if defined(_WIN32) && defined(MBEDTLS_THREADING_ALT)
void MutexInit(mbedtls_threading_mutex_t* m) {
    auto* cs = new CRITICAL_SECTION;
    InitializeCriticalSection(cs);
    m->cs = cs;
    m->is_valid = 1;
}
void MutexFree(mbedtls_threading_mutex_t* m) {
    if (!m->is_valid) return;
    auto* cs = static_cast<CRITICAL_SECTION*>(m->cs);
    DeleteCriticalSection(cs);
    delete cs;
    m->cs = nullptr;
    m->is_valid = 0;
}
int MutexLock(mbedtls_threading_mutex_t* m) {
    if (!m->is_valid) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    EnterCriticalSection(static_cast<CRITICAL_SECTION*>(m->cs));
    return 0;
}
int MutexUnlock(mbedtls_threading_mutex_t* m) {
    if (!m->is_valid) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    LeaveCriticalSection(static_cast<CRITICAL_SECTION*>(m->cs));
    return 0;
}
#endif

std::once_flag    g_init_once;
bool              g_init_ok = false;
std::atomic<bool> g_prefer_bundle{false};

void DoInit() {
#if defined(_WIN32) && defined(MBEDTLS_THREADING_ALT)
    // Must precede every other mbedTLS call: it also initialises the
    // library's own global mutexes (PSA key slots, RNG, gmtime).
    mbedtls_threading_set_alt(MutexInit, MutexFree, MutexLock, MutexUnlock);
#endif
    const psa_status_t st = psa_crypto_init();
    g_init_ok = (st == PSA_SUCCESS);
    if (!g_init_ok) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "TLS: psa_crypto_init failed (%d) -- HTTPS/WSS unavailable", (int)st);
    }
}

}  // namespace

bool GlobalInit() {
    std::call_once(g_init_once, DoInit);
    return g_init_ok;
}

bool RandomBytes(void* out, size_t len) {
    if (!GlobalInit()) return false;
    return psa_generate_random(static_cast<uint8_t*>(out), len) == PSA_SUCCESS;
}

bool PreferBundle() { return g_prefer_bundle.load(std::memory_order_relaxed); }
void SetPreferBundle() { g_prefer_bundle.store(true, std::memory_order_relaxed); }

std::string ErrText(int code) {
    char buf[160];
    mbedtls_strerror(code, buf, sizeof(buf));
    char tail[24];
    std::snprintf(tail, sizeof(tail), " (-0x%04X)", (unsigned)(code < 0 ? -code : code));
    return std::string(buf) + tail;
}

std::string VerifyText(uint32_t flags) {
    char buf[512];
    const int n = mbedtls_x509_crt_verify_info(buf, sizeof(buf), "", flags);
    std::string s = (n > 0) ? std::string(buf, (size_t)n) : std::string();
    for (char& c : s) {
        if (c == '\n') c = ';';
    }
    while (!s.empty() && (s.back() == ';' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace fm2k::net::tls
