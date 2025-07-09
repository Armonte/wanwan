#pragma once
#include <windows.h>

namespace argentum::hooks::core {

/**
 * Centralized game addresses extracted from hook.cpp
 * All addresses are offsets from the game's base address
 */
struct GameAddresses {
    // ===========================================
    // TIMING SYSTEM ADDRESSES
    // ===========================================
    static constexpr DWORD TIME_STALL = 0x2d840;
    static constexpr DWORD MAIN_GAME_LOOP = 0x11030;          // MainGameLoop_todo at 0x411030
    static constexpr DWORD BATTLE_GAME_LOOP = 0x1DEE0;        // BattleGameLoop at 0x41DEE0
    
    // ===========================================
    // GRAPHICS SYSTEM ADDRESSES
    // ===========================================
    
    // DirectDraw to SDL3 Migration
    static constexpr DWORD INIT_DIRECTDRAW = 0x6580;          // 0x406580 - initDirectDraw
    static constexpr DWORD CREATE_MAIN_WINDOW = 0x5EF0;       // 0x405EF0 - CreateMainWindow
    static constexpr DWORD UPDATE_COLOR_INFORMATION = 0x126C0; // 0x4126C0 - UpdateColorInformation
    static constexpr DWORD INITIALIZE_RESOURCE_HANDLERS = 0x12670; // 0x412670 - initializeResourceHandlers
    static constexpr DWORD PROCESS_SCREEN_UPDATES = 0x124D0;   // 0x4124D0 - ProcessScreenUpdatesAndResources
    
    // Palette Management
    static constexpr DWORD GET_PALETTE_ENTRY = 0x2BBF0;       // 0x42BBF0 - GetPaletteEntry
    static constexpr DWORD UPDATE_PALETTE_ENTRIES = 0x2BA10;  // 0x42BA10 - UpdatePaletteEntries
    
    // Additional SDL3-compatible functions
    static constexpr DWORD INITIALIZE_WINDOW = 0x2D440;       // 0x42D440 - InitializeWindow
    static constexpr DWORD IS_GRAPHICS_INITIALIZED = 0x2D400; // 0x42D400 - isGraphicsSystemInitialized
    
    // ===========================================
    // SPRITE SYSTEM ADDRESSES
    // ===========================================
    static constexpr DWORD ADD_FRM_SPRITE_TO_RENDER_BUFFER = 0x2CD40; // 0x42CD40
    static constexpr DWORD INTERNAL_FRM_SPRITE = 0x2F650;     // 0x42F650 - InternalFrmSprite
    static constexpr DWORD DISPLAY_FONT_SPRITE = 0x14a9a;     // Display font sprite hook location
    
    // ===========================================
    // RESOURCE MANAGEMENT ADDRESSES
    // ===========================================
    static constexpr DWORD REALLOCATE_GLOBAL_RESOURCE_ARRAY = 0x2CBC0; // 0x42CBC0
    static constexpr DWORD REALLOCATE_RENDER_BUFFER = 0x2CCC0;         // 0x42CCC0
    static constexpr DWORD RESET_RESOURCE_COUNTER = 0x2CC10;           // 0x42CC10
    static constexpr DWORD CLEANUP_RESOURCES = 0x2CC20;                // 0x42CC20
    static constexpr DWORD UPDATE_POINTER_ARRAY = 0x2CE10;             // 0x42CE10
    
    // ===========================================
    // ANIMATION CONTROL ADDRESSES
    // ===========================================
    static constexpr DWORD CLEAR_GLOBAL_ANIM_CONTROL = 0x2CC40;       // Need to verify - estimated
    static constexpr DWORD UPDATE_RENDER_STATE = 0x2CC50;             // 0x42CC50
    static constexpr DWORD RESET_GAME_VARIABLE_TODO = 0x2CC30;        // 0x42CC30
    
    // ===========================================
    // VSE DATA PROCESSING ADDRESSES
    // ===========================================
    static constexpr DWORD PROCESS_VSE_DATA = 0x11680;        // 0x411680 - process_VSE_Data
    static constexpr DWORD PROCESS_VSE_ENTRY = 0x2FB70;       // 0x42FB70 - processVSEentry
    
    // ===========================================
    // INPUT SYSTEM ADDRESSES
    // ===========================================
    static constexpr DWORD PROCESS_JOYSTICK_INPUT = 0x1129A;  // 0x0041129A - call to ProcessJoystickInput
    
    // ===========================================
    // COMPATIBILITY FIX ADDRESSES
    // ===========================================
    static constexpr DWORD TITLE_SCREEN_DEMO_COUNTDOWN = 0x14AAF; // 0x414AAF - demo mode countdown
    static constexpr DWORD DOUBLE_INSTANCE_CHECK = 0x0;           // Pattern-based, no fixed address
    
    // ===========================================
    // FULL SCREEN CRASH FIX ADDRESSES
    // ===========================================
    // These are pattern-based patches, not fixed addresses
    static constexpr DWORD FULLSCREEN_CRASH_FIX_1 = 0x12522 - 0x124fd; // Relative offset
    static constexpr DWORD FULLSCREEN_CRASH_FIX_2 = 0x12596 - 0x12584; // Relative offset
    static constexpr DWORD FULLSCREEN_CRASH_FIX_3 = 0x126a0 - 0x12686; // Relative offset
    static constexpr DWORD FULLSCREEN_CRASH_FIX_4 = 0x126AE - 0x12686; // Relative offset
};

/**
 * Pattern definitions for pattern-based patches
 */
struct GamePatterns {
    // Double instance check pattern
    static constexpr unsigned char DOUBLE_INSTANCE_PATTERN[] = {
        0x8B, 0xF0, 0x85, 0xF6, 0x74, 0x63, 0xC7, 0x44, 0x24, 0x04, 0x2C, 0x00, 0x00, 0x00
    };
    static constexpr size_t DOUBLE_INSTANCE_PATTERN_SIZE = sizeof(DOUBLE_INSTANCE_PATTERN);
    static constexpr ptrdiff_t DOUBLE_INSTANCE_PATCH_OFFSET = 0x406A90 - 0x406AA5; // Negative offset
    
    // Full screen crash fix patterns
    static constexpr unsigned char FULLSCREEN_PATTERN_1[] = {
        0x89, 0x44, 0x24, 0x10, 0x89, 0x44, 0x24, 0x0c, 0x8d, 0x44, 0x24, 0x0c
    };
    
    static constexpr unsigned char FULLSCREEN_PATTERN_2[] = {
        0x8d, 0x44, 0x24, 0x34, 0x6a, 0x00, 0x6a, 0x00, 0x8b, 0x0d
    };
    
    static constexpr unsigned char FULLSCREEN_PATTERN_3[] = {
        0x68, 0x00, 0x00, 0x00, 0x01, 0xc7, 0x84, 0x24, 0xa8, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00
    };
    
    static constexpr unsigned char FULLSCREEN_PATTERN_4[] = {
        0x68, 0x00, 0x00, 0x00, 0x01, 0xc7, 0x84, 0x24, 0xa8, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00
    };
};

/**
 * Hook type enumeration for standardized hook installation
 */
enum class HookType {
    CALL,
    JMP,
    PATTERN_CALL,
    PATTERN_JMP
};

/**
 * Hook installation information structure
 */
struct HookInfo {
    const char* name;
    DWORD address;
    void* newFunction;
    HookType type;
    size_t nopCount;
    const unsigned char* pattern;
    size_t patternSize;
    ptrdiff_t patchOffset;
};

} // namespace argentum::hooks::core