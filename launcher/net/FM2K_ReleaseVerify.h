// FM2K_ReleaseVerify.h -- does this downloaded update actually come from us?
//
// FM2K_Updater fetches fm2k_v<ver>.zip over HTTPS and FM2KUpdater.exe
// extracts it over the user's launcher and BOTH hook DLLs. Until this
// existed, nothing between those two steps looked at what had been
// downloaded. TLS proves we talked to github.com; it proves nothing about
// what the bytes are once a release asset can be replaced, and "the
// updater runs whatever the release page serves" is a very short sentence
// to have to say after the fact.
//
// So: every release from the first signed one on ships fm2k_v<ver>.zip.sig
// beside the zip, and this refuses anything that does not verify against
// the public key compiled into the binary (FM2K_ReleaseKey.h).
//
// NO FALLBACK. A missing .sig, an empty .sig, or a bad signature all fail
// the same way -- there is deliberately no "unsigned releases still work"
// path, because an attacker who can swap the zip can also delete the .sig,
// and a check that can be turned off by the thing it is checking is not a
// check.
#pragma once
#include <string>

namespace fm2k::release {

enum class Verdict {
    Ok,             // signature present and valid for this file
    NoSignature,    // .sig missing or empty
    BadSignature,   // present, did not verify against our key
    ReadError,      // could not read the zip or the signature off disk
    CryptoError,    // CNG refused to set up -- should not happen
};

// Human-readable, already phrased for the update UI. Never says "try
// again": none of these are transient, and telling someone to retry a
// tampered download is the wrong advice.
const char* Explain(Verdict v);

// Verify `zip_path` against `sig_path` using the compiled-in public key.
// Both paths are read whole; a release zip is a few MB.
Verdict VerifyZip(const std::string& zip_path, const std::string& sig_path);

}  // namespace fm2k::release
