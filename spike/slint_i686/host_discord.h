// host_discord.h -- Discord sign-in, host side, over the shipping pairing
// module (launcher/net/FM2K_DiscordAuth.cpp): same hub exchange, same
// discord_auth.json cache the ImGui launcher reads, so signing in here
// signs the other launcher in too.
#pragma once
#include <string>

namespace hostgui {

// Where the sign-in stands, for the HUB tab rows.
struct DiscordState {
    bool        signed_in = false;
    std::string display;        // the name the hub will show
    bool        use_discord_name = true;
    bool        pending = false;    // a pairing is waiting on the browser
    std::string authorize_url;      // while pending
    bool        browser_open_failed = false;
    std::string problem;            // Expired / Error detail, until dismissed
};
DiscordState DiscordGet();

// Actions from the HUB tab: "signin", "signout", "cancel", "copy_url",
// "reopen", "use_discord_name:<0|1>". Returns false for an unknown one.
bool DiscordAction(const std::string& action, const std::string& hub_host);

// Per frame: advance a pending pairing. Returns true when the state
// changed and the rows must be republished.
bool DiscordTick();

// The hub host the NETWORK tab holds; the pairing exchange goes there.
void DiscordSetHubHost(const std::string& host);

// The cached hub token for connecting, "" when signed out.
std::string DiscordHubToken();

}  // namespace hostgui
