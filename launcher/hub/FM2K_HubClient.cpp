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
    // Discard spec-relay frames left over from a previous session. They
    // describe a match that is over (or a viewer that is gone), so
    // replaying them into a fresh hub session would ship stale spec data
    // at whoever subscribes next. JSON control messages are kept -- the
    // API contract is that they queue until the upgrade completes.
    // Counted as drops so the RELAY pill still tells the truth.
    {
        uint64_t discarded = 0;
        std::lock_guard<std::mutex> lk(out_mtx_);
        for (auto it = outbox_.begin(); it != outbox_.end(); ) {
            if (it->is_binary) { it = outbox_.erase(it); ++discarded; }
            else               { ++it; }
        }
        spec_relay_queued_bytes_ = 0;
        if (discarded) {
            spec_relay_dropped_.fetch_add(discarded, std::memory_order_relaxed);
        }
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
    // Two refusals, both counted (see SpecRelayDropped in the header):
    //
    //  1. No hub session. Nobody is subscribed, the hub would reject the
    //     frame anyway, and queueing it only means a later reconnect
    //     replays stale mid-match spec data at a viewer. Refuse now so
    //     the caller's ring drain is visible as loss instead of looking
    //     healthy while bytes pile up in RAM.
    //  2. Outbox already holding kSpecRelayMaxQueuedBytes of unsent spec
    //     data -- the WS cannot keep up. Refusing the newest frame keeps
    //     memory bounded; the viewer's own gap detection handles the hole.
    const size_t frame_bytes = frame.size();
    if (!connected_.load(std::memory_order_acquire)) {
        const uint64_t n =
            spec_relay_dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
        // Throttled: a relay-mode host with no hub produces one of these
        // per drained ring slot, i.e. tens per second during a snapshot.
        if (n == 1 || (n % 256) == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "HubClient: spec-relay frame dropped -- not connected to hub "
                "(dropped so far: %llu). Spec data is NOT reaching viewers.",
                (unsigned long long)n);
        }
        return;
    }
    OutMsg out{};
    out.is_binary = true;
    out.data      = std::move(frame);
    size_t queued_now = 0;
    bool   over_cap   = false;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        if (spec_relay_queued_bytes_ + frame_bytes > kSpecRelayMaxQueuedBytes) {
            over_cap   = true;
            queued_now = spec_relay_queued_bytes_;
        } else {
            spec_relay_queued_bytes_ += frame_bytes;
            outbox_.push_back(std::move(out));
        }
    }
    // Log outside the lock -- the SDL sink hops into the launcher's UI log.
    if (over_cap) {
        const uint64_t n =
            spec_relay_dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || (n % 256) == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "HubClient: spec-relay outbox full (%zu bytes queued) -- "
                "dropping frame (dropped so far: %llu)",
                queued_now, (unsigned long long)n);
        }
        return;
    }
    out_cv_.notify_one();
}

void HubClient::EmitEvent(HubEvent ev) {
    std::lock_guard<std::mutex> lk(in_mtx_);
    inbox_.push_back(std::move(ev));
}

// ----- public outbound helpers -- all just queue a JSON string -----

}  // namespace fm2k
