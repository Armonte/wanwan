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


namespace fm2k {

// ============================================================
// HubClient
// ============================================================

HubClient::HubClient() = default;

HubClient::~HubClient() {
    Disconnect();
}

bool HubClient::Connect(const std::string& host, uint16_t port,
                        const std::string& path, const std::string& nick,
                        const std::string& hub_token, bool secure) {
    if (running_.load()) return false;  // already connecting / connected
    // A previous failed Connect leaves io_ in a finished-but-joinable
    // state -- IoThread returned, but std::thread doesn't auto-detach.
    // Reassigning over a joinable thread calls std::terminate(); join
    // first to clean up. The thread is already done so this is instant.
    if (io_.joinable()) {
        io_.join();
    }
    // Stash the token + secure flag for IoThread to read. Member state
    // because std::thread argument forwarding caps at 4 here without
    // pulling in another tuple wrapper.
    hub_token_  = hub_token;
    use_tls_    = secure;
    running_.store(true);
    io_ = std::thread(&HubClient::IoThread, this, host, port, path, nick);
    return true;
}

void HubClient::Disconnect() {
    if (!running_.exchange(false)) return;
    out_cv_.notify_all();
    if (ws_ != nullptr) {
        // Closing the handle interrupts WinHttpWebSocketReceive in the IO thread.
        WinHttpCloseHandle(ws_);
        ws_ = nullptr;
    }
    if (io_.joinable()) io_.join();
    CleanupHandles();
    connected_.store(false);
}

void HubClient::Poll(const std::function<void(const HubEvent&)>& on_event) {
    std::deque<HubEvent> drained;
    {
        std::lock_guard<std::mutex> lk(in_mtx_);
        drained.swap(inbox_);
    }
    for (auto& ev : drained) on_event(ev);
}

void HubClient::EnqueueOut(std::string msg) {
    OutMsg out{};
    out.is_binary = false;
    out.data.assign(msg.begin(), msg.end());
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        outbox_.push_back(std::move(out));
    }
    out_cv_.notify_one();
}

void HubClient::SendSpecRelayFrame(std::vector<uint8_t> frame) {
    OutMsg out{};
    out.is_binary = true;
    out.data      = std::move(frame);
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        outbox_.push_back(std::move(out));
    }
    out_cv_.notify_one();
}

void HubClient::EmitEvent(HubEvent ev) {
    std::lock_guard<std::mutex> lk(in_mtx_);
    inbox_.push_back(std::move(ev));
}

// ----- public outbound helpers -- all just queue a JSON string -----

}  // namespace fm2k
