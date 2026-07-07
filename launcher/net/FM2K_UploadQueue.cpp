// Launcher-side processor for hook-generated upload manifests.
// See FM2K_UploadQueue.h for the design.

#include "FM2K_UploadQueue.h"
#include "FM2KHook/src/util/pii_scrub.h"  // fm2k::pii::Scrub — redact OS username etc. from the meta we transmit

#include <SDL3/SDL.h>
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace fm2k::upload_queue {

namespace {

constexpr size_t MAX_FILE_TAIL_BYTES = 2 * 1024 * 1024;   // 2 MB
constexpr size_t MAX_BODY_BYTES      = 48 * 1024 * 1024;  // 48 MB (server caps 50, leave headroom)
constexpr int    HTTP_TIMEOUT_MS     = 15000;

// ---------- minimal JSON scanner ----------
//
// The manifest is produced by our own hook code (upload_queue.cpp), so
// the format is predictable: top-level object with string/number scalars
// and a "files" array of strings. We don't need a real JSON parser;
// just lift fields by key with cheap substring matching. Anything that
// doesn't match the format gets quarantined.
//
// All Find* helpers return false on missing/malformed values so the
// caller can quarantine the manifest.

bool FindStringField(const std::string& text, const std::string& key,
                     std::string& out) {
    const std::string needle = "\"" + key + "\":";
    size_t p = text.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    // Skip whitespace.
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
    if (p >= text.size() || text[p] != '"') return false;
    ++p;  // past opening quote
    std::string val;
    val.reserve(128);
    while (p < text.size() && text[p] != '"') {
        if (text[p] == '\\' && p + 1 < text.size()) {
            char esc = text[p + 1];
            switch (esc) {
                case '"':  val += '"';  break;
                case '\\': val += '\\'; break;
                case 'n':  val += '\n'; break;
                case 'r':  val += '\r'; break;
                case 't':  val += '\t'; break;
                default:
                    val += esc;  // best-effort
                    break;
            }
            p += 2;
        } else {
            val += text[p++];
        }
    }
    if (p >= text.size()) return false;
    out = std::move(val);
    return true;
}

bool FindNumberField(const std::string& text, const std::string& key,
                     std::string& out) {
    const std::string needle = "\"" + key + "\":";
    size_t p = text.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
    std::string val;
    while (p < text.size() && (text[p] == '-' || (text[p] >= '0' && text[p] <= '9'))) {
        val += text[p++];
    }
    if (val.empty()) return false;
    out = val;
    return true;
}

bool FindStringArray(const std::string& text, const std::string& key,
                     std::vector<std::string>& out) {
    const std::string needle = "\"" + key + "\":";
    size_t p = text.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t' ||
                               text[p] == '\n' || text[p] == '\r')) ++p;
    if (p >= text.size() || text[p] != '[') return false;
    ++p;
    while (p < text.size()) {
        while (p < text.size() && (text[p] == ' ' || text[p] == '\t' ||
                                   text[p] == '\n' || text[p] == '\r' ||
                                   text[p] == ',')) ++p;
        if (p >= text.size()) return false;
        if (text[p] == ']') return true;
        if (text[p] != '"') return false;
        ++p;
        std::string val;
        while (p < text.size() && text[p] != '"') {
            if (text[p] == '\\' && p + 1 < text.size()) {
                char esc = text[p + 1];
                switch (esc) {
                    case '"':  val += '"';  break;
                    case '\\': val += '\\'; break;
                    case 'n':  val += '\n'; break;
                    case 'r':  val += '\r'; break;
                    case 't':  val += '\t'; break;
                    default:   val += esc;  break;
                }
                p += 2;
            } else {
                val += text[p++];
            }
        }
        if (p >= text.size()) return false;
        ++p;  // past closing quote
        out.push_back(std::move(val));
    }
    return false;
}

// ---------- file I/O ----------

bool ReadEntireFile(const fs::path& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    if (sz < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize((size_t)sz);
    if (sz > 0) f.read(out.data(), sz);
    return (bool)f;
}

// Read at most max_bytes from the END of the file. For debug logs we
// only care about the tail. Returns "" on missing file (so the upload
// goes through with an empty placeholder rather than failing the
// whole manifest because one file was deleted).
std::string ReadFileTail(const fs::path& path, size_t max_bytes) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    if (sz <= 0) return {};
    std::streamoff start = sz > (std::streamoff)max_bytes
                           ? sz - (std::streamoff)max_bytes : 0;
    f.seekg(start);
    std::string out;
    out.resize((size_t)(sz - start));
    f.read(out.data(), out.size());
    if (start > 0) {
        // Mark truncation so the dev pulling the log knows the prefix
        // was elided. Keeps the LLM/grepper from concluding the log
        // started here.
        std::string prefix = "[…truncated " +
            std::to_string((unsigned long long)start) +
            " bytes from head, last " +
            std::to_string((unsigned long long)max_bytes) +
            " kept…]\n";
        out.insert(0, prefix);
    }
    return out;
}

// ---------- WinHTTP multipart POST ----------

struct HttpResp {
    int   status     = 0;
    DWORD last_error = 0;
    const char* failed_at = "";
    std::string body;
};

bool ParseUrl(const std::string& url, bool& https_out, std::wstring& host_out,
              uint16_t& port_out, std::wstring& path_out) {
    https_out = false;
    std::string s = url;
    if (s.compare(0, 8, "https://") == 0) { https_out = true; s = s.substr(8); }
    else if (s.compare(0, 7, "http://") == 0) { s = s.substr(7); }
    else return false;
    size_t slash = s.find('/');
    std::string hostport = (slash == std::string::npos) ? s : s.substr(0, slash);
    std::string path     = (slash == std::string::npos) ? "/" : s.substr(slash);
    size_t colon = hostport.find(':');
    std::string host = hostport;
    uint16_t port = https_out ? 443 : 80;
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = (uint16_t)std::atoi(hostport.substr(colon + 1).c_str());
    }
    host_out.assign(host.begin(), host.end());
    path_out.assign(path.begin(), path.end());
    port_out = port;
    return true;
}

std::string MakeBoundary() {
    // 16 bytes of randomness, hex-encoded. boundary must not appear
    // anywhere in the multipart body — random 32-hex collision odds
    // are negligible for our payload sizes.
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    char buf[33] = {};
    uint64_t a = rng(), b = rng();
    std::snprintf(buf, sizeof(buf),
                  "%08x%08x%08x%08x",
                  (unsigned)(a >> 32), (unsigned)(a & 0xFFFFFFFFu),
                  (unsigned)(b >> 32), (unsigned)(b & 0xFFFFFFFFu));
    return std::string("FM2KUpload-") + buf;
}

struct UploadFile {
    std::string filename;   // sent in Content-Disposition
    std::string body;       // raw bytes
};

bool HttpPostMultipart(const std::string& url,
                       const std::string& secret,
                       const std::string& meta_json,
                       const std::vector<UploadFile>& files,
                       HttpResp& out) {
    bool https = false;
    std::wstring host, path;
    uint16_t port = 0;
    if (!ParseUrl(url, https, host, port, path)) {
        out.failed_at = "ParseUrl";
        return false;
    }

    const std::string boundary = MakeBoundary();
    std::string body;
    body.reserve(meta_json.size() + 4096);
    for (const auto& f : files) body.reserve(body.size() + f.body.size() + 256);

    auto append = [&body](const std::string& s) { body.append(s); };

    // meta part
    append("--"); append(boundary); append("\r\n");
    append("Content-Disposition: form-data; name=\"meta\"\r\n\r\n");
    append(meta_json);
    append("\r\n");

    // file parts
    for (const auto& f : files) {
        append("--"); append(boundary); append("\r\n");
        append("Content-Disposition: form-data; name=\"files\"; filename=\"");
        append(f.filename);
        append("\"\r\n");
        append("Content-Type: application/octet-stream\r\n\r\n");
        body.append(f.body);
        append("\r\n");
    }
    append("--"); append(boundary); append("--\r\n");

    if (body.size() > MAX_BODY_BYTES) {
        out.failed_at = "body too large";
        return false;
    }

    HINTERNET hSes = WinHttpOpen(L"FM2K_Rollback/log-uploader",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,  // not AUTOMATIC: that needs Win8.1+ (WinHttpOpen -> 87 on Win8.0); DEFAULT works on every OS + honors system proxy
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) { out.last_error = GetLastError(); out.failed_at = "WinHttpOpen"; return false; }
    // Win8.0/7: enable TLS 1.2 (WinHTTP defaults to TLS 1.0 there). No-op on 8.1+.
    { DWORD sp = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
      WinHttpSetOption(hSes, WINHTTP_OPTION_SECURE_PROTOCOLS, &sp, sizeof(sp)); }
    WinHttpSetTimeouts(hSes, HTTP_TIMEOUT_MS, HTTP_TIMEOUT_MS, HTTP_TIMEOUT_MS, HTTP_TIMEOUT_MS);

    HINTERNET hCon = WinHttpConnect(hSes, host.c_str(), port, 0);
    if (!hCon) {
        out.last_error = GetLastError(); out.failed_at = "WinHttpConnect";
        WinHttpCloseHandle(hSes);
        return false;
    }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hCon, L"POST", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        out.last_error = GetLastError(); out.failed_at = "WinHttpOpenRequest";
        WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes);
        return false;
    }

    // SECURITY: X-FM2K-Log-Secret is a SHARED secret compiled into every
    // client (kLogUploadSecret) — it is OBFUSCATION, NOT authentication. Any
    // user can extract it and forge/flood/poison the endpoint. The hub log-
    // ingest MUST therefore: (1) rate-limit + size-cap per source IP, (2) treat
    // every upload as untrusted (never index/execute its contents), and
    // (3) ideally issue a per-session upload token bound to the WS-authed
    // session instead of relying on this. Uploaded logs also carry match
    // tokens + peer IPs, so the secret leaking compounds the relay/STUN
    // findings — scrub or gate those server-side.
    std::wstring headers = L"Content-Type: multipart/form-data; boundary=";
    headers.append(boundary.begin(), boundary.end());
    headers += L"\r\nX-FM2K-Log-Secret: ";
    headers.append(secret.begin(), secret.end());
    headers += L"\r\n";

    BOOL ok = WinHttpSendRequest(hReq,
        headers.c_str(), (DWORD)headers.size(),
        (LPVOID)body.data(), (DWORD)body.size(),
        (DWORD)body.size(), 0);
    if (!ok) {
        out.last_error = GetLastError(); out.failed_at = "WinHttpSendRequest";
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes);
        return false;
    }
    if (!WinHttpReceiveResponse(hReq, nullptr)) {
        out.last_error = GetLastError(); out.failed_at = "WinHttpReceiveResponse";
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes);
        return false;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    out.status = (int)status;

    std::vector<char> respbuf;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
        size_t off = respbuf.size();
        respbuf.resize(off + avail);
        DWORD got = 0;
        if (!WinHttpReadData(hReq, respbuf.data() + off, avail, &got)) break;
        respbuf.resize(off + got);
        if (respbuf.size() > 16 * 1024) break;  // server returns tiny JSON
    }
    out.body.assign(respbuf.begin(), respbuf.end());

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hCon);
    WinHttpCloseHandle(hSes);
    return true;
}

// ---------- main processor ----------

void MoveTo(const fs::path& src, const fs::path& dst_dir) {
    std::error_code ec;
    fs::create_directories(dst_dir, ec);
    fs::path dst = dst_dir / src.filename();
    fs::rename(src, dst, ec);
    if (ec) {
        // Cross-device rename can fail; copy+remove as fallback.
        ec.clear();
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::remove(src, ec);
    }
}

}  // namespace

// Validate that a string is well-formed UTF-8. We reject manifests
// containing invalid UTF-8 in any path field, because passing those
// raw bytes into std::filesystem::path on MinGW can throw — the
// resulting unhandled exception propagates through PollUploadQueue
// (called every render frame from Render) and crashes the launcher
// at startup before the user can do anything. Confirmed culprit:
// hook v0.2.41 wrote manifests using GetModuleFileNameA / GetCurrent-
// DirectoryA, which return Shift-JIS bytes on Japanese-named game
// directories. Those are not valid UTF-8 — the manifest parses but
// std::filesystem then throws. Until v0.2.44+ hooks write proper
// UTF-8, this validation gate stops the bleeding.
static bool IsValidUtf8(const std::string& s) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const unsigned char* end = p + s.size();
    while (p < end) {
        unsigned char c = *p;
        size_t need;
        if (c < 0x80)            { need = 0; }
        else if ((c & 0xE0) == 0xC0) { need = 1; if (c < 0xC2) return false; }
        else if ((c & 0xF0) == 0xE0) { need = 2; }
        else if ((c & 0xF8) == 0xF0) { need = 3; if (c > 0xF4) return false; }
        else                          { return false; }
        ++p;
        for (size_t i = 0; i < need; ++i) {
            if (p >= end) return false;
            if ((*p & 0xC0) != 0x80) return false;
            ++p;
        }
    }
    return true;
}

bool ProcessImpl(const ProcessorConfig& cfg) {
    if (!cfg.enabled) return false;
    if (cfg.upload_url.empty() || cfg.secret.empty()) return false;

    fs::path queue_dir = fs::path(cfg.game_dir) / "upload_queue";
    std::error_code ec;
    if (!fs::is_directory(queue_dir, ec)) return false;

    // Pick the OLDEST manifest. Stable processing order makes the
    // upload trail easier to reason about.
    fs::path target;
    fs::file_time_type oldest = fs::file_time_type::max();
    for (auto& entry : fs::directory_iterator(queue_dir, ec)) {
        if (ec) return false;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        auto t = entry.last_write_time(ec);
        if (ec) continue;
        if (t < oldest) {
            oldest = t;
            target = entry.path();
        }
    }
    if (target.empty()) return false;

    std::string manifest_text;
    if (!ReadEntireFile(target, manifest_text)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "UploadQueue: failed to read manifest %s — quarantining",
            target.string().c_str());
        MoveTo(target, queue_dir / "quarantine");
        return true;
    }

    std::vector<std::string> file_paths;
    if (!FindStringArray(manifest_text, "files", file_paths)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "UploadQueue: manifest %s has no 'files' array — quarantining",
            target.string().c_str());
        MoveTo(target, queue_dir / "quarantine");
        return true;
    }

    // Minimal field validation. We're permissive — the server has its
    // own validation, and we don't want a client-side schema drift to
    // black-hole legitimate reports. We just need kind+session_id+
    // player_index for filename construction on the receiving end.
    std::string kind, sid, pidx;
    if (!FindStringField(manifest_text, "kind", kind) ||
        !FindStringField(manifest_text, "session_id", sid) ||
        !FindNumberField(manifest_text, "player_index", pidx)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "UploadQueue: manifest %s missing required fields — quarantining",
            target.string().c_str());
        MoveTo(target, queue_dir / "quarantine");
        return true;
    }

    // Validate every path is well-formed UTF-8 BEFORE building any
    // fs::path — bad bytes will throw on MinGW (see IsValidUtf8 comment).
    // Quarantine the whole manifest if any path is bad; we can't trust
    // any of them.
    for (const auto& fp : file_paths) {
        if (!IsValidUtf8(fp)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "UploadQueue: manifest %s contains non-UTF-8 path (size=%zu) "
                "— hook predates v0.2.44 UTF-8 manifest fix, quarantining",
                target.filename().string().c_str(), fp.size());
            MoveTo(target, queue_dir / "quarantine");
            return true;
        }
    }

    std::vector<UploadFile> attachments;
    size_t total = 0;
    for (const auto& fp : file_paths) {
        fs::path p(fp);
        std::string base = p.filename().string();
        if (base.empty()) continue;
        // Server's filename allow-list is strict — strip anything
        // non-[A-Za-z0-9_-.]. Names from the hook are already safe
        // (FM2K_PN_Debug.log etc.) but be defensive.
        for (char& c : base) {
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
                c = '_';
            }
        }
        std::string body;
        if (base.find("Debug.log") != std::string::npos) {
            body = ReadFileTail(p, MAX_FILE_TAIL_BYTES);
        } else if (!ReadEntireFile(p, body)) {
            // Missing referenced file isn't fatal — just log and skip.
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "UploadQueue: referenced file missing, skipping: %s",
                fp.c_str());
            continue;
        }
        total += body.size();
        if (total > MAX_BODY_BYTES) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "UploadQueue: bundle exceeded cap (%zu bytes) — truncating",
                total);
            break;
        }
        attachments.push_back({std::move(base), std::move(body)});
    }

    // Scrub PII from the META we transmit (OS username in the "files" path,
    // public IPs, emails, etc.). file_paths were already resolved from the
    // un-scrubbed manifest_text above and the attachments are already loaded,
    // so this affects ONLY what's sent to the server, never local file lookup.
    // Log *contents* in the zip are already scrubbed at write time by the hook;
    // this closes the last leak (the absolute path in the manifest) so we can
    // safely default auto-upload ON for the community.
    const std::string meta_to_send = fm2k::pii::Scrub(manifest_text);

    HttpResp resp;
    bool sent = HttpPostMultipart(cfg.upload_url, cfg.secret, meta_to_send,
                                  attachments, resp);

    if (sent && resp.status == 200) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "UploadQueue: uploaded %s (%zu files, kind=%s sid=%s) → %s",
            target.filename().string().c_str(),
            attachments.size(), kind.c_str(), sid.c_str(),
            resp.body.c_str());
        MoveTo(target, queue_dir / "done");
        return true;
    }

    if (sent && resp.status >= 400 && resp.status < 500) {
        // Client-side error from the server — schema mismatch, bad
        // filename, etc. Don't retry forever; quarantine so we keep
        // moving on subsequent manifests.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "UploadQueue: server rejected %s (status=%d body=%s) — quarantining",
            target.filename().string().c_str(), resp.status,
            resp.body.c_str());
        MoveTo(target, queue_dir / "quarantine");
        return true;
    }

    // Transient failure (timeout, 5xx, network down). Leave the
    // manifest in place; next Process() call will retry.
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
        "UploadQueue: transient failure %s (status=%d failed_at=%s err=%lu) — will retry",
        target.filename().string().c_str(), resp.status,
        resp.failed_at, (unsigned long)resp.last_error);
    return false;
}

bool Process(const ProcessorConfig& cfg) {
    // PollUploadQueue runs every render frame, so any uncaught throw
    // from inside ProcessImpl (std::filesystem ctor on bad UTF-8, IO
    // error during quarantine move, malformed manifest, etc.) would
    // propagate out of Render and abort the launcher BEFORE the user
    // can interact. Ianthina (v0.2.41 → Higurashi vs Touhou Universe 2,
    // Japanese-named game folder, Shift-JIS bytes in a desync manifest)
    // hit exactly this — launcher unusable until the manifest was
    // deleted by hand.
    //
    // We catch everything here and just return false (= "no progress
    // this tick"). The bad manifest stays on disk for the next tick to
    // re-try, but ProcessImpl's UTF-8 validation will see it and
    // quarantine it on first run, so we don't loop forever.
    try {
        return ProcessImpl(cfg);
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "UploadQueue: ProcessImpl threw std::exception (%s) — "
            "swallowing so launcher stays alive; manifest will be "
            "quarantined on next attempt",
            e.what());
        return false;
    } catch (...) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "UploadQueue: ProcessImpl threw unknown exception — "
            "swallowing so launcher stays alive");
        return false;
    }
}

}  // namespace fm2k::upload_queue
