#pragma once

#include "SDL3/SDL.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "vendored/GekkoNet/GekkoLib/include/gekkonet.h"
#include "MinHook.h"
#include "ISession.h"

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
    constexpr uintptr_t SELECTED_STAGE_ADDR = 0x470188;     // Selected stage ID (u32)
    
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
    
    // Game instance info
    struct FM2KGameInfo {
        std::string exe_path;
        std::string dll_path;
        uint32_t process_id;
        bool is_host;

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

// Main launcher class
class FM2KLauncher {
public:
    FM2KLauncher();
    ~FM2KLauncher();
    
    bool Initialize();
    void Shutdown();
    void Update(float delta_time);
    void Render();
    void HandleEvent(SDL_Event* event);
    
    bool LaunchGame(const FM2K::FM2KGameInfo& game);
    void TerminateGame();

    void StartOfflineSession();
    void StartOnlineSession(const NetworkConfig& config, bool is_host);
    void StopSession();
    
    std::vector<FM2K::FM2KGameInfo> DiscoverGames();
    const std::vector<FM2K::FM2KGameInfo>& GetDiscoveredGames() const { return discovered_games_; }
    
    void SetState(LauncherState state);
    bool IsRunning() const { return running_; }
    void SetRunning(bool running) { running_ = running; }
    
    // Games directory management
    const std::string& GetGamesRootPath() const { return games_root_path_; }
    void SetGamesRootPath(const std::string& path);
    void SetSelectedGame(const FM2K::FM2KGameInfo& game);
    
    // ----- Asynchronous game discovery -----
    SDL_Thread* discovery_thread_ = nullptr; // Worker thread handle
    bool discovery_in_progress_ = false;     // Flag so we don't launch multiple scans

    // Starts a background SDL thread that will run DiscoverGames() and notify the main
    // thread when done. Implemented in FM2K_RollbackClient.cpp.
    void StartAsyncDiscovery();
    
    // Scan progress accessors for UI
    void SetScanning(bool scanning);

    SDL_Window* GetWindow() const { return window_; }

private:
    bool InitializeSDL();
    bool InitializeImGui();
    
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    std::unique_ptr<LauncherUI> ui_;
    std::unique_ptr<FM2KGameInstance> game_instance_;
    std::vector<FM2K::FM2KGameInfo> discovered_games_;
    
    // Multi-client testing instances
    std::unique_ptr<FM2KGameInstance> client1_instance_;
    std::unique_ptr<FM2KGameInstance> client2_instance_;
    FM2K::FM2KGameInfo selected_game_;
    NetworkConfig network_config_;
    LauncherState current_state_;
    bool running_;
    
    // Timing
    std::chrono::steady_clock::time_point last_frame_time_;
    
    // Game discovery helpers
    bool ValidateGameFiles(FM2K::FM2KGameInfo& game);
    std::string DetectGameVersion(const std::string& exe_path);
    
    // Multi-client testing helpers
    bool LaunchLocalClient(const std::string& game_path, bool is_host, int port);
    bool TerminateAllClients();
    
    // GekkoNet session management
    bool InitializeGekkoSession();
    void ShutdownGekkoSession();
    bool StartLocalSession();
    void StopLocalSession();
    
    // Multi-client testing
    uint32_t client1_process_id_;
    uint32_t client2_process_id_;
    
    // GekkoNet session management
    GekkoSession* gekko_session_;
    GekkoConfig gekko_config_;
    bool gekko_initialized_;
    
    // Games directory (root where FM2K games are located)
    std::string games_root_path_;
    
    // Pending configuration (set before instances are created)
    struct PendingConfig {
        bool has_minimal_gamestate_testing = false;
        bool minimal_gamestate_testing_value = false;
        bool has_production_mode = false;
        bool production_mode_value = false;
        bool has_input_recording = false;
        bool input_recording_value = false;
    } pending_config_;
    
    // Helper method to read rollback statistics from hook shared memory  
    bool ReadRollbackStatsFromSharedMemory(RollbackStats& stats);
    
    // Apply pending configuration to game instances
    void ApplyPendingConfigToInstance(FM2KGameInstance* instance);
};

// Game instance management - see FM2K_GameInstance.h for full definition

// Modern ImGui launcher interface
class LauncherUI {
public:
    LauncherUI();
    ~LauncherUI();
    
    bool Initialize(SDL_Window* window, SDL_Renderer* renderer);
    void Shutdown();
    
    void NewFrame();
    void Render();
    
    // UI state callbacks
    std::function<void(const FM2K::FM2KGameInfo&)> on_game_selected;
    std::function<void()> on_offline_session_start;
    std::function<void(const NetworkConfig&)> on_online_session_start;
    std::function<void()> on_session_stop;
    std::function<void()> on_exit;
    std::function<void(const std::string&)> on_games_folder_set;
    
    // Debug state callbacks
    std::function<bool()> on_debug_save_state;
    std::function<bool()> on_debug_load_state;
    std::function<bool(uint32_t)> on_debug_force_rollback;
    
    // Slot-based save/load callbacks
    std::function<bool(uint32_t)> on_debug_save_to_slot;
    std::function<bool(uint32_t)> on_debug_load_from_slot;
    std::function<bool(bool, uint32_t)> on_debug_auto_save_config;  // (enabled, interval_frames)
    
    // Slot status callback
    struct SlotStatusInfo {
        bool occupied;
        uint32_t frame_number;
        uint64_t timestamp_ms;
        uint32_t checksum;
        uint32_t state_size_kb;
        uint32_t save_time_us;
        uint32_t load_time_us;
    };
    std::function<bool(uint32_t, SlotStatusInfo&)> on_get_slot_status;  // (slot, status_out)
    
    // Auto-save configuration callbacks
    struct AutoSaveConfig {
        bool enabled;
        uint32_t interval_frames;
    };
    std::function<bool(AutoSaveConfig&)> on_get_auto_save_config;  // Get current auto-save settings
    
    // Debug and testing configuration callbacks
    std::function<bool(bool)> on_set_production_mode;              // Set production mode (reduced logging)
    std::function<bool(bool)> on_set_input_recording;              // Set input recording
    std::function<bool(bool)> on_set_minimal_gamestate_testing;    // Set MinimalGameState testing
    
    // Multi-client testing data structures
    // NetworkStats struct removed - network stats handled by LocalNetworkAdapter
    
    // RollbackStats is now defined at global scope
    
    // Save state profile removed - now using optimized FastGameState system
    
    // ======= Multi-Client Testing Infrastructure =======
    
    // Multi-client process management  
    std::function<bool(const std::string&)> on_launch_local_client1;     // Launch first client as host
    std::function<bool(const std::string&)> on_launch_local_client2;     // Launch second client as guest
    std::function<bool()> on_terminate_all_clients;                      // Kill all launched clients
    std::function<bool(uint32_t&, uint32_t&)> on_get_client_status;      // Get client process IDs (client1_pid, client2_pid)
    
    // Network simulation callbacks removed - not connected to LocalNetworkAdapter
    
    // Rollback monitoring  
    std::function<bool(RollbackStats&)> on_get_rollback_stats;           // Get rollback performance data
    
    // Data binding
    void SetGames(const std::vector<FM2K::FM2KGameInfo>& games);
    void SetNetworkConfig(const NetworkConfig& config);
    void SetNetworkStats(const GekkoNetworkStats& stats);
    void SetLauncherState(LauncherState state);
    void SetFramesAhead(float frames_ahead);
    // Update scanning progress (0-1). Only meaningful while scanning flag is true.
    void SetScanning(bool scanning);
    void SetGamesRootPath(const std::string& path);

private:
    // Logging
    void AddLog(const char* message);
    void ClearLog();
    static void SDLCustomLogOutput(void* userdata, int category, SDL_LogPriority priority, const char* message);

    // UI state
    std::vector<FM2K::FM2KGameInfo> games_;
    NetworkConfig network_config_;
    GekkoNetworkStats network_stats_;
    float frames_ahead_;
    LauncherState launcher_state_;
    SDL_Renderer* renderer_;
    SDL_Window* window_;
    std::string games_root_path_;  // Current games root directory
    int selected_game_index_ = -1; // -1 means no selection
    bool scanning_games_ = false;  // True while background discovery is running
    
    // Console Log
    ImGuiTextBuffer log_buffer_;
    SDL_Mutex* log_buffer_mutex_;
    bool scroll_to_bottom_;
    SDL_LogOutputFunction original_log_function_;
    void* original_log_userdata_;

    // UI components
    void RenderGameSelection();
    void RenderNetworkConfig();
    void RenderConnectionStatus();
    void RenderInGameUI();
    void RenderMenuBar();
    void RenderSessionControls();
    void RenderDebugTools();
    void RenderSaveStateTools();        // Save state management tab
    void RenderMultiClientTools();      // Multi-client testing tab
    void RenderNetworkTools();          // Network simulation tab
    void RenderPerformanceStats();      // Performance statistics tab
    
    // Helper methods
    void ShowGameValidationStatus(const FM2K::FM2KGameInfo& game);
    void ShowNetworkDiagnostics();
    bool ValidateNetworkConfig();
    
    enum class UITheme { Dark, Light, System, DarkCyan };
    void SetTheme(UITheme theme);
    UITheme current_theme_;

    // int selected_game_index_ = -1; // -1 means no selection
    // bool scanning_games_ = false;  // True while background discovery is running

private:
    void ApplyDarkCyanThemeStyle();
}; 