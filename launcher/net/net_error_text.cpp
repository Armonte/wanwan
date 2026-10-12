// See net_error_text.h.
#include "net_error_text.h"

#include "FM2K_Locale.h"

#include "mbedtls/x509.h"

namespace fm2k::net {

const char* NetErrorHint(const NetStatus& st) {
    switch (st.code) {
    case NetError::None:
    case NetError::Aborted:   return "";
    case NetError::Timeout:   return T("net_hint_timeout");
    case NetError::Dns:       return T("net_hint_dns");
    case NetError::Connect:   return T("net_hint_connect");
    case NetError::Closed:
    case NetError::Io:        return T("net_hint_closed");
    case NetError::Proxy:     return T("net_hint_proxy");
    case NetError::TlsVerify:
        if (st.verify & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)) {
            return T("net_hint_tls_clock");
        }
        return T("net_hint_tls_untrusted");
    case NetError::Tls:       return T("net_hint_tls");
    case NetError::BadUrl:
    case NetError::Protocol:
    case NetError::TooLarge:  return T("net_hint_protocol");
    }
    return "";
}

}  // namespace fm2k::net
