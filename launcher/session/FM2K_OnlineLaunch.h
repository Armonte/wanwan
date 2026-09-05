// FM2K_OnlineLaunch.h -- the hub-brokered online launch, launcher-free.
//
// Everything that happens between "the hub said match_start" and "the game
// process exists" used to live inside LauncherUI::HandleMatchStartEvent and
// FM2KLauncher::StartOnlineSession: the best-effort NAT punch, the same-box
// loopback fallback, the FM2K_HUB_* / FM2K_PEER_* / FM2K_STAGE_RANDOM_*
// environment the hook reads on Netplay_Init, and the game.ini apply that
// carries the online anti-cheat clamps. None of that depends on ImGui or on
// the FM2KLauncher object, and the Slint launcher needs exactly the same
// sequence, so it lives here and BOTH launchers call it. One copy, one
// behaviour, one place the netplay gate has to cover.
#pragma once
#include "FM2K_Integration.h"   // NetworkConfig, FM2K::Engine

#include <cstdint>
#include <filesystem>
#include <string>

namespace fm2k { struct HubEvent; }
class FM2KGameInstance;

// Pre-match UDP punch toward the peer (opens the NAT mapping before the game
// binds the same port). Returns true when the peer answered in time.
bool HubPreflightPunch(uint16_t local_port,
                       const std::string& peer_ip,
                       uint16_t peer_port,
                       const std::string& match_token_hex,
                       int timeout_ms);

namespace fm2k::online_launch {

// Where the game should aim its first HELLO.
struct PeerPlan {
    std::string peer_ip;
    int         peer_port = 0;
    bool        public_reachable = false;   // the preflight punch got a reply
};

// Best-effort punch at the peer's public address, then -- only when the hub
// configured NO relay -- a loopback probe so a same-box test gets 127.0.0.1.
// Never refuses: the in-game NAT layer (STUN, punch rounds, relay) is the
// real traversal and retries on its own.
PeerPlan ProbePeer(uint16_t local_port, const fm2k::HubEvent& ev);

// Exports, on THIS process, every variable the spawned game inherits for a
// hub match: hub UDP address (pre-resolved so DllMain stays off DNS), user
// id, match token, this player's SOCD mode, relay, same-LAN and IPv6
// candidates, the unverified-reflexive flag and the random-stage seed.
// Clears each one the event does not carry, so nothing leaks between
// matches in a long-lived launcher process.
void ExportMatchEnv(const fm2k::HubEvent& ev, const std::string& my_id,
                    int local_socd_mode);

// The Program Files / VirtualStore split detector, logged once per launch.
// Never fatal on its own.
void LogInstallHealth(const std::filesystem::path& exe_path);

enum class StartResult {
    Ok,
    IniRefused,     // game.ini could not be applied: anti-cheat clamps not written, refused
    LaunchFailed,   // CreateProcess / inject failed
};

// Per-instance environment, the online game.ini apply (with its one retry and
// the deleted-ini rescue) and the launch itself. `inst` must be fresh; the
// caller applies its own pending instance config before calling.
StartResult StartOnlineGame(FM2KGameInstance& inst,
                            const std::string& exe_path,
                            FM2K::Engine engine,
                            const NetworkConfig& cfg,
                            bool is_host);

}  // namespace fm2k::online_launch
