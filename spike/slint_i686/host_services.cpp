// host_services.cpp -- see the header.
#include "host_services.h"

#include "launcher_log.h"
#include "pii_scrub.h"
#include "FM2K_PortMapper.h"
#include "FM2K_ReplayUpload.h"
#include "FM2K_UploadQueue.h"
#include "auto_upload_secret.h"

#include <SDL3/SDL.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace hostgui {
namespace {

std::unique_ptr<fm2k::PortMapper> g_mapper;
int  g_mapper_last_state = -1;
bool g_log_open = false;

// One upload at a time, off the frame thread. WinHTTP blocks for as long
// as the network takes, and doing that inline is what used to spike the
// ping mid-match in the old launcher.
std::atomic<bool> g_upload_busy{false};
size_t g_upload_rr = 0;                    // round-robin cursor over the library
std::chrono::steady_clock::time_point g_upload_next;

}  // namespace

void ServicesInitLog() {
    if (g_log_open) return;
    // Scrubber first: launcher_log runs every line through it, so a log
    // opened before Init would write the unredacted ones.
    fm2k::pii::Init();
    fm2k::launcher_log::Init();
    fm2k::launcher_log::InstallCrashHandler();
    g_log_open = true;
}

void ServicesShutdownLog() {
    if (!g_log_open) return;
    fm2k::launcher_log::Shutdown();
    g_log_open = false;
}

void ServicesTick(const std::vector<std::string>& game_dirs) {
    // The replay uploader keeps its own queue on disk and returns quickly
    // when it is empty, so it is cheap to call every frame.
    fm2k::replay_upload::Process();

    if (game_dirs.empty()) return;
    if (!fm2k::kLogUploadSecret || fm2k::kLogUploadSecret[0] == '\0') return;
    if (g_upload_busy.load()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_upload_next) return;
    // 1.5s between attempts: a backlog of desync bundles trickles out
    // instead of saturating the connection in one burst.
    g_upload_next = now + std::chrono::milliseconds(1500);

    if (g_upload_rr >= game_dirs.size()) g_upload_rr = 0;
    const std::string dir = game_dirs[g_upload_rr++];
    g_upload_busy.store(true);
    std::thread([dir]() {
        fm2k::upload_queue::ProcessorConfig cfg;
        cfg.game_dir   = dir;
        cfg.upload_url = fm2k::kLogUploadUrl;
        cfg.secret     = fm2k::kLogUploadSecret;
        cfg.enabled    = true;
        fm2k::upload_queue::Process(cfg);
        g_upload_busy.store(false);
    }).detach();
}

void ServicesMatchEnded(const std::string& game_dir, const std::string& game_id,
                        const std::string& hub_host, const std::string& my_nick,
                        const std::string& peer_nick) {
    if (game_dir.empty() || game_id.empty()) return;
    fm2k::replay_upload::Config rc;
    rc.game_dir   = game_dir;
    rc.game_id    = game_id;
    rc.upload_url = "https://" + (hub_host.empty() ? std::string("hub.2dfm.org")
                                                   : hub_host) + "/replay/upload";
    rc.p1_nick    = my_nick;
    rc.p2_nick    = peer_nick;
    rc.enabled    = true;
    fm2k::replay_upload::OnMatchEnded(rc);
}

void ServicesPortMapStart(uint16_t udp_port) {
    if (!g_mapper) g_mapper = std::make_unique<fm2k::PortMapper>();
    // Re-arm the one-shot Mapped report so a reconnect sends it again.
    g_mapper_last_state = -1;
    g_mapper->StartAsync(udp_port);
}

void ServicesPortMapStop() {
    if (g_mapper) g_mapper->Stop();
    g_mapper_last_state = -1;
}

bool ServicesPortMapBecameMapped(std::string* ext_ip, uint16_t* ext_port) {
    if (!g_mapper) return false;
    const fm2k::PortMapper::Status st = g_mapper->Snapshot();
    const int cur = static_cast<int>(st.state);
    if (cur == g_mapper_last_state) return false;
    g_mapper_last_state = cur;
    if (st.state != fm2k::PortMapper::State::Mapped) return false;
    if (st.ext_udp_port == 0 || st.ext_ip.empty()) return false;
    if (ext_ip)   *ext_ip = st.ext_ip;
    if (ext_port) *ext_port = st.ext_udp_port;
    return true;
}

std::string ServicesPortMapStatus() {
    if (!g_mapper) return {};
    const fm2k::PortMapper::Status st = g_mapper->Snapshot();
    switch (st.state) {
        case fm2k::PortMapper::State::Idle:        return "UPnP idle";
        case fm2k::PortMapper::State::Discovering: return "looking for a UPnP router";
        case fm2k::PortMapper::State::Mapped:
            return "mapped " + st.ext_ip + ":" + std::to_string(st.ext_udp_port);
        case fm2k::PortMapper::State::NoIgd:       return "no UPnP router on this network";
        case fm2k::PortMapper::State::Failed:      return "UPnP mapping failed";
        case fm2k::PortMapper::State::Cgnat:
            // The router mapped a port on an address that is itself behind
            // the ISP's NAT, so the mapping cannot be reached from outside.
            return "router is behind carrier NAT -- mapping unusable";
    }
    return {};
}

}  // namespace hostgui
