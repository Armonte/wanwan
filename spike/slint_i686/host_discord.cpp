// host_discord.cpp -- see host_discord.h.
#include "host_discord.h"

#include "FM2K_DiscordAuth.h"

#include <SDL3/SDL.h>
#include <windows.h>
#include <shellapi.h>

#include <memory>
#include <string>

namespace hostgui {
namespace {

using namespace fm2k::discord_auth;

CachedAuth                g_cached;
bool                      g_cached_loaded = false;
std::unique_ptr<Pairing>  g_pairing;
std::string               g_problem;
int                       g_last_status = -1;
std::string               g_hub_host;

void EnsureLoaded() {
    if (g_cached_loaded) return;
    g_cached_loaded = true;
    g_cached = LoadCached();
}

// Same split the shipping launcher makes for the pairing exchange
// (launcher_ui_settings.cpp): plain http only for a loopback or private
// host (a local dev hub on 7700), https for anything routable. The hub
// token is obtained through this exchange, so it never goes out in the
// clear to a public name.
bool IsPrivate(const std::string& h) {
    if (h == "localhost" || h == "127.0.0.1" || h == "::1") return true;
    if (h.rfind("10.", 0) == 0 || h.rfind("192.168.", 0) == 0) return true;
    if (h.rfind("172.", 0) == 0) {
        const int second = std::atoi(h.c_str() + 4);
        return second >= 16 && second <= 31;
    }
    return false;
}

std::string HubBaseUrl(std::string host) {
    if (host.empty() || host == "2dfm.sytes.net") host = "hub.2dfm.org";
    const bool v6 = (host == "::1");
    const size_t colon = v6 ? std::string::npos : host.find(':');
    const std::string bare = colon == std::string::npos ? host : host.substr(0, colon);
    if (IsPrivate(bare)) {
        std::string base = "http://" + (v6 ? std::string("[::1]") : host);
        if (colon == std::string::npos) base += ":7700";
        return base;
    }
    return "https://" + bare;
}

}  // namespace

DiscordState DiscordGet() {
    EnsureLoaded();
    DiscordState s;
    s.signed_in = g_cached.valid;
    s.use_discord_name = g_cached.use_discord_name;
    s.display = (g_cached.use_discord_name && !g_cached.discord_global_name.empty())
                ? g_cached.discord_global_name : g_cached.nick;
    s.problem = g_problem;
    if (g_pairing && g_pairing->status() == Pairing::Status::Pending) {
        s.pending = true;
        s.authorize_url = g_pairing->authorize_url();
        s.browser_open_failed = g_pairing->browser_open_failed();
    }
    return s;
}

bool DiscordAction(const std::string& a, const std::string& hub_host) {
    EnsureLoaded();
    if (a == "signin") {
        g_problem.clear();
        g_pairing.reset(Begin(HubBaseUrl(hub_host.empty() ? g_hub_host : hub_host)));
        g_last_status = -1;
        return true;
    }
    if (a == "signout") {
        ClearCached();
        g_cached = CachedAuth{};
        g_pairing.reset();
        g_problem.clear();
        return true;
    }
    if (a == "cancel") {
        if (g_pairing) g_pairing->Cancel();
        g_pairing.reset();
        g_problem.clear();
        return true;
    }
    if (a == "copy_url") {
        if (!g_pairing) return false;
        SDL_SetClipboardText(g_pairing->authorize_url().c_str());
        return true;
    }
    if (a == "reopen") {
        if (!g_pairing) return false;
        ShellExecuteA(nullptr, "open", g_pairing->authorize_url().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }
    if (a.rfind("use_discord_name:", 0) == 0) {
        if (!g_cached.valid) return false;
        g_cached.use_discord_name = (a.back() == '1');
        SaveCached(g_cached);
        return true;
    }
    return false;
}

bool DiscordTick() {
    if (!g_pairing) return false;
    const int st = (int)g_pairing->status();
    bool changed = (st != g_last_status);
    g_last_status = st;
    switch (g_pairing->status()) {
        case Pairing::Status::Pending:
            break;
        case Pairing::Status::Ok: {
            const CachedAuth a = g_pairing->result();
            if (SaveCached(a)) g_cached = a;
            g_pairing.reset();
            changed = true;
            break;
        }
        case Pairing::Status::Expired:
        case Pairing::Status::Error:
            g_problem = g_pairing->error_detail();
            g_pairing.reset();
            changed = true;
            break;
    }
    return changed;
}

void DiscordSetHubHost(const std::string& host) { g_hub_host = host; }

std::string DiscordHubToken() {
    EnsureLoaded();
    return g_cached.valid ? g_cached.hub_token : std::string();
}

}  // namespace hostgui
