#pragma once
// Trust-anchor loading for the launcher's TLS client. See ca_store.cpp.

#include <string>

namespace fm2k::net::tls {

struct CaStoreStats {
    int         store_seen    = 0;  // certificates enumerated in the Windows ROOT store
    int         store_parsed  = 0;  // ...of which mbedTLS accepted
    int         bundle_parsed = 0;  // bundled Mozilla roots accepted
    int         extra_parsed  = 0;  // FM2K_CA_FILE certificates accepted
    bool        primary_is_bundle = false;  // store gave nothing usable
    std::string bundle_date;        // "Mozilla data as of" of the bundle
};

// Forces the anchor sets to load (if not already) and reports what they hold.
CaStoreStats GetCaStoreStats();

}  // namespace fm2k::net::tls
