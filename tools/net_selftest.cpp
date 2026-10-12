// fm2k_net_selftest.exe -- end-to-end check of the launcher's own HTTPS /
// WebSocket stack (launcher/net) on a real Windows box, without the launcher
// UI. Built only on request: `ninja -C build fm2k_net_selftest`.
//
//   fm2k_net_selftest ca                       trust-anchor stats
//   fm2k_net_selftest get <url> [max_bytes]    HTTPS GET, prints status/headers/size
//   fm2k_net_selftest ws <url> [hello_nick]    WSS upgrade, ping/pong, optional hub
//                                              hello (stealth), first reply, close;
//                                              "raw:<text>" sends <text> verbatim
//
// Exit code 0 = every requested step succeeded. Output goes to stdout because
// this is a console test harness, not the launcher (which logs via SDL_Log).

#include "https_client.h"
#include "ws_client.h"
#include "tls/ca_store.h"

#include <SDL3/SDL_log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void LogToStdout(void*, int, SDL_LogPriority prio, const char* msg) {
    std::fprintf(stdout, "  [log %d] %s\n", (int)prio, msg);
    std::fflush(stdout);
}

int CmdCa() {
    const auto st = fm2k::net::tls::GetCaStoreStats();
    std::printf("ROOT store: %d seen, %d parsed\nbundle: %d parsed (Mozilla data as of %s)\n"
                "extra (FM2K_CA_FILE): %d\nprimary set is bundle: %s\n",
                st.store_seen, st.store_parsed, st.bundle_parsed, st.bundle_date.c_str(),
                st.extra_parsed, st.primary_is_bundle ? "yes" : "no");
    return (st.store_parsed > 0 || st.bundle_parsed > 0) ? 0 : 1;
}

int CmdGet(const char* url, size_t max_bytes) {
    fm2k::net::HttpRequest req;
    req.url        = url;
    req.user_agent = "FM2K_Updater/0.2";
    req.headers.push_back({"Accept", "application/vnd.github+json"});
    req.max_body   = max_bytes;
    req.log_tag    = "selftest";
    fm2k::net::HttpResult res;
    const bool ok = fm2k::net::HttpFetch(req, res);
    std::printf("GET %s\n  ok=%d status=%d redirects=%d final=%s body=%zu bytes\n", url, ok,
                res.status, res.redirects, res.final_url.c_str(), res.body.size());
    if (!res.net.ok()) {
        std::printf("  error: %s (legacy err=%lu)\n", res.net.Describe().c_str(), res.last_error);
    }
    for (const auto& h : res.headers) {
        if (fm2k::net::IEquals(h.name, "content-type") || fm2k::net::IEquals(h.name, "content-length") ||
            fm2k::net::IEquals(h.name, "transfer-encoding") || fm2k::net::IEquals(h.name, "server")) {
            std::printf("  %s: %s\n", h.name.c_str(), h.value.c_str());
        }
    }
    std::printf("  body head: %.120s\n", res.body.c_str());
    return (ok && res.status >= 200 && res.status < 400) ? 0 : 1;
}

int CmdWs(const char* url_s, const char* hello_nick) {
    fm2k::net::Url u;
    if (!fm2k::net::ParseUrl(url_s, u)) { std::printf("bad url\n"); return 2; }
    fm2k::net::WsConnectOptions opt;
    opt.host = u.host;
    opt.port = u.port;
    opt.path = u.target;
    opt.tls  = u.tls;
    opt.user_agent = "FM2K-Launcher/selftest";
    opt.log_tag = "selftest";
    fm2k::net::WsClient ws;
    fm2k::net::NetStatus st;
    if (!ws.Connect(opt, st)) {
        std::printf("WS connect FAILED: %s\n", st.Describe().c_str());
        return 1;
    }
    std::printf("WS upgraded: %s %s\n", url_s, ws.tls_info().c_str());
    if (!ws.SendPing(st)) { std::printf("ping send failed: %s\n", st.Describe().c_str()); return 1; }

    int rc = 0;
    if (hello_nick) {
        // "raw:<text>" sends <text> verbatim (local test server commands).
        const bool raw = std::strncmp(hello_nick, "raw:", 4) == 0;
        const std::string hello = raw ? std::string(hello_nick + 4)
            : std::string("{\"type\":\"hello\",\"nick\":\"") + hello_nick +
              "\",\"client_version\":\"selftest\",\"stealth\":true}";
        if (!ws.SendText(hello.data(), hello.size(), st)) {
            std::printf("hello send failed: %s\n", st.Describe().c_str());
            return 1;
        }
        std::printf("sent hello (nick=%s, stealth)\n", hello_nick);
        bool got = false;
        for (int i = 0; i < 50 && !got; ++i) {   // up to ~5 s
            fm2k::net::WsMessage m;
            const auto r = ws.Receive(m, 100, st);
            if (r == fm2k::net::WsClient::Recv::Message) {
                std::string text(m.data.begin(), m.data.end());
                if (text.size() > 300) text = text.substr(0, 300) + "...";
                std::printf("reply (%s, %zu bytes): %s\n", m.binary ? "binary" : "text",
                            m.data.size(), text.c_str());
                got = !raw;   // raw mode keeps reading until the server acts
            } else if (r == fm2k::net::WsClient::Recv::Closed && raw) {
                std::printf("server closed: %s\n", st.Describe().c_str());
                ws.Close();
                return 0;
            } else if (r != fm2k::net::WsClient::Recv::Timeout) {
                std::printf("receive ended: %s\n", st.Describe().c_str());
                rc = 1;
                break;
            }
        }
        if (!got && rc == 0) { std::printf("no reply within 5 s\n"); rc = 1; }
    } else {
        // No hello: print whatever the server sends for a second (answering
        // its pings happens inside Receive).
        for (int i = 0; i < 10; ++i) {
            fm2k::net::WsMessage m;
            const auto r = ws.Receive(m, 100, st);
            if (r == fm2k::net::WsClient::Recv::Message) {
                std::printf("message (%s, %zu bytes): %.60s\n", m.binary ? "binary" : "text",
                            m.data.size(), m.binary ? "" : std::string(m.data.begin(), m.data.end()).c_str());
            } else if (r != fm2k::net::WsClient::Recv::Timeout) {
                std::printf("receive ended: %s\n", st.Describe().c_str());
                rc = 1;
                break;
            }
        }
    }
    ws.SendClose(1000, "selftest done", st);
    // Wait briefly for the server's close echo (close handshake).
    for (int i = 0; i < 20; ++i) {
        fm2k::net::WsMessage m;
        const auto r = ws.Receive(m, 100, st);
        if (r == fm2k::net::WsClient::Recv::Closed) {
            std::printf("close handshake complete (server code %u)\n", (unsigned)ws.close_code());
            break;
        }
        if (r == fm2k::net::WsClient::Recv::Error) {
            std::printf("after close: %s\n", st.Describe().c_str());
            break;
        }
    }
    ws.Close();
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    SDL_SetLogOutputFunction(LogToStdout, nullptr);
    if (argc >= 2 && std::strcmp(argv[1], "ca") == 0) return CmdCa();
    if (argc >= 3 && std::strcmp(argv[1], "get") == 0) {
        return CmdGet(argv[2], argc >= 4 ? (size_t)std::strtoull(argv[3], nullptr, 10)
                                         : 16u * 1024u * 1024u);
    }
    if (argc >= 3 && std::strcmp(argv[1], "ws") == 0) {
        return CmdWs(argv[2], argc >= 4 ? argv[3] : nullptr);
    }
    std::printf("usage: fm2k_net_selftest ca | get <url> [max] | ws <url> [hello_nick]\n");
    return 2;
}
