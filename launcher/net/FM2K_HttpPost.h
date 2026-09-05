#pragma once

// Multipart form POST over WinHTTP.
//
// Extracted from FM2K_UploadQueue.cpp so the replay uploader can use the
// same code rather than a second, drifting copy. The diagnostics uploader
// and the replay uploader talk to the same hub with the same shape --
// a JSON `meta` part plus file parts -- and the parts that are easy to get
// subtly wrong (boundary generation, TLS protocol selection, timeouts,
// chunked reads of the response) should exist once.

#include <cstdint>
#include <string>
#include <vector>

namespace fm2k::http {

struct File {
    std::string filename;   // sent in Content-Disposition
    std::string body;       // raw bytes
};

struct Response {
    int          status     = 0;
    unsigned long last_error = 0;
    const char*  failed_at  = "";
    std::string  body;
};

// POST `files` plus a JSON `meta` part to `url`.
//
// `secret`, when non-empty, is sent as X-FM2K-Log-Secret. Replay uploads
// leave it empty: they are deliberately unauthenticated, so that anyone
// can contribute to a game's replay list.
//
// Blocks the calling thread for the duration, with a WinHTTP timeout, so
// a slow server cannot pin the caller indefinitely. Returns false when
// the request never completed; `out.status` carries the HTTP code when it
// did.
bool PostMultipart(const std::string& url,
                   const std::string& secret,
                   const std::string& meta_json,
                   const std::vector<File>& files,
                   Response& out);

}  // namespace fm2k::http
