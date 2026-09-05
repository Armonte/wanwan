// host_http.h -- a minimal HTTP GET for the spike.
//
// The community replay list is served over HTTP by the hub, and the
// hub CLIENT this spike links speaks only WebSocket. Rather than widen
// that client's API for one call, this is the smallest possible GET.
#pragma once

#include <string>

namespace hostgui {

// Fetch `url` into `out`. Returns false on any failure; `out` is only
// touched on success. Blocking, so call it off the frame loop.
bool HttpGet(const std::string& url, std::string* out);

}  // namespace hostgui
