// FM2K Hub client -- WebSocket transport.
//
// One I/O thread does the WS handshake then spawns a sender thread.
// The I/O thread itself owns the receive loop. Both push events
// onto a thread-safe inbox; the launcher's UI thread drains via
// HubClient::Poll() once per frame.
//
// The socket is our own RFC 6455 client over bundled TLS (launcher/net/
// ws_client.h), identical on every Windows version. It replaced the WinHTTP
// WebSocket API, which needs Windows 8 and stopped the launcher starting on
// Windows 7. Behaviour kept from the WinHTTP version: direct connection (no
// proxy), a ping every 30 s while idle (WinHTTP's default keep-alive), the
// same error/Disconnected event sequence, and no automatic reconnect here
// (the UI layer decides when to reconnect).

#include "FM2K_HubClient.h"
#include "version_local.h"  // fm2k::kAppVersion
#include "net_error_text.h"
#include "ws_client.h"

#include <SDL3/SDL_log.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "FM2K_HubClient_internal.h"

namespace fm2k {

void HubClient::IoThread(std::string host, uint16_t port,
                         std::string path, std::string nick) {
    net::WsClient& ws = *ws_;
    auto fail = [&](const char* where, const net::NetStatus& st) {
        // Spell out the cause + likely fix: "cannot connect" alone is
        // opaque, and users read this straight off the hub status line.
        // err= keeps the WinHTTP-era number so old support notes still match.
        const char* hint = net::NetErrorHint(st);
        const unsigned long err = net::LegacyWinHttpCode(st.code);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "HubClient: %s failed (err=%lu) %s%s%s",
                     where, err, st.Describe().c_str(), *hint ? " -- " : "", hint);
        HubEvent ev;
        ev.kind = HubEvent::Kind::Error;
        ev.error = std::string(where) + " (err=" + std::to_string(err) + ")" +
                   (*hint ? std::string(" (") + hint + ")" : std::string());
        EmitEvent(std::move(ev));
        ev.kind = HubEvent::Kind::Disconnected;
        EmitEvent(std::move(ev));
        running_.store(false);
        connected_.store(false);
        ws.Close();
    };

    net::WsConnectOptions opt;
    opt.host       = host;
    opt.port       = port;
    opt.path       = path.empty() ? "/" : path;
    opt.tls        = use_tls_;
    opt.timeout_ms = 15000;
    opt.user_agent = std::string("FM2K-Launcher/") + fm2k::kAppVersion;
    opt.log_tag    = "HubClient";
    opt.should_abort = [this]() { return !running_.load(); };
    {
        net::NetStatus st;
        if (!ws.Connect(opt, st)) {
            if (st.code == net::NetError::Aborted) {   // Disconnect() during connect
                HubEvent ev;
                ev.kind = HubEvent::Kind::Disconnected;
                EmitEvent(std::move(ev));
                connected_.store(false);
                return;
            }
            fail("WebSocket connect", st);
            return;
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "HubClient: WebSocket upgraded -> %s:%u%s %s",
                host.c_str(), (unsigned)port, path.c_str(), ws.tls_info().c_str());

    // Send hello first. Includes hub_token when the user has signed
    // in with Discord; the hub uses it to validate against the patron
    // role list before accepting the connection.
    {
        std::string hello = "{\"type\":\"hello\",\"nick\":\""
                            + EscapeJsonString(nick) + "\"";
        // client_version -- sent so hub-side logs (connection, match,
        // spectate_request) can print "v0.2.37" alongside the user
        // and operators can spot version-mismatch bugs at a glance
        // (e.g. cross-NAT spec failing because the host they're
        // spec'ing is on a build that doesn't fire the TCP punch).
        // Sourced from version_local.h's kAppVersion (auto-stamped
        // by scripts/make_version.sh per release).
        hello += ",\"client_version\":\"" + EscapeJsonString(fm2k::kAppVersion) + "\"";
        if (!hub_token_.empty()) {
            hello += ",\"hub_token\":\"" + EscapeJsonString(hub_token_) + "\"";
        }
        // Dev-only: FM2K_DEV_USER_SUFFIX lets a single Discord user run
        // multiple launcher instances with distinct lobby entries (for
        // testing). Hub honors this only for accounts on the
        // HUB_DEV_USER_IDS allowlist; everyone else's suffix is
        // ignored. Set differently per-launcher (e.g. =a in client A,
        // =b in client B) to see both in the lobby. Match records still
        // strip the suffix back to the bare dc_id, so stats aggregate
        // correctly regardless of how many dev launchers were involved.
        if (const char* dev_suffix = std::getenv("FM2K_DEV_USER_SUFFIX");
            dev_suffix && dev_suffix[0]) {
            hello += ",\"dev_suffix\":\"" + EscapeJsonString(dev_suffix) + "\"";
        }
        // FM2K_SPEC_TRANSPORT -- the host's spec data plane:
        //   "direct" (default; omitted) -- the reliable channel over UDP,
        //            on the same socket everything else already uses.
        //   "relay"  -- route spec data through hub WS binary frames.
        //            Works on every NAT class.
        // "tcp" is still ACCEPTED here and still means not-relay: the hub's
        // stored default for a client that never advertised is the string
        // "tcp", so refusing it would silently drop the field for every
        // pre-existing user record. The TCP spec data plane itself is gone.
        // Hub stores on User.spec_transport and forwards it in
        // spectate_grant so the viewer's launcher knows which plane to put
        // its spectator on.
        if (const char* transport = std::getenv("FM2K_SPEC_TRANSPORT");
            transport && (std::strcmp(transport, "relay") == 0 ||
                          std::strcmp(transport, "direct") == 0 ||
                          std::strcmp(transport, "tcp") == 0)) {
            hello += ",\"spec_transport\":\"" + std::string(transport) + "\"";
        }
        // Stealth / "ghost" mode for testers on an unreleased build (e.g. a
        // secret character). The hub then presents this user as idle and keeps
        // their match + characters out of the lobby and out of public stats so
        // the build doesn't leak. They stay challengeable (visible as idle) so
        // testers can still reach each other. Hub side: User.stealth in hub.py.
        // Driven by the launcher's Stealth checkbox via SetStealth(); env
        // FM2K_STEALTH=1 also forces it on (dev/CI convenience).
        bool stealth_on = stealth_.load();
        if (!stealth_on) {
            const char* env = std::getenv("FM2K_STEALTH");
            stealth_on = (env && env[0] == '1');
        }
        if (stealth_on) {
            hello += ",\"stealth\":true";
        }
        hello += "}";
        net::NetStatus st;
        if (!ws.SendText(hello.data(), hello.size(), st)) { fail("WebSocket send(hello)", st); return; }
    }

    // Sender side-thread -- drains outbox, sleeps on cv when empty.
    // Each OutMsg carries is_binary; we pick the matching frame type.
    // Same thread for both JSON and binary keeps message order on the
    // wire. When idle for kKeepAlive it sends a ping, like WinHTTP's
    // default WebSocket keep-alive, so NATs and proxies keep the flow open.
    std::thread sender([this, &ws]() {
        constexpr auto kKeepAlive = std::chrono::seconds(30);
        while (running_.load()) {
            OutMsg msg;
            bool have = false;
            {
                std::unique_lock<std::mutex> lk(out_mtx_);
                out_cv_.wait_for(lk, kKeepAlive,
                                 [&]() { return !outbox_.empty() || !running_.load(); });
                if (!running_.load()) return;
                if (!outbox_.empty()) {
                    msg = std::move(outbox_.front());
                    outbox_.pop_front();
                    have = true;
                    // Release this frame's share of the spec-relay byte
                    // budget (SendSpecRelayFrame charges it on enqueue).
                    if (msg.is_binary) {
                        const size_t n = msg.data.size();
                        spec_relay_queued_bytes_ =
                            (spec_relay_queued_bytes_ > n)
                                ? (spec_relay_queued_bytes_ - n) : 0;
                    }
                }
            }
            net::NetStatus st;
            const bool ok = !have ? ws.SendPing(st)
                          : msg.is_binary ? ws.SendBinary(msg.data.data(), msg.data.size(), st)
                                          : ws.SendText(msg.data.data(), msg.data.size(), st);
            if (!ok) {
                if (st.code != net::NetError::Aborted) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "HubClient: WS send failed (err=%lu, kind=%s) %s",
                                net::LegacyWinHttpCode(st.code),
                                !have ? "ping" : msg.is_binary ? "binary" : "json",
                                st.Describe().c_str());
                }
                running_.store(false);
                break;
            }
        }
    });

    // Receive loop. The client reassembles fragmented messages and answers
    // pings itself; text messages go to OnMessage, binary ones (spec
    // hub-relay) become SpecRelayBinary events. The 100 ms wait bounds how
    // long Disconnect() waits for this thread.
    bool user_closed = false;
    while (running_.load()) {
        net::WsMessage m;
        net::NetStatus st;
        const auto r = ws.Receive(m, 100, st);
        if (r == net::WsClient::Recv::Timeout) continue;
        if (r == net::WsClient::Recv::Message) {
            if (m.binary) {
                HubEvent ev;
                ev.kind = HubEvent::Kind::SpecRelayBinary;
                ev.spec_relay_bytes = std::move(m.data);
                EmitEvent(std::move(ev));
            } else {
                OnMessage(std::string(m.data.begin(), m.data.end()));
            }
            continue;
        }
        if (st.code == net::NetError::Aborted) { user_closed = true; break; }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "HubClient: WebSocket ended: %s",
                    st.Describe().c_str());
        break;
    }
    if (!running_.load()) user_closed = true;
    if (user_closed) {
        net::NetStatus ignore;
        ws.SendClose(1000, "client disconnect", ignore);   // best effort
    }

    running_.store(false);
    out_cv_.notify_all();
    sender.join();
    ws.Close();

    HubEvent ev;
    ev.kind = HubEvent::Kind::Disconnected;
    EmitEvent(std::move(ev));
    connected_.store(false);
}

void HubClient::CleanupHandles() {
    if (ws_) ws_->Close();
}

// ----- inbound dispatch -----

}  // namespace fm2k
