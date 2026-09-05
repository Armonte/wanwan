// host_launch.h -- BOOT. The game-instance code is the shipping launcher's
// (launcher/core/FM2K_GameInstance*.cpp: CreateProcess suspended, inject
// FM2KHook.dll / FM95Hook.dll from beside THIS exe, resume), so the spike
// launches exactly what the ImGui launcher launches, with the same env
// contract for an OFFLINE session (session_control.cpp StartOfflineSession
// + the Play Offline button's developer flags + per-game patches).
#pragma once
#include <cstdint>
#include <string>

namespace fm2k { struct HubEvent; }

namespace hostgui {

// Start the selected game offline. `engine` is "FM2K" or "FM95" (the
// library's engine column). Returns false with `err` filled when nothing
// was launched.
bool LaunchOffline(const std::string& exe_path_utf8, const std::string& engine,
                   const std::string& game_stem, std::string* err);

// Start the selected game ONLINE for a hub match. Runs the shipping
// sequence (launcher/session/FM2K_OnlineLaunch.cpp): best-effort punch,
// hub/peer/relay environment, online game.ini apply, launch. `my_id` is
// this user's hub id. Fails with *err set when the ini could not be
// applied or the process did not start.
bool LaunchOnline(const std::string& exe_path_utf8, const std::string& engine,
                  const std::string& game_stem, const fm2k::HubEvent& ev,
                  const std::string& my_id, std::string* err);

// True while the running (or just-exited) game was an online match, so the
// host can tell the hub the match ended when the process goes away.
bool LaunchWasOnline();

// The running game's process id, 0 when none (the hook's shared memory
// is named by it).
uint32_t LaunchPid();

// ── Developer tools (CONFIG > DEVELOPER > Tools) ──
// The same env contracts the ImGui launcher's process_manager.cpp uses:
// two loopback clients on 7000/7001 with a FIXED rng seed (local test
// only, never the matchmade path), spectators on 7002/7003 joining the
// host on 7000, and the stress harness (GekkoStressSession forcing
// rollbacks on one instance). Each returns false with *err set.
bool DevLaunchDual(const std::string& exe, const std::string& engine, std::string* err);
bool DevLaunchSpectator(int which /*1 or 2*/, const std::string& exe, const std::string& engine, std::string* err);
bool DevLaunchStress(const std::string& exe, const std::string& engine, std::string* err);
// Online by hand: host on 7000 (peer address may be empty, the hook
// learns it from HELLO) or join "ip:port". Legacy direct-IP path.
bool DevLaunchDirect(const std::string& exe, const std::string& engine, bool host,
                     const std::string& remote, std::string* err);
// Stop every developer instance (and the main one).
void DevTerminateAll();

// Per frame: notices the game exiting (restores game.ini from the backup
// ApplyForLaunch made). Returns 1 while a game is running, 0 otherwise.
int LaunchTick();

// Seconds since the running game started (0 when none).
int LaunchElapsedSeconds();

void LaunchTerminate();
void LaunchShutdown();

}  // namespace hostgui
