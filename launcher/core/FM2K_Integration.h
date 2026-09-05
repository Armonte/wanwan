#pragma once

#include "SDL3/SDL.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "MinHook.h"
#include "ISession.h"
#include "FM2K_CncDDraw.h"   // IniConfig used as a member of LauncherUI
#include "FM2K_KgtParser.h"  // KgtSummary stored on FM2KGameInfo

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <windows.h>
#include <filesystem>
#include <cstdint>
#include <unordered_map>

// Forward declarations
class FM2KGameInstance;
class LauncherUI;
namespace fm2k { struct HubEvent; }   // hub WS event (FM2K_HubClient.h); only a
                                      // reference is needed for the LauncherUI::HandleHubEvent decl
namespace fm2k { struct MatchSettings; }  // challenge payload (FM2K_HubClient.h);
                                      // only a reference is needed for the
                                      // LauncherUI::BuildChallengeSettings decl
namespace fm2k { class PortMapper; }  // UPnP port mapper (Phase 1); defined
                                      // in FM2K_PortMapper.h, owned by
                                      // LauncherUI as a unique_ptr so the
                                      // miniupnpc headers stay out of here.

namespace fm2k {
// NAT classification result (Phase 2a). Produced by the launcher's dual STUN
// probe: one probe to the hub's primary STUN port and one to its
// classification port (+3) from the same bound socket. Comparing the two
// reflected external ports yields the RFC-4787 mapping behavior:
//   "cone"      -- both ports equal: endpoint-independent mapping, punchable.
//   "symmetric" -- ports differ: a new external port per destination, hard
//                  to punch.
//   "blocked"   -- no acks at all: UDP appears filtered.
//   "unknown"   -- only one ack (inconclusive) or the probe couldn't run.
// port_a / port_b are the reflected external ports (0 if that ack was
// missing). This is the value reported to the hub as udp_addr.nat_type.
struct NatClassifyResult {
    std::string nat_type = "unknown";
    uint16_t    port_a   = 0;  // reflected ext port from the primary STUN port
    uint16_t    port_b   = 0;  // reflected ext port from the classification port
};

// Run the dual STUN classification probe against a hub. Binds local_port,
// sends a 0xCD/0x01 probe to hub_udp_port AND to hub_udp_port+3 from the same
// socket, collects both acks under one ~1s window, and returns the
// classification. The probe to hub_udp_port also serves as the primary STUN
// pre-stamp (the hub records user.udp_addr from it), so this fully replaces
// the old single-port pre-match STUN. user_id is the 24-byte-padded Discord
// id the hub keys on; pass empty to skip (returns "unknown"). Transient: the
// socket is closed before return, exactly like the old single probe.
NatClassifyResult LauncherStunClassify(uint16_t local_port,
                                       const std::string& hub_host,
                                       uint16_t hub_udp_port,
                                       const std::string& user_id);

// Best-effort primary LAN IPv4 (e.g. "192.168.1.42") of this machine, via the
// UDP-connect-to-public + getsockname trick (no packet is sent). Used as the
// same-LAN candidate in the hub's local_ip exchange so two players behind the
// same router connect directly over the LAN. Returns "" if it can't resolve a
// private address (then the same-LAN path is simply skipped).
std::string LocalLanIp();
// This machine's GLOBAL IPv6 host candidate (2xxx::...), or "" if none. Sent to
// the hub so a peer can connect directly over IPv6 and bypass CGNAT entirely.
std::string LocalLanIpV6();
}  // namespace fm2k

// FM2K Memory addresses and structures (from research)
namespace FM2K {
    // Forward declarations for FM2K namespace classes
    class GekkoNetBridge;

    // NO ADDRESS TABLE LIVES HERE ANY MORE. 127 constants used to sit in this
    // header describing FM2K memory (player state, CSS, character variables,
    // system variables, sprite effects, hook points), reachable by 30+ launcher
    // TUs and referenced by exactly none of them: the only consumers were
    // CharSelect::Memory, CharSelect::Constants, struct GameState and struct
    // MinimalGameState, which nothing ever instantiated. Several were
    // provably wrong -- P1/P2_CHARACTER_ID_ADDR (0x470180/0x470184) and
    // P1/P2_INPUT_HISTORY_ADDR (0x470200/0x470400) are recorded in
    // FM2KHook/src/core/globals.h as unwritten memory with zero xrefs, and a
    // dozen more had 7-digit values that cannot be addresses in this image.
    // The live, IDA-verified address tables are globals.h (hook side, both
    // engines) and FM95_Integration.h (FM95 reference). Do not reintroduce a
    // launcher-side copy: the launcher does not read game memory, the hook
    // does.
    //
    // MinimalGameState went with them. Its SaveToMemory() dereferenced
    // game-process addresses as raw pointers from inside the LAUNCHER
    // process, so wiring it up would have faulted or corrupted launcher
    // memory, and its CalculateChecksum() could not have linked anyway:
    // FM2K::Fletcher32 was declared and never defined in that namespace.
    
    // Engine variant -- selects address tables / hook strategy / launch flow.
    // FM2K (Fighter Maker 2nd) is the default supported engine. FM95 is the
    // earlier prototype (e.g. CPW.exe -- Comic Party Wars). They share the
    // 256-slot object pool and 11-bit input format but differ in:
    //   - g_game_mode encoding (FM2K uses 2000/3000 magic; FM95 stays 0/1)
    //   - frame loop shape (FM2K has RUN_GAME_LOOP; FM95 inlines into WinMain)
    //   - CSS / battle phase classification (FM95 has no global mode flag)
    // See FM95_Integration.h for FM95 address tables and CharSelect:: helpers.
    enum class Engine : uint32_t {
        FM2K = 0,  // default -- Fighter Maker 2nd (most modern 2DFM titles)
        FM95 = 1,  // prototype -- Fighter Maker 95 (CPW.exe, early 2002 builds)
    };

    inline const char* EngineName(Engine e) {
        switch (e) {
            case Engine::FM2K: return "FM2K";
            case Engine::FM95: return "FM95";
            default:           return "unknown";
        }
    }

    // Game instance info
    struct FM2KGameInfo {
        std::string exe_path;
        std::string dll_path;
        uint32_t process_id;
        bool is_host;
        Engine engine = Engine::FM2K;  // detected at discovery time

        // Identification state, set by discovery:
        //   - is_clean   : exe hash matched a known-clean entry in the registry.
        //   - packer_label: non-empty => detected a real packer (Enigma /
        //                   UPX / MoleBox / etc) by sniffing PE section names.
        //                   This is what we *actually* warn about in the UI.
        //   - clean_label: friendly title when is_clean (e.g. "WonderfulWorld v0.946").
        //   - xxh64      : computed once at discovery, reused for cache + UI.
        //
        // Three render states drive from these:
        //   is_clean=true                       -> "<exe>  [<engine> -- <label>]"           normal
        //   packer_label non-empty              -> "* <exe>  [<engine> -- packed: <name>]"  yellow
        //   else (untested, no packer detected) -> "<exe>  [<engine> -- untested]"          normal
        bool        is_clean = false;
        std::string clean_label;
        std::string packer_label;
        uint64_t    xxh64    = 0;

        // Parsed .kgt summary -- player/stage/demo name lists. Populated at
        // discovery time so the UI can populate dropdowns pre-launch without
        // having to boot the game once and ReadProcessMemory the in-memory
        // buffers. `kgt.valid == false` means parse failed or no .kgt was
        // present (FM95 .player-only directories).
        fm2k::KgtSummary kgt;

        // Helper to get just the filename from the full path
        std::string GetExeName() const {
            const char* path = exe_path.c_str();
            const char* last_slash = SDL_strrchr(path, '/');
            if (!last_slash) last_slash = SDL_strrchr(path, '\\');
            return last_slash ? std::string(last_slash + 1) : exe_path;
        }

        // Helper to get the directory containing the exe
        std::string GetExeDir() const {
            const char* path = exe_path.c_str();
            char* dir = SDL_strdup(path);
            if (!dir) return "";

            char* last_slash = SDL_strrchr(dir, '/');
            if (!last_slash) last_slash = SDL_strrchr(dir, '\\');
            if (last_slash) {
                *last_slash = '\0';
                std::string result(dir);
                SDL_free(dir);
                return result;
            }
            SDL_free(dir);
            return "";
        }
    };

    // Utility functions
    bool FileExists(const std::string& path);



    // Input structure (11-bit input mask)
    struct Input {
        union {
            struct {
                uint16_t left     : 1;
                uint16_t right    : 1;
                uint16_t up       : 1;
                uint16_t down     : 1;
                uint16_t button1  : 1;
                uint16_t button2  : 1;
                uint16_t button3  : 1;
                uint16_t button4  : 1;
                uint16_t button5  : 1;
                uint16_t button6  : 1;
                uint16_t button7  : 1;
                uint16_t reserved : 5;
            } bits;
            uint16_t value;
        };
    };

    // Memory access functions
    template<typename T>
    bool ReadMemory(HANDLE process, uintptr_t address, T& value) {
        SIZE_T bytes_read;
        return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), 
            &value, sizeof(T), &bytes_read) && bytes_read == sizeof(T);
    }

    template<typename T>
    bool WriteMemory(HANDLE process, uintptr_t address, const T& value) {
        SIZE_T bytes_written;
        return WriteProcessMemory(process, reinterpret_cast<LPVOID>(address),
            &value, sizeof(T), &bytes_written) && bytes_written == sizeof(T);
    }
}

// Launcher states
enum class LauncherState {
    GameSelection,      // Selecting which FM2K game to launch
    Connecting,         // Establishing network connection
    InGame,            // Game running with rollback active
    Disconnected       // Connection lost, can reconnect
};

// Network configuration
struct NetworkConfig {
    SessionMode session_mode;
    int local_port;
    std::string remote_address;
    int input_delay;
    bool is_host; // True if hosting, false if joining

    NetworkConfig() 
        : session_mode(SessionMode::LOCAL)  // Default to LOCAL for testing
        , local_port(7000)
        , remote_address("127.0.0.1:7001")
        , input_delay(2)
        , is_host(false)
    {
        // Use SDL string functions for initialization
        char remote_addr[32];
        SDL_strlcpy(remote_addr, "127.0.0.1:7001", sizeof(remote_addr));
        remote_address = remote_addr;
    }
};

// Rollback performance statistics (standalone, used by both launcher and UI)
struct RollbackStats {
    uint32_t rollbacks_per_second; // Current rollback frequency
    uint32_t max_rollback_frames;  // Maximum rollback distance
    uint32_t avg_rollback_frames;  // Average rollback distance
    float frame_advantage;         // Current frame advantage
    uint32_t input_delay_frames;   // Current input delay
    uint32_t confirmed_frames;     // Number of confirmed frames
    uint32_t speculative_frames;   // Number of speculative frames
};

// Forward declaration
class LauncherUI;


// ---------------------------------------------------------------------------
// Launcher app classes -- split out of this header (umbrella include). Order
// matters: FM2KLauncher first (it only needs the LauncherUI forward-decl
// above for its unique_ptr member), LauncherUI second.
// ---------------------------------------------------------------------------
#include "FM2K_Launcher_decl.h"
#include "FM2K_LauncherUI_decl.h"
