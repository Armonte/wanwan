// FM2K_ReleaseVerify.cpp -- CNG side of the update signature check.
// Contract and rationale live in FM2K_ReleaseVerify.h.
#include "FM2K_ReleaseVerify.h"
#include "FM2K_ReleaseKey.h"

#include <windows.h>
#include <bcrypt.h>

#include <fstream>
#include <vector>

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

namespace fm2k::release {
namespace {

bool ReadWhole(const std::string& path, std::vector<unsigned char>* out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    out->resize(static_cast<size_t>(n));
    if (n == 0) return true;
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out->data()), n);
    return static_cast<bool>(f);
}

// SHA-256 the buffer with CNG. Separate from the verify step so a hash
// failure cannot be mistaken for a signature mismatch -- those two mean
// very different things to whoever reads the log.
bool Sha256(const std::vector<unsigned char>& data, unsigned char out[32]) {
    // Create/HashData/FinishHash rather than the one-shot BCryptHash:
    // mingw-w64's bcrypt.h does not declare the latter, and this sequence
    // is available on every Windows version we target.
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)
            != STATUS_SUCCESS) {
        return false;
    }
    BCRYPT_HASH_HANDLE h = nullptr;
    NTSTATUS st = BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    if (st == STATUS_SUCCESS) {
        st = BCryptHashData(h, const_cast<PUCHAR>(data.data()),
                            static_cast<ULONG>(data.size()), 0);
    }
    if (st == STATUS_SUCCESS) {
        st = BCryptFinishHash(h, out, 32, 0);
    }
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return st == STATUS_SUCCESS;
}

}  // namespace

const char* Explain(Verdict v) {
    switch (v) {
        case Verdict::Ok:           return "signature verified";
        case Verdict::NoSignature:  return "update is not signed -- refusing to install it";
        case Verdict::BadSignature: return "update signature does not match -- refusing to install it";
        case Verdict::ReadError:    return "could not read the downloaded update";
        case Verdict::CryptoError:  return "could not start signature checking on this system";
    }
    return "unknown verification result";
}

Verdict VerifyZip(const std::string& zip_path, const std::string& sig_path) {
    std::vector<unsigned char> zip, sig;
    if (!ReadWhole(zip_path, &zip)) return Verdict::ReadError;
    if (!ReadWhole(sig_path, &sig)) return Verdict::NoSignature;
    if (sig.empty())               return Verdict::NoSignature;

    unsigned char digest[32];
    if (!Sha256(zip, digest)) return Verdict::CryptoError;

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0)
            != STATUS_SUCCESS) {
        return Verdict::CryptoError;
    }

    BCRYPT_KEY_HANDLE key = nullptr;
    NTSTATUS st = BCryptImportKeyPair(
        alg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key,
        const_cast<PUCHAR>(kReleasePublicKeyBlob),
        static_cast<ULONG>(kReleasePublicKeyBlobLen), 0);
    if (st != STATUS_SUCCESS) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return Verdict::CryptoError;
    }

    // PKCS#1 v1.5, matching `openssl dgst -sha256 -sign` in
    // scripts/sign_release.sh. PSS would be the better padding and is what
    // a greenfield design should use, but openssl's default dgst -sign is
    // v1.5 and the two ends agreeing matters more than the margin here.
    BCRYPT_PKCS1_PADDING_INFO pad{};
    pad.pszAlgId = BCRYPT_SHA256_ALGORITHM;

    st = BCryptVerifySignature(key, &pad, digest, 32,
                               sig.data(), static_cast<ULONG>(sig.size()),
                               BCRYPT_PAD_PKCS1);

    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    return st == STATUS_SUCCESS ? Verdict::Ok : Verdict::BadSignature;
}

}  // namespace fm2k::release
