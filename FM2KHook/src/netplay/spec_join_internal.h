// Shared internals for the spec_join cluster: spec_join.cpp (host-side accept/
// redirect + JOIN_ACK construction) and spec_join_viewer.cpp (viewer-side
// request/ack/redirect/kick). Split out of spec_join.cpp when it reached the
// 1000-line cap; everything here was previously file-static in that TU.
#pragma once

#include <cstdint>

// kAppVersion "0.M.P" -> (M, P) for the SPEC_JOIN_VERSIONED gate. Parsed once
// and cached; a malformed string (never happens -- make_version.sh stamps it)
// degrades to 0.0, which simply fails the gate closed. Defined in spec_join.cpp
// and used by BOTH TUs: the host reads it to gate inbound JOIN_REQs, the viewer
// stamps it into every outbound one.
void SpecJoin_AppVersionBytes(uint8_t* out_minor, uint8_t* out_patch);
