#pragma once
// Localised, user-actionable hint for a network failure (T() keys
// net_hint_* in locales/*.ini). Returns "" for success.
#include "net_stream.h"

namespace fm2k::net {

const char* NetErrorHint(const NetStatus& st);

}  // namespace fm2k::net
