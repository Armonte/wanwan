// FM2K Hub client -- WinHTTP WebSocket transport.
//
// One I/O thread does the WS handshake then spawns a sender thread.
// The I/O thread itself owns the receive loop. Both push events
// onto a thread-safe inbox; the launcher's UI thread drains via
// HubClient::Poll() once per frame.
//
// JSON encode/decode is deliberately minimal -- the message catalog
// in docs/FM2K_Matchmaking_Design.md §15.2 is small enough that
// hand-rolled extractors are simpler than vendoring a JSON lib.
// If that catalog grows, swap in nlohmann/json.

// WinHTTP WebSocket APIs (WinHttpWebSocketCompleteUpgrade etc.) are
// gated on _WIN32_WINNT >= 0x0602 (Windows 8). Project-wide setting
// is 0x0601 (Win7) for compatibility; bump only this TU.
#ifdef _WIN32_WINNT
#  undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0602
#ifdef WINVER
#  undef WINVER
#endif
#define WINVER 0x0602

#include "FM2K_HubClient.h"
#include "version_local.h"  // fm2k::kAppVersion
#include <winhttp.h>

#include <SDL3/SDL_log.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

#include "FM2K_HubClient_internal.h"

namespace fm2k {

void HubClient::IoThread(std::string host, uint16_t port,
                         std::string path, std::string nick) {
    auto fail = [&](const char* where) {
        DWORD err = GetLastError();
        // Map the most common WinHTTP failure codes to text the user
        // can act on. "WinHttpSendRequest failed (err=12029)" by
        // itself is opaque -- the user keeps hitting these and asking
        // "what's going wrong?". Spelling out the cause + likely fix
        // surfaces the answer in the UI status_line.
        const char* hint = "";
        switch (err) {
            case 12002: hint = " (timeout -- host unreachable or slow)"; break;
            case 12007: hint = " (name not resolved -- typo in Host field?)"; break;
            case 12029: hint = " (cannot connect -- host reached but TCP refused. "
                               "If hub is local, set Host to 127.0.0.1.)"; break;
            case 12030: hint = " (connection reset -- hub closed unexpectedly)"; break;
            case 12152: hint = " (invalid server response -- wrong protocol on port?)"; break;
            default: break;
        }
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "HubClient: %s failed (err=%lu)%s",
                     where, (unsigned long)err, hint);
        HubEvent ev;
        ev.kind = HubEvent::Kind::Error;
        ev.error = std::string(where) + " (err=" + std::to_string(err) + ")" + hint;
        EmitEvent(std::move(ev));
        ev.kind = HubEvent::Kind::Disconnected;
        EmitEvent(std::move(ev));
        running_.store(false);
        connected_.store(false);
    };

    session_ = WinHttpOpen(L"FM2K-Launcher/0.1",
                           WINHTTP_ACCESS_TYPE_NO_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) { fail("WinHttpOpen"); return; }
    // Win8.0/7: enable TLS 1.2 (WinHTTP defaults to TLS 1.0 there, breaking wss://
    // to a modern hub). No-op on 8.1+. See FM2K_DiscordAuth for the full why.
    { DWORD sp = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
      WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &sp, sizeof(sp)); }

    std::wstring whost = Widen(host);
    conn_ = WinHttpConnect(session_, whost.c_str(), port, 0);
    if (!conn_) { fail("WinHttpConnect"); return; }

    std::wstring wpath = Widen(path.empty() ? "/" : path);
    DWORD req_flags = use_tls_ ? WINHTTP_FLAG_SECURE : 0;
    req_ = WinHttpOpenRequest(conn_, L"GET", wpath.c_str(), nullptr,
                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                              req_flags);
    if (!req_) { fail("WinHttpOpenRequest"); return; }

    // Required to upgrade.
    if (!WinHttpSetOption(req_, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        fail("WinHttpSetOption(UPGRADE)"); return;
    }

    if (!WinHttpSendRequest(req_, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        fail("WinHttpSendRequest"); return;
    }
    if (!WinHttpReceiveResponse(req_, nullptr)) {
        fail("WinHttpReceiveResponse"); return;
    }

    ws_ = WinHttpWebSocketCompleteUpgrade(req_, 0);
    if (!ws_) { fail("WinHttpWebSocketCompleteUpgrade"); return; }

    // Request handle no longer needed once we have the WS handle.
    WinHttpCloseHandle(req_);
    req_ = nullptr;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "HubClient: WebSocket upgraded → %s:%u%s",
                host.c_str(), (unsigned)port, path.c_str());

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
        DWORD r = WinHttpWebSocketSend(ws_,
            WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            hello.data(), (DWORD)hello.size());
        if (r != ERROR_SUCCESS) { fail("WinHttpWebSocketSend(hello)"); return; }
    }

    // Sender side-thread -- drains outbox, sleeps on cv when empty.
    // Each OutMsg carries is_binary; we pick the matching WS buffer
    // type. Same thread for both JSON and binary so MSDN's per-handle
    // serialization guidance holds.
    std::thread sender([this]() {
        while (running_.load()) {
            OutMsg msg;
            {
                std::unique_lock<std::mutex> lk(out_mtx_);
                out_cv_.wait(lk, [&]() { return !outbox_.empty() || !running_.load(); });
                if (!running_.load()) return;
                msg = std::move(outbox_.front());
                outbox_.pop_front();
                // Release this frame's share of the spec-relay byte
                // budget (SendSpecRelayFrame charges it on enqueue).
                if (msg.is_binary) {
                    const size_t n = msg.data.size();
                    spec_relay_queued_bytes_ =
                        (spec_relay_queued_bytes_ > n)
                            ? (spec_relay_queued_bytes_ - n) : 0;
                }
            }
            const WINHTTP_WEB_SOCKET_BUFFER_TYPE bt = msg.is_binary
                ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE
                : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
            DWORD r = WinHttpWebSocketSend(ws_, bt,
                msg.data.data(), (DWORD)msg.data.size());
            if (r != ERROR_SUCCESS) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "HubClient: WS send failed (err=%lu, kind=%s)",
                            (unsigned long)r, msg.is_binary ? "binary" : "json");
                running_.store(false);
                break;
            }
        }
    });

    // Receive loop -- assembles fragmented UTF-8 messages and dispatches.
    // Binary frames (Phase 3 spec hub-relay) get accumulated separately
    // and emitted as a SpecRelayBinary event when the message completes.
    std::string assembly;
    std::vector<uint8_t> binary_assembly;
    BYTE buf[4096];
    while (running_.load()) {
        DWORD bytes = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bt;
        DWORD r = WinHttpWebSocketReceive(ws_, buf, sizeof(buf), &bytes, &bt);
        if (r != ERROR_SUCCESS) break;

        if (bt == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;

        if (bt == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            bt == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
            assembly.append(reinterpret_cast<char*>(buf), bytes);
            if (bt == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                OnMessage(assembly);
                assembly.clear();
            }
        } else if (bt == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                   bt == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
            binary_assembly.insert(binary_assembly.end(), buf, buf + bytes);
            if (bt == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                HubEvent ev;
                ev.kind = HubEvent::Kind::SpecRelayBinary;
                ev.spec_relay_bytes = std::move(binary_assembly);
                EmitEvent(std::move(ev));
                binary_assembly.clear();
            }
        }
    }

    running_.store(false);
    out_cv_.notify_all();
    sender.join();

    HubEvent ev;
    ev.kind = HubEvent::Kind::Disconnected;
    EmitEvent(std::move(ev));
    connected_.store(false);
}

void HubClient::CleanupHandles() {
    if (ws_)      { WinHttpCloseHandle(ws_);      ws_      = nullptr; }
    if (req_)     { WinHttpCloseHandle(req_);     req_     = nullptr; }
    if (conn_)    { WinHttpCloseHandle(conn_);    conn_    = nullptr; }
    if (session_) { WinHttpCloseHandle(session_); session_ = nullptr; }
}

// ----- inbound dispatch -----

}  // namespace fm2k
