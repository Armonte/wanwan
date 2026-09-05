// host_services.h -- the background services the ImGui launcher runs and
// this one did not: the disk log, the diagnostics upload queue, the replay
// uploader and UPnP port mapping.
//
// None of these have any UI. They are the difference between a launcher
// that looks right and one that can replace the shipping product: without
// the log a "it opened then closed" report has nothing to attach, without
// the queues we lose the desync bundles and replays the hook writes, and
// without the mapping a peer behind a NAT is only reachable by luck.
//
// Every function here is safe to call when the feature is unavailable (no
// secret baked in, a router with no UPnP, a read-only install directory).
// They do nothing in that case rather than failing the launcher.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hostgui {

// launcher.log next to the exe, plus the crash breadcrumb handlers. Call
// once, first thing, before any SDL logging. The previous run rotates to
// launcher.prev.log.
void ServicesInitLog();
void ServicesShutdownLog();

// Once per frame. Services at most one diagnostics manifest and one
// replay per call, round-robin over the library, so a backlog trickles
// out instead of stalling a frame. `game_dirs` is every installed game's
// folder: the hook writes upload_queue/, replays/ and sessions/ there.
void ServicesTick(const std::vector<std::string>& game_dirs);

// A hub match ended. Arms the replay upload for that game; the hook
// finalises the session file after the match, so nothing is sent yet.
// `hub_host` decides where it goes, so a self-hosted hub receives its own
// replays instead of posting them to production.
void ServicesMatchEnded(const std::string& game_dir, const std::string& game_id,
                        const std::string& hub_host, const std::string& my_nick,
                        const std::string& peer_nick);

// UPnP. Start asks the router to forward this UDP port; it runs on its own
// thread and never blocks the frame. Skip it for a loopback hub, or two
// launchers on one machine fight over the same mapping.
void ServicesPortMapStart(uint16_t udp_port);
void ServicesPortMapStop();
// True exactly once, on the transition into Mapped, filling the external
// endpoint the hub needs. Every other call is false, so the caller can
// poll it every frame and act on the one that matters.
bool ServicesPortMapBecameMapped(std::string* ext_ip, uint16_t* ext_port);
// One line for the Network tab: "mapped 203.0.113.7:7000", "no UPnP
// router", and so on. Empty when the mapper was never started.
std::string ServicesPortMapStatus();

}  // namespace hostgui
