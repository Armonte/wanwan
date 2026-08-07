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
    
    // Memory addresses for critical game state
    constexpr uintptr_t INPUT_BUFFER_INDEX_ADDR = 0x470000;
    constexpr uintptr_t RANDOM_SEED_ADDR = 0x41FB1C;
    
    // Player state addresses (working addresses for this game version)
    constexpr uintptr_t P1_INPUT_ADDR = 0x470100;          // Current input state (working)
    constexpr uintptr_t P1_STAGE_X_ADDR = 0x470104;
    constexpr uintptr_t P1_STAGE_Y_ADDR = 0x470108;
    constexpr uintptr_t P1_HP_ADDR = 0x47010C;              // Current HP (verified in combat)
    constexpr uintptr_t P1_MAX_HP_ADDR = 0x470110;
    constexpr uintptr_t P1_INPUT_HISTORY_ADDR = 0x470200;  // Input history buffer
    
    constexpr uintptr_t P2_INPUT_ADDR = 0x470300;          // Current input state (working)
    constexpr uintptr_t P2_HP_ADDR = 0x47030C;              // Current HP (verified in combat)
    constexpr uintptr_t P2_MAX_HP_ADDR = 0x470310;
    constexpr uintptr_t P2_INPUT_HISTORY_ADDR = 0x470400;
    
    // ArtMoney verified addresses (tested during live combat)
    // Player coordinates (stage positions)
    constexpr uintptr_t P1_COORD_X_ADDR = 0x4ADCC3;        // P1 X coordinate (u32)
    constexpr uintptr_t P1_COORD_Y_ADDR = 0x4ADCC7;        // P1 Y coordinate (u16)
    constexpr uintptr_t P2_COORD_X_ADDR = 0x4EDD02;        // P2 X coordinate (u32)
    constexpr uintptr_t P2_COORD_Y_ADDR = 0x4EDD06;        // P2 Y coordinate (u16)
    
    // Player max HP (character-specific values)
    constexpr uintptr_t P1_MAX_HP_ARTMONEY_ADDR = 0x4DFC85; // P1 max HP (u32) - verified: 800
    constexpr uintptr_t P2_MAX_HP_ARTMONEY_ADDR = 0x4EDC4;  // P2 max HP (u32) - needs validation
    
    // Super meter system
    constexpr uintptr_t P1_SUPER_ADDR = 0x4EDC3D;          // P1 super meter (u32)
    constexpr uintptr_t P2_SUPER_ADDR = 0x4EDC0C;          // P2 super meter (u32)
    constexpr uintptr_t P1_SPECIAL_STOCK_ADDR = 0x4EDDC95; // P1 special stock (u32) - needs validation
    constexpr uintptr_t P2_SPECIAL_STOCK_ADDR = 0x4EDC4;   // P2 special stock (u32) - same as max HP?
    
    // Map/stage coordinates
    constexpr uintptr_t MAP_X_COORD_ADDR = 0x44742C;       // Map X coordinate (u32)
    constexpr uintptr_t MAP_Y_COORD_ADDR = 0x447F30;       // Map Y coordinate (u32) - needs validation
    
    // Global state addresses
    constexpr uintptr_t ROUND_TIMER_ADDR = 0x470044;       // Primary timer (verified)
    constexpr uintptr_t ROUND_TIMER_ALT_ADDR = 0x47DB94;   // ArtMoney timer (verified: same value)
    constexpr uintptr_t GAME_TIMER_ADDR = 0x470060;
    constexpr uintptr_t ROUND_NUMBER_ADDR = 0x4070044;     // Round number (u32)
    
    // ======= CHARACTER SELECT ADDRESSES (CCCaster-style) =======
    // Core character selection state (similar to CC_P1/P2_CHARACTER_ADDR)
    constexpr uintptr_t P1_CHARACTER_ID_ADDR = 0x470180;    // P1 selected character ID (u32)
    constexpr uintptr_t P2_CHARACTER_ID_ADDR = 0x470184;    // P2 selected character ID (u32)
    constexpr uintptr_t SELECTED_STAGE_ADDR = 0x43010c;     // Selected stage ID (u32) -- IDA-verified WW: vs_round_function reads this as wParam, settings_dialog_proc writes CB_GETCURSEL into it
    
    // Character grid cursor positions (similar to CC_P1/P2_CHARA_SELECTOR_ADDR)
    constexpr uintptr_t P1_CURSOR_X_ADDR = 0x47018C;        // P1 character grid cursor X (u32)
    constexpr uintptr_t P1_CURSOR_Y_ADDR = 0x470190;        // P1 character grid cursor Y (u32)
    constexpr uintptr_t P2_CURSOR_X_ADDR = 0x470194;        // P2 character grid cursor X (u32)
    constexpr uintptr_t P2_CURSOR_Y_ADDR = 0x470198;        // P2 character grid cursor Y (u32)
    
    // Character variations and colors (similar to CC_P1/P2_MOON_SELECTOR_ADDR, CC_P1/P2_COLOR_SELECTOR_ADDR)
    constexpr uintptr_t P1_VARIANT_ADDR = 0x47019C;         // P1 character variant/style (u32) - also confirms P1
    constexpr uintptr_t P2_VARIANT_ADDR = 0x4701A0;         // P2 character variant/style (u32) - also confirms P2
    constexpr uintptr_t P1_COLOR_ADDR = 0x4701A4;           // P1 color palette selection (u32)
    constexpr uintptr_t P2_COLOR_ADDR = 0x4701A8;           // P2 color palette selection (u32)
    
    // Selection mode tracking (similar to CC_P1/P2_SELECTOR_MODE_ADDR)
    constexpr uintptr_t P1_SELECTION_MODE_ADDR = 0x4701AC;  // P1 selection mode (0=selecting, 1=confirmed, 2=ready) (u32)
    constexpr uintptr_t P2_SELECTION_MODE_ADDR = 0x4701B0;  // P2 selection mode (0=selecting, 1=confirmed, 2=ready) (u32)
    
    // CSS timing and validation
    constexpr uintptr_t CSS_FRAME_COUNTER_ADDR = 0x4701B4;  // Frames since entering CSS (u32)
    constexpr uintptr_t CSS_STATE_FLAGS_ADDR = 0x4701B8;    // CSS state flags (lockout, timing validation) (u32)
    
    // Character select confirmation addresses (verified working addresses)
    constexpr uintptr_t P1_CONFIRMED_ADDR = 0x47019C;       // P1 confirmation status (1 = confirmed) (same as variant for FM2K)
    constexpr uintptr_t P2_CONFIRMED_ADDR = 0x4701A0;       // P2 confirmation status (1 = confirmed) (same as variant for FM2K)
    
    // ======= CHARACTER SELECT MEMORY ACCESS NAMESPACE =======
    namespace CharSelect {
        // Core selection addresses (CCCaster-style access)
        namespace Memory {
            // Character selection data
            constexpr uintptr_t P1_CHARACTER_ADDR = P1_CHARACTER_ID_ADDR;
            constexpr uintptr_t P2_CHARACTER_ADDR = P2_CHARACTER_ID_ADDR;
            constexpr uintptr_t STAGE_SELECTOR_ADDR = SELECTED_STAGE_ADDR;
            
            // Cursor positions (character grid navigation)
            constexpr uintptr_t P1_CURSOR_X_ADDR = FM2K::P1_CURSOR_X_ADDR;
            constexpr uintptr_t P1_CURSOR_Y_ADDR = FM2K::P1_CURSOR_Y_ADDR;
            constexpr uintptr_t P2_CURSOR_X_ADDR = FM2K::P2_CURSOR_X_ADDR;
            constexpr uintptr_t P2_CURSOR_Y_ADDR = FM2K::P2_CURSOR_Y_ADDR;
            
            // Character variations and colors
            constexpr uintptr_t P1_VARIANT_SELECTOR_ADDR = P1_VARIANT_ADDR;
            constexpr uintptr_t P2_VARIANT_SELECTOR_ADDR = P2_VARIANT_ADDR;
            constexpr uintptr_t P1_COLOR_SELECTOR_ADDR = P1_COLOR_ADDR;
            constexpr uintptr_t P2_COLOR_SELECTOR_ADDR = P2_COLOR_ADDR;
            
            // Selection mode tracking 
            constexpr uintptr_t P1_SELECTOR_MODE_ADDR = P1_SELECTION_MODE_ADDR;
            constexpr uintptr_t P2_SELECTOR_MODE_ADDR = P2_SELECTION_MODE_ADDR;
            
            // Confirmation status (CCCaster compatibility)
            constexpr uintptr_t P1_CONFIRMED_STATUS_ADDR = P1_CONFIRMED_ADDR;
            constexpr uintptr_t P2_CONFIRMED_STATUS_ADDR = P2_CONFIRMED_ADDR;
            
            // Timing and validation
            constexpr uintptr_t CSS_FRAME_COUNTER_ADDR = FM2K::CSS_FRAME_COUNTER_ADDR;
            constexpr uintptr_t CSS_STATE_FLAGS_ADDR = FM2K::CSS_STATE_FLAGS_ADDR;
        }
        
        // Character select constants (similar to CCCaster's CC_SELECT_CHARA)
        namespace Constants {
            constexpr uint32_t SELECT_CHARA = 0;        // Player is selecting character
            constexpr uint32_t CHARA_CONFIRMED = 1;     // Player has confirmed character
            constexpr uint32_t FULLY_READY = 2;         // Player is ready for battle
            
            // Input validation timing (CCCaster-style)
            constexpr uint32_t CSS_LOCKOUT_FRAMES = 150;        // Frames to wait before allowing confirm (prevents moon selector desync)
            constexpr uint32_t MODE_CHANGE_LOCKOUT = 2;         // Frames to wait after selector mode change
            constexpr uint32_t BUTTON_HISTORY_FRAMES = 3;       // Frames to check for button conflicts
        }
    }
    
    // Character variables (16 variables per player - verified addresses)
    // Player 1 character variables (1-byte each)
    constexpr uintptr_t P1_CHAR_VAR_A_ADDR = 0x4ADFD17;   // Char Var A (u8)
    constexpr uintptr_t P1_CHAR_VAR_B_ADDR = 0x4ADFD19;   // Char Var B (u8)
    constexpr uintptr_t P1_CHAR_VAR_C_ADDR = 0x4ADFD1B;   // Char Var C (u8)
    constexpr uintptr_t P1_CHAR_VAR_D_ADDR = 0x4ADFD1D;   // Char Var D (u8)
    constexpr uintptr_t P1_CHAR_VAR_E_ADDR = 0x4ADFD1F;   // Char Var E (u8)
    constexpr uintptr_t P1_CHAR_VAR_F_ADDR = 0x4ADFD21;   // Char Var F (u8)
    constexpr uintptr_t P1_CHAR_VAR_G_ADDR = 0x4ADFD23;   // Char Var G (u8)
    constexpr uintptr_t P1_CHAR_VAR_H_ADDR = 0x4ADFD25;   // Char Var H (u8)
    constexpr uintptr_t P1_CHAR_VAR_I_ADDR = 0x4ADFD27;   // Char Var I (u8)
    constexpr uintptr_t P1_CHAR_VAR_J_ADDR = 0x4ADFD29;   // Char Var J (u8)
    constexpr uintptr_t P1_CHAR_VAR_K_ADDR = 0x4ADFD2B;   // Char Var K (u8)
    constexpr uintptr_t P1_CHAR_VAR_L_ADDR = 0x4ADFD2D;   // Char Var L (u8)
    constexpr uintptr_t P1_CHAR_VAR_M_ADDR = 0x4ADFD2F;   // Char Var M (u8)
    constexpr uintptr_t P1_CHAR_VAR_N_ADDR = 0x4ADFD31;   // Char Var N (u8)
    constexpr uintptr_t P1_CHAR_VAR_O_ADDR = 0x4ADFD33;   // Char Var O (u8)
    constexpr uintptr_t P1_CHAR_VAR_P_ADDR = 0x4ADFD35;   // Char Var P (u8)
    
    // Player 2 character variables (2-byte each) 
    constexpr uintptr_t P2_CHAR_VAR_A_ADDR = 0x4ADFD5;    // Char Var A P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_B_ADDR = 0x4EDD58;    // Char Var B P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_C_ADDR = 0x4EDD5A;    // Char Var C P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_D_ADDR = 0x4EDDC;     // Char Var D P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_F_ADDR = 0x4EDD60;    // Char Var F P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_G_ADDR = 0x4EDD62;    // Char Var G P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_H_ADDR = 0x4EDD64;    // Char Var H P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_I_ADDR = 0x4ADFD6;    // Char Var I P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_J_ADDR = 0x4ADFD8;    // Char Var J P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_K_ADDR = 0x4EDD6A;    // Char Var K P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_L_ADDR = 0x4EDDC6;    // Char Var L P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_M_ADDR = 0x4EDD6E;    // Char Var M P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_N_ADDR = 0x4ADFD0;    // Char Var N P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_O_ADDR = 0x4ADFD7;    // Char Var O P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_P_ADDR = 0x4EDD74;    // Char Var P P2 (u16)
    
    // System variables (global game state variables)
    constexpr uintptr_t SYSTEM_VAR_A_ADDR = 0x4456B0;     // System Var A (u8)
    constexpr uintptr_t SYSTEM_VAR_B_ADDR = 0x4456B2;     // System Var B (u8)
    constexpr uintptr_t SYSTEM_VAR_D_ADDR = 0x4456B6;     // System Var D (u8)
    constexpr uintptr_t SYSTEM_VAR_E_ADDR = 0x4456B8;     // System Var E (u8)
    constexpr uintptr_t SYSTEM_VAR_F_ADDR = 0x4456BA;     // System Var F (u8)
    constexpr uintptr_t SYSTEM_VAR_G_ADDR = 0x4456BC;     // System Var G (u8)
    constexpr uintptr_t SYSTEM_VAR_H_ADDR = 0x4456BE;     // System Var H (u16)
    constexpr uintptr_t SYSTEM_VAR_I_ADDR = 0x4456C0;     // System Var I (u16)
    constexpr uintptr_t SYSTEM_VAR_J_ADDR = 0x456C2;      // System Var J (u16)
    constexpr uintptr_t SYSTEM_VAR_K_ADDR = 0x456C4;      // System Var K (u8)
    constexpr uintptr_t SYSTEM_VAR_L_ADDR = 0x456C6;      // System Var L (u8)
    constexpr uintptr_t SYSTEM_VAR_M_ADDR = 0x456C8;      // System Var M (u8)
    constexpr uintptr_t SYSTEM_VAR_N_ADDR = 0x456CA;      // System Var N (u8)
    constexpr uintptr_t SYSTEM_VAR_O_ADDR = 0x456CC;      // System Var O (u8)
    constexpr uintptr_t SYSTEM_VAR_P_ADDR = 0x456CE;      // System Var P (u16)

    // Sprite effect system addresses
    constexpr uintptr_t EFFECT_ACTIVE_FLAGS = 0x40CC30;  // Bitfield of active effects
    constexpr uintptr_t EFFECT_TIMERS_BASE = 0x40CC34;   // Array of 8 effect timers
    constexpr uintptr_t EFFECT_COLORS_BASE = 0x40CC54;   // Array of 8 RGB color sets
    constexpr uintptr_t EFFECT_TARGETS_BASE = 0x40CCD4;  // Array of 8 target IDs
    
    // Game State
    constexpr DWORD OBJECT_POOL_ADDR = 0x4701E0;
    
    // Hook Points
    constexpr DWORD FRAME_HOOK_ADDR = 0x4146D0;
    constexpr DWORD UPDATE_GAME_STATE_ADDR = 0x404CD0;
    
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
    uint32_t Fletcher32(const uint16_t* data, size_t len);
    float GetFM2KFrameTime(float frames_ahead);
    std::chrono::milliseconds GetFrameDuration();

    struct GameState {
        // Frame and timing state
        uint32_t frame_number;
        uint32_t input_buffer_index;
        uint32_t last_frame_time;
        uint32_t frame_time_delta;
        uint8_t frame_skip_count;
        uint8_t frame_sync_flag;
        
        // Random number generator state
        uint32_t random_seed;
        
        // Player states
        struct PlayerState {
            uint32_t input_current;
            uint32_t input_history[1024];
            uint32_t stage_x, stage_y;
            uint32_t hp, max_hp;
            uint32_t meter, max_meter;
            uint32_t combo_counter;
            uint32_t hitstun_timer;
            uint32_t blockstun_timer;
            uint32_t anim_timer;
            uint32_t move_id;
            uint32_t state_flags;
        } players[2];

        // Visual effects state (simplified - no IPC)
        uint32_t visual_state_flags;

        // Hit detection tables
        struct HitBox {
            int32_t x, y, w, h;
            uint32_t type;
            uint32_t damage;
            uint32_t flags;
        };
        
        static constexpr size_t MAX_HITBOXES = 32;
        HitBox hit_boxes[MAX_HITBOXES];
        uint32_t hit_box_count;

        // Calculate state checksum for rollback verification
        uint32_t CalculateChecksum() const {
            return Fletcher32(reinterpret_cast<const uint16_t*>(this), sizeof(GameState));
        }
    };

    // Minimal game state for GekkoNet rollback testing (48 bytes)
    // Contains only essential combat state to test desync detection
    struct MinimalGameState {
        // Core combat state (32 bytes)
        uint32_t p1_hp, p2_hp;              // Current HP (0x47010C, 0x47030C)
        uint32_t p1_max_hp, p2_max_hp;      // Max HP (0x4DFC85, 0x4EDC4)
        uint32_t p1_x, p1_y;                // Positions (0x4ADCC3, 0x4ADCC7)
        uint32_t p2_x, p2_y;                // Positions (0x4EDD02, 0x4EDD06)
        
        // Essential timers & RNG (16 bytes)
        uint32_t round_timer;                // 0x470044 or 0x47DB94
        uint32_t random_seed;                // 0x41FB1C
        uint32_t frame_number;               // Current frame
        uint32_t input_checksum;             // XOR of recent inputs
        
        // Calculate minimal state checksum
        uint32_t CalculateChecksum() const {
            return Fletcher32(reinterpret_cast<const uint16_t*>(this), sizeof(MinimalGameState));
        }
        
        // Load minimal state from memory addresses
        bool LoadFromMemory() {
            // Read HP values
            uint32_t* p1_hp_ptr = (uint32_t*)P1_HP_ADDR;
            uint32_t* p2_hp_ptr = (uint32_t*)P2_HP_ADDR;
            uint32_t* p1_max_hp_ptr = (uint32_t*)P1_MAX_HP_ARTMONEY_ADDR;
            uint32_t* p2_max_hp_ptr = (uint32_t*)P2_MAX_HP_ARTMONEY_ADDR;
            
            if (!p1_hp_ptr || !p2_hp_ptr || !p1_max_hp_ptr || !p2_max_hp_ptr) return false;
            
            p1_hp = *p1_hp_ptr;
            p2_hp = *p2_hp_ptr;
            p1_max_hp = *p1_max_hp_ptr;
            p2_max_hp = *p2_max_hp_ptr;
            
            // Read positions
            uint32_t* p1_x_ptr = (uint32_t*)P1_COORD_X_ADDR;
            uint16_t* p1_y_ptr = (uint16_t*)P1_COORD_Y_ADDR;
            uint32_t* p2_x_ptr = (uint32_t*)P2_COORD_X_ADDR;
            uint16_t* p2_y_ptr = (uint16_t*)P2_COORD_Y_ADDR;
            
            if (!p1_x_ptr || !p1_y_ptr || !p2_x_ptr || !p2_y_ptr) return false;
            
            p1_x = *p1_x_ptr;
            p1_y = *p1_y_ptr;
            p2_x = *p2_x_ptr;
            p2_y = *p2_y_ptr;
            
            // Read timers and RNG
            uint32_t* timer_ptr = (uint32_t*)ROUND_TIMER_ADDR;
            uint32_t* rng_ptr = (uint32_t*)RANDOM_SEED_ADDR;
            
            if (!timer_ptr || !rng_ptr) return false;
            
            round_timer = *timer_ptr;
            random_seed = *rng_ptr;
            
            return true;
        }
        
        // Save minimal state to memory addresses
        bool SaveToMemory() const {
            // Write HP values
            uint32_t* p1_hp_ptr = (uint32_t*)P1_HP_ADDR;
            uint32_t* p2_hp_ptr = (uint32_t*)P2_HP_ADDR;
            
            if (!p1_hp_ptr || !p2_hp_ptr) return false;
            
            *p1_hp_ptr = p1_hp;
            *p2_hp_ptr = p2_hp;
            
            // Write positions
            uint32_t* p1_x_ptr = (uint32_t*)P1_COORD_X_ADDR;
            uint16_t* p1_y_ptr = (uint16_t*)P1_COORD_Y_ADDR;
            uint32_t* p2_x_ptr = (uint32_t*)P2_COORD_X_ADDR;
            uint16_t* p2_y_ptr = (uint16_t*)P2_COORD_Y_ADDR;
            
            if (!p1_x_ptr || !p1_y_ptr || !p2_x_ptr || !p2_y_ptr) return false;
            
            *p1_x_ptr = p1_x;
            *p1_y_ptr = (uint16_t)p1_y;
            *p2_x_ptr = p2_x;
            *p2_y_ptr = (uint16_t)p2_y;
            
            // Write timers and RNG
            uint32_t* timer_ptr = (uint32_t*)ROUND_TIMER_ADDR;
            uint32_t* rng_ptr = (uint32_t*)RANDOM_SEED_ADDR;
            
            if (!timer_ptr || !rng_ptr) return false;
            
            *timer_ptr = round_timer;
            *rng_ptr = random_seed;
            
            return true;
        }
    };

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
