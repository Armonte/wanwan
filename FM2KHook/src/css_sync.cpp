#include "css_sync.h"
#include "globals.h"
#include "gekkonet.h"
#include <SDL3/SDL_log.h>
#include <cstring>

// *** RUNTIME VERIFIED CSS ADDRESSES ***
namespace FM2K {
namespace CharSelect {
namespace Memory {
    // CURSOR POSITIONS (Runtime verified - 8 bytes each: X=4bytes, Y=4bytes)
    constexpr uintptr_t P1_SELECTION_CURSOR_ADDR = 0x424E50;  // P1 cursor: X=2, Y=1 confirmed working
    constexpr uintptr_t P2_SELECTION_CURSOR_ADDR = 0x424E58;  // P2 cursor: X=3, Y=1 confirmed working  
    constexpr uintptr_t P1_STAGE_X_ADDR = 0x424E68;          // g_p1_stage_x (single player mode)
    constexpr uintptr_t P1_STAGE_Y_ADDR = 0x424E6C;          // g_p1_stage_y (single player mode)
    
    // CHARACTER SELECTION AND CONFIRMATION (IDA Pro verified)
    constexpr uintptr_t PLAYER_CHARACTER_SELECTION_ADDR = 0x470020;  // g_player_character_selection[2]
    constexpr uintptr_t P1_CONFIRMED_STATUS_ADDR = 0x47019C;         // g_css_p1_confirmed
    constexpr uintptr_t P2_CONFIRMED_STATUS_ADDR = 0x4701A0;         // g_css_p2_confirmed
    
    // VARIANT/COLOR (NEW - From FM2K_Integration.h)
    constexpr uintptr_t P1_VARIANT_ADDR = 0x47019C;
    constexpr uintptr_t P2_VARIANT_ADDR = 0x4701A0;
    constexpr uintptr_t P1_COLOR_ADDR = 0x4701A4;
    constexpr uintptr_t P2_COLOR_ADDR = 0x4701A8;
}
namespace Constants {
    constexpr uint32_t SELECT_CHARA = 0;
    constexpr uint32_t CHARA_CONFIRMED = 1;
    constexpr uint32_t FULLY_READY = 2;
    constexpr uint32_t CSS_LOCKOUT_FRAMES = 150;
    constexpr uint32_t MODE_CHANGE_LOCKOUT = 2;
    constexpr uint32_t BUTTON_HISTORY_FRAMES = 3;
}
}
}

namespace FM2K {
namespace CSS {

// Global instance
CharSelectSync g_css_sync;

CharSelectSync::CharSelectSync()
    : local_state_{}
    , remote_state_{}
    , last_sent_state_{}
    , in_sync_(true)
    , desync_frames_(0)
    , last_sync_frame_(0)
    , confirmation_sent_(false)
    , confirmation_received_(false)
{
}

void CharSelectSync::Update() {
    // DEBUG: Log CSS update frequency
    static uint32_t css_update_count = 0;
    css_update_count++;
    
    // Only operate during character select phase
    if (State::g_game_state_machine.GetCurrentPhase() != State::GamePhase::CHARACTER_SELECT) {
        // Periodic logging when not in CSS
        if (css_update_count % 600 == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS_DEBUG: Not in CHARACTER_SELECT phase (phase=%d), skipping CSS update", 
                       static_cast<int>(State::g_game_state_machine.GetCurrentPhase()));
        }
        return;
    }
    
    // Read current state from memory
    local_state_ = ReadCurrentState();
    
    // DEBUG: Log CSS state periodically (reduced frequency)
    if (css_update_count % 600 == 0) { // Every 10 seconds instead of 3
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                   "CSS_STATE: P1=(%d,%d) P2=(%d,%d) chars=(%d,%d) confirmed=(%d,%d) gekko=%s session=%s", 
                   local_state_.p1_cursor_x, local_state_.p1_cursor_y,
                   local_state_.p2_cursor_x, local_state_.p2_cursor_y,
                   local_state_.p1_character, local_state_.p2_character,
                   local_state_.p1_confirmed, local_state_.p2_confirmed,
                   gekko_initialized ? "YES" : "NO",
                   gekko_session_started ? "YES" : "NO");
    }
    
    // Update the state machine with current CSS state
    State::g_game_state_machine.UpdateCharacterSelect(local_state_);
    
    // Only sync if we're in a network session
    if (!gekko_initialized || !gekko_session_started) {
        if (css_update_count % 300 == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "CSS_DEBUG: No GekkoNet session active, CSS sync disabled");
        }
        return;
    }
    
    // SIMPLIFIED: Use lockstep synchronization for character select
    ApplyLockstepSync();
    
    // Handle character confirmation synchronization
    HandleCharacterConfirmation();
}

State::CharacterSelectState CharSelectSync::ReadCurrentState() {
    State::CharacterSelectState state;
    
    // DEBUG: Add detailed memory reading diagnostics
    static uint32_t debug_read_count = 0;
    debug_read_count++;
    bool should_debug = (debug_read_count % 300 == 0); // Every 5 seconds
    
    // Read cursor positions from VERIFIED addresses
    if (!IsBadReadPtr((void*)FM2K::CharSelect::Memory::P1_SELECTION_CURSOR_ADDR, sizeof(uint32_t) * 2)) {
        // p1_selection_cursor is 8 bytes: [x,y] as 32-bit integers
        uint32_t* p1_cursor = (uint32_t*)FM2K::CharSelect::Memory::P1_SELECTION_CURSOR_ADDR;
        state.p1_cursor_x = p1_cursor[0];
        state.p1_cursor_y = p1_cursor[1];
        
        // TESTING: Force cursor movement if we detect inputs
        if (::live_p1_input != 0 && should_debug) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "CSS_TEST: P1 input detected=0x%02X, current cursor=(%d,%d)", 
                       ::live_p1_input & 0xFF, state.p1_cursor_x, state.p1_cursor_y);
        }
        
        if (should_debug) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "CSS_MEMORY_DEBUG: P1 cursor read SUCCESS - addr=0x%08X, x=%d, y=%d", 
                       FM2K::CharSelect::Memory::P1_SELECTION_CURSOR_ADDR, state.p1_cursor_x, state.p1_cursor_y);
        }
    } else {
        state.p1_cursor_x = 0;
        state.p1_cursor_y = 0;
        if (should_debug) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                       "CSS_MEMORY_DEBUG: P1 cursor read FAILED - IsBadReadPtr returned true for addr=0x%08X", 
                       FM2K::CharSelect::Memory::P1_SELECTION_CURSOR_ADDR);
        }
    }
    
    if (!IsBadReadPtr((void*)FM2K::CharSelect::Memory::P2_SELECTION_CURSOR_ADDR, sizeof(uint32_t) * 2)) {
        // p2_selection_cursor is 8 bytes: [x,y] as 32-bit integers
        uint32_t* p2_cursor = (uint32_t*)FM2K::CharSelect::Memory::P2_SELECTION_CURSOR_ADDR;
        state.p2_cursor_x = p2_cursor[0];
        state.p2_cursor_y = p2_cursor[1];
        
        if (should_debug) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "CSS_MEMORY_DEBUG: P2 cursor read SUCCESS - addr=0x%08X, x=%d, y=%d", 
                       FM2K::CharSelect::Memory::P2_SELECTION_CURSOR_ADDR, state.p2_cursor_x, state.p2_cursor_y);
        }
    } else {
        state.p2_cursor_x = 0;
        state.p2_cursor_y = 0;
        if (should_debug) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                       "CSS_MEMORY_DEBUG: P2 cursor read FAILED - IsBadReadPtr returned true for addr=0x%08X", 
                       FM2K::CharSelect::Memory::P2_SELECTION_CURSOR_ADDR);
        }
    }
    
    // Read character selections from g_player_character_selection[2]
    if (!IsBadReadPtr((void*)FM2K::CharSelect::Memory::PLAYER_CHARACTER_SELECTION_ADDR, sizeof(uint32_t) * 2)) {
        uint32_t* character_selection = (uint32_t*)FM2K::CharSelect::Memory::PLAYER_CHARACTER_SELECTION_ADDR;
        state.p1_character = character_selection[0];
        state.p2_character = character_selection[1];
        
        if (should_debug) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "CSS_MEMORY_DEBUG: Character selection read SUCCESS - addr=0x%08X, p1_char=%d, p2_char=%d", 
                       FM2K::CharSelect::Memory::PLAYER_CHARACTER_SELECTION_ADDR, state.p1_character, state.p2_character);
        }
    } else {
        state.p1_character = 0;
        state.p2_character = 0;
        if (should_debug) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                       "CSS_MEMORY_DEBUG: Character selection read FAILED - addr=0x%08X", 
                       FM2K::CharSelect::Memory::PLAYER_CHARACTER_SELECTION_ADDR);
        }
    }
    
    // Read confirmation status from VERIFIED addresses
    if (!IsBadReadPtr((void*)FM2K::CharSelect::Memory::P1_CONFIRMED_STATUS_ADDR, sizeof(uint32_t))) {
        state.p1_confirmed = *(uint32_t*)FM2K::CharSelect::Memory::P1_CONFIRMED_STATUS_ADDR;
    } else {
        state.p1_confirmed = 0;
    }
    
    if (!IsBadReadPtr((void*)FM2K::CharSelect::Memory::P2_CONFIRMED_STATUS_ADDR, sizeof(uint32_t))) {
        state.p2_confirmed = *(uint32_t*)FM2K::CharSelect::Memory::P2_CONFIRMED_STATUS_ADDR;
    } else {
        state.p2_confirmed = 0;
    }
    
    // DEBUG: Log confirmation status occasionally
    if (should_debug) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                   "CSS_MEMORY_DEBUG: Confirmation status - p1_confirmed=%d, p2_confirmed=%d", 
                   state.p1_confirmed, state.p2_confirmed);
    }
    
    // Set other fields to defaults for now
    state.selected_stage = 0;
    
    // Read variant and color
    if (!IsBadReadPtr((void*)FM2K::CharSelect::Memory::P1_VARIANT_ADDR, sizeof(uint32_t))) {
        state.p1_variant = *(uint32_t*)FM2K::CharSelect::Memory::P1_VARIANT_ADDR;
    } else {
        state.p1_variant = 0;
    }
    if (!IsBadReadPtr((void*)FM2K::CharSelect::Memory::P2_VARIANT_ADDR, sizeof(uint32_t))) {
        state.p2_variant = *(uint32_t*)FM2K::CharSelect::Memory::P2_VARIANT_ADDR;
    } else {
        state.p2_variant = 0;
    }
    if (!IsBadReadPtr((void*)FM2K::CharSelect::Memory::P1_COLOR_ADDR, sizeof(uint32_t))) {
        state.p1_color = *(uint32_t*)FM2K::CharSelect::Memory::P1_COLOR_ADDR;
    } else {
        state.p1_color = 0;
    }
    if (!IsBadReadPtr((void*)FM2K::CharSelect::Memory::P2_COLOR_ADDR, sizeof(uint32_t))) {
        state.p2_color = *(uint32_t*)FM2K::CharSelect::Memory::P2_COLOR_ADDR;
    } else {
        state.p2_color = 0;
    }
    
    // DEBUG: Log memory reads occasionally to verify addresses are working
    static uint32_t read_count = 0;
    read_count++;
    if (read_count % 1200 == 0) { // Every 20 seconds instead of 10
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                   "CSS_MEMORY_READ_VERIFIED: P1_cursor=(%d,%d) P2_cursor=(%d,%d) chars=(%d,%d) confirmed=(%d,%d) p1_color=%d p2_color=%d", 
                   state.p1_cursor_x, state.p1_cursor_y,
                   state.p2_cursor_x, state.p2_cursor_y,
                   state.p1_character, state.p2_character,
                   state.p1_confirmed, state.p2_confirmed,
                   state.p1_color, state.p2_color);
    }
    
    return state;
}

void CharSelectSync::ApplyLockstepSync() {
    // CCCASTER-STYLE LOCKSTEP: Implement proper CSS input management
    
    // 1. Log significant state changes (reduced frequency)
    static State::CharacterSelectState last_logged_state = {};
    static uint32_t log_counter = 0;
    log_counter++;
    
    bool state_changed = (memcmp(&local_state_, &last_logged_state, sizeof(State::CharacterSelectState)) != 0);
    
    if (state_changed) {
        // Only log every 60 frames (1 second at 60fps) to reduce spam
        if (log_counter % 60 == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "CSS Lockstep: P1_cursor=(%d,%d) P2_cursor=(%d,%d) chars=(%d,%d) confirmed=(%d,%d) p1_color=%d p2_color=%d",
                local_state_.p1_cursor_x, local_state_.p1_cursor_y,
                local_state_.p2_cursor_x, local_state_.p2_cursor_y,
                local_state_.p1_character, local_state_.p2_character,
                local_state_.p1_confirmed, local_state_.p2_confirmed,
                local_state_.p1_color, local_state_.p2_color);
        }
            
        last_logged_state = local_state_;
        in_sync_ = true;
        desync_frames_ = 0;
    }
    
    // 2. CRITICAL: Handle CSS input processing like CCCaster
    ProcessCSSInputs();
    
    // 3. REMOVED: ApplyRemoteStateSynchronization() - GekkoNet handles input sync automatically
    // The CSS sync should only monitor state changes, not interfere with input processing
}

void CharSelectSync::ProcessCSSInputs() {
    // COMPREHENSIVE CCCASTER-STYLE CSS INPUT PROCESSING
    // Based on CCCaster's getCharaSelectInput() method with timing validation
    
    if (!gekko_session_started) {
        return;
    }
    
    // Track frames in CSS for timing controls
    static uint32_t css_frames = 0;
    css_frames++;
    
    // Update comprehensive state tracking
    UpdateCSSTimingAndValidation(css_frames);
    
    // IMPORTANT: Do NOT write to FM2K input memory directly during CSS
    // Hook_GetPlayerInput handles all input routing correctly like dllmain_orig.cpp
    
    // CRITICAL FIX: Each client should only capture their LOCAL player input
    // Host controls P1, Client controls P2 (like GekkoNet example)
    
    // CSS sync should NOT interfere with input capture - Hook_GetPlayerInput handles this
    // The CSS sync only needs to monitor the state, not capture inputs
    
    // DEBUG: Log the inputs being captured (but don't modify them)
    uint8_t local_player_num = ::is_host ? 1 : 2;
    uint32_t local_input = (local_player_num == 1) ? ::live_p1_input : ::live_p2_input;
    
    static uint32_t monitor_count = 0;
    if (local_input != 0 && (++monitor_count % 300 == 0)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                   "CSS_INPUT_MONITOR: P%d local_input=0x%02X (frames=%u)", 
                   local_player_num, local_input & 0xFF, css_frames);
    }
    
    // Update button history for debugging
    UpdateButtonHistory(local_player_num, local_input);
    
    // Log state periodically (reduced frequency)
    static uint32_t debug_count = 0;
    debug_count++;
    if (debug_count % 600 == 0) { // Every 10 seconds instead of 3
        LogCSSInputState(css_frames);
    }
}

// ===== CCCASTER-STYLE INPUT VALIDATION METHODS =====

void CharSelectSync::UpdateCSSTimingAndValidation(uint32_t css_frames) {
    // Update the comprehensive state tracking following CCCaster's approach
    
    // For now, assume both players are in SELECT_CHARA mode (we don't have selector mode addresses yet)
    uint32_t p1_mode = FM2K::CharSelect::Constants::SELECT_CHARA;
    uint32_t p2_mode = FM2K::CharSelect::Constants::SELECT_CHARA;
    
    // Update enhanced state tracking
    local_state_.frames_in_css = css_frames;
    local_state_.p1_selection_mode = static_cast<State::CharacterSelectState::SelectionMode>(p1_mode);
    local_state_.p2_selection_mode = static_cast<State::CharacterSelectState::SelectionMode>(p2_mode);
    
    // Calculate input validation flags (CCCaster-style timing controls)
    local_state_.p1_can_confirm = CanPlayerConfirm(1, css_frames);
    local_state_.p2_can_confirm = CanPlayerConfirm(2, css_frames);
    local_state_.p1_can_cancel = CanPlayerCancel(1, css_frames);
    local_state_.p2_can_cancel = CanPlayerCancel(2, css_frames);
    
    // Update state checksum for sync validation
    local_state_.checksum = local_state_.CalculateChecksum();
    local_state_.sync_frame = css_frames;
}

uint32_t CharSelectSync::ValidateAndFilterCSSInput(uint32_t raw_input, uint8_t player, uint32_t css_frames) {
    // CCCaster-style input validation with timing controls
    // CRITICAL: Only filter confirm/cancel buttons, PRESERVE directional inputs
    
    uint32_t filtered_input = raw_input;
    
    // TEMPORARILY DISABLE ALL FILTERING TO TEST DIRECTIONAL INPUTS
    // Only log what we would filter, but don't actually filter anything yet
    
    // 1. INITIAL LOCKOUT: Would prevent Confirm until 150f after beginning
    if (css_frames < FM2K::CharSelect::Constants::CSS_LOCKOUT_FRAMES) {
        if (raw_input & (0x10 | 0x20)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "CSS_LOCKOUT_TEST: P%d confirm would be blocked (frame %u < %u) but allowing for testing", 
                       player, css_frames, FM2K::CharSelect::Constants::CSS_LOCKOUT_FRAMES);
        }
    }
    
    // 2. SELECTION MODE VALIDATION: Would prevent exiting character select inappropriately
    uint32_t player_mode = (player == 1) ? static_cast<uint32_t>(local_state_.p1_selection_mode) 
                                          : static_cast<uint32_t>(local_state_.p2_selection_mode);
    
    if (player_mode == FM2K::CharSelect::Constants::SELECT_CHARA) {
        if (raw_input & (0x02 | 0x40)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "CSS_MODE_TEST: P%d cancel would be blocked (in SELECT_CHARA mode) but allowing for testing", player);
        }
    }
    
    // 3. BUTTON HISTORY VALIDATION: Would prevent rapid inputs
    if (HasRecentButtonInHistory(player, 0x10 | 0x20 | 0x02 | 0x40, 1, FM2K::CharSelect::Constants::MODE_CHANGE_LOCKOUT)) {
        if (raw_input & (0x10 | 0x20 | 0x02 | 0x40)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "CSS_HISTORY_TEST: P%d buttons would be blocked (recent history conflict) but allowing for testing", player);
        }
    }
    
    // DEBUG: Log all inputs to see what we're getting
    if (raw_input != 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                   "CSS_INPUT_TEST: P%d raw=0x%02X ALLOWING ALL for cursor movement testing", 
                   player, raw_input & 0xFF);
    }
    
    // RETURN UNFILTERED INPUT FOR NOW
    return filtered_input;
}

bool CharSelectSync::IsInInputLockout(uint32_t css_frames) {
    return css_frames < FM2K::CharSelect::Constants::CSS_LOCKOUT_FRAMES;
}

bool CharSelectSync::CanPlayerConfirm(uint8_t player, uint32_t css_frames) {
    // CCCaster-style confirmation validation
    
    // 1. Must pass initial lockout period
    if (css_frames < FM2K::CharSelect::Constants::CSS_LOCKOUT_FRAMES) {
        return false;
    }
    
    // 2. Must not have recent button conflicts
    if (HasRecentButtonInHistory(player, 0x10 | 0x20, 1, FM2K::CharSelect::Constants::MODE_CHANGE_LOCKOUT)) {
        return false;
    }
    
    // 3. Player must be in valid selection state
    uint32_t player_mode = (player == 1) ? static_cast<uint32_t>(local_state_.p1_selection_mode) 
                                          : static_cast<uint32_t>(local_state_.p2_selection_mode);
    
    return player_mode == FM2K::CharSelect::Constants::SELECT_CHARA;
}

bool CharSelectSync::CanPlayerCancel(uint8_t player, uint32_t css_frames) {
    // CCCaster-style cancel validation
    
    // 1. Must not have recent button conflicts 
    if (HasRecentButtonInHistory(player, 0x02 | 0x40, 1, FM2K::CharSelect::Constants::MODE_CHANGE_LOCKOUT)) {
        return false;
    }
    
    // 2. Player must not be locked into character selection
    uint32_t player_mode = (player == 1) ? static_cast<uint32_t>(local_state_.p1_selection_mode) 
                                          : static_cast<uint32_t>(local_state_.p2_selection_mode);
    
    // Prevent cancel when actively selecting character (CCCaster behavior)
    return player_mode != FM2K::CharSelect::Constants::SELECT_CHARA;
}

void CharSelectSync::UpdateButtonHistory(uint8_t player, uint32_t input) {
    // Store button input history for validation (simple ring buffer approach)
    static uint32_t p1_history[FM2K::CharSelect::Constants::BUTTON_HISTORY_FRAMES] = {0};
    static uint32_t p2_history[FM2K::CharSelect::Constants::BUTTON_HISTORY_FRAMES] = {0};
    static uint32_t history_index = 0;
    
    uint32_t* history = (player == 1) ? p1_history : p2_history;
    history[history_index % FM2K::CharSelect::Constants::BUTTON_HISTORY_FRAMES] = input;
    
    // Only increment index once per frame (for both players)
    static uint32_t last_update_frame = 0;
    if (last_update_frame != local_state_.frames_in_css) {
        history_index++;
        last_update_frame = local_state_.frames_in_css;
    }
}

bool CharSelectSync::HasRecentButtonInHistory(uint8_t player, uint32_t button_mask, uint32_t start_offset, uint32_t end_offset) {
    // Check if player pressed specific buttons within the given frame range
    // Similar to CCCaster's hasButtonInHistory() method
    
    static uint32_t p1_history[FM2K::CharSelect::Constants::BUTTON_HISTORY_FRAMES] = {0};
    static uint32_t p2_history[FM2K::CharSelect::Constants::BUTTON_HISTORY_FRAMES] = {0};
    static uint32_t history_index = 0;
    
    uint32_t* history = (player == 1) ? p1_history : p2_history;
    
    // Check recent history for button presses
    for (uint32_t offset = start_offset; offset <= end_offset; offset++) {
        uint32_t check_index = (history_index - offset) % FM2K::CharSelect::Constants::BUTTON_HISTORY_FRAMES;
        if (history[check_index] & button_mask) {
            return true;
        }
    }
    
    return false;
}

void CharSelectSync::LogCSSInputState(uint32_t css_frames) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
               "CSS_INPUT_STATE: frames=%u P1(mode=%d confirm=%s cancel=%s) P2(mode=%d confirm=%s cancel=%s)",
               css_frames,
               static_cast<int>(local_state_.p1_selection_mode), 
               local_state_.p1_can_confirm ? "OK" : "NO",
               local_state_.p1_can_cancel ? "OK" : "NO",
               static_cast<int>(local_state_.p2_selection_mode),
               local_state_.p2_can_confirm ? "OK" : "NO", 
               local_state_.p2_can_cancel ? "OK" : "NO");
}

void CharSelectSync::ReceiveRemoteConfirmation() {
    // Set the internal flag
    confirmation_received_ = true;
    
    // CRITICAL: Write to FM2K's memory to register the remote player confirmation
    if (::is_host) {
        // Host: Set P2 confirmation in FM2K memory
        if (!IsBadWritePtr((void*)FM2K::CharSelect::Memory::P2_CONFIRMED_STATUS_ADDR, sizeof(uint32_t))) {
            *(uint32_t*)FM2K::CharSelect::Memory::P2_CONFIRMED_STATUS_ADDR = 1;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS_CONFIRM: Host set P2 confirmation=1 in FM2K memory");
        }
    } else {
        // Client: Set P1 confirmation in FM2K memory  
        if (!IsBadWritePtr((void*)FM2K::CharSelect::Memory::P1_CONFIRMED_STATUS_ADDR, sizeof(uint32_t))) {
            *(uint32_t*)FM2K::CharSelect::Memory::P1_CONFIRMED_STATUS_ADDR = 1;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS_CONFIRM: Client set P1 confirmation=1 in FM2K memory");
        }
    }
}

void CharSelectSync::HandleCharacterConfirmation() {
    if (!gekko_session_started) return;

    // CRITICAL FIX: Only allow the LOCAL player to confirm, not both players
    // Host can only confirm P1, Client can only confirm P2
    
    // 1. Check if the local player has confirmed their selection
    bool local_player_confirmed = false;
    uint8_t local_player_num = ::is_host ? 1 : 2;
    
    if (::is_host) {
        // Host can only confirm P1
        local_player_confirmed = (local_state_.p1_confirmed == 1);
        if (local_state_.p2_confirmed == 1) {
            // CRITICAL: Reset P2 confirmation if host somehow triggered it
            if (!IsBadWritePtr((void*)0x4701A0, sizeof(uint32_t))) {
                *(uint32_t*)0x4701A0 = 0;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "CSS_CONFIRM_FIX: Host reset invalid P2 confirmation");
            }
        }
    } else {
        // Client can only confirm P2
        local_player_confirmed = (local_state_.p2_confirmed == 1);
        if (local_state_.p1_confirmed == 1) {
            // CRITICAL: Reset P1 confirmation if client somehow triggered it
            if (!IsBadWritePtr((void*)0x47019C, sizeof(uint32_t))) {
                *(uint32_t*)0x47019C = 0;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "CSS_CONFIRM_FIX: Client reset invalid P1 confirmation");
            }
        }
    }

    // 2. If confirmed and we haven't sent our signal yet, send it.
    if (local_player_confirmed && !confirmation_sent_) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Local player %d confirmed. Sending 0xFF signal.", local_player_num);
        confirmation_sent_ = true;
        
        // This is the special input signal - ONLY send once per CSS session
        uint8_t confirmation_input = 0xFF;
        gekko_add_local_input(::gekko_session, ::local_player_handle, &confirmation_input);
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                   "CSS_CONFIRM: Player %d sent 0xFF confirmation to remote", 
                   local_player_num);
    }

    // 3. Check if the handshake is complete.
    if (confirmation_sent_ && confirmation_received_ && !State::g_game_state_machine.IsCharacterSelectionConfirmed()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Handshake complete! Both players confirmed.");
        
        // Tell the state machine it's safe to transition.
        State::g_game_state_machine.ConfirmCharacterSelection();
    }
    
    // 4. DEBUG: Log handshake status periodically (reduced frequency)
    static uint32_t handshake_debug_count = 0;
    handshake_debug_count++;
    if (handshake_debug_count % 600 == 0) { // Every 10 seconds instead of 3
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                   "CSS_HANDSHAKE_STATUS: P%d_confirmed=%s sent=%s received=%s state_confirmed=%s",
                   local_player_num,
                   local_player_confirmed ? "YES" : "NO",
                   confirmation_sent_ ? "YES" : "NO", 
                   confirmation_received_ ? "YES" : "NO",
                   State::g_game_state_machine.IsCharacterSelectionConfirmed() ? "YES" : "NO");
    }
}

void CharSelectSync::SendLocalState() {
    if (!gekko_session) return;
    
    // For lockstep mode, we don't need custom messages
    // GekkoNet handles the synchronization through its normal mechanisms
}

void CharSelectSync::ReceiveRemoteState() {
    if (!gekko_session) return;
    
    // For lockstep mode, we read the remote state directly from memory
    // after GekkoNet has synchronized the game states
}

void CharSelectSync::ApplyRemoteState() {
    // For host: apply P2 state from remote
    // For client: apply P1 state from remote
    
    if (is_host) {
        // Host controls P1, apply remote P2 state
        if (!IsBadWritePtr((void*)FM2K::CharSelect::Memory::P2_SELECTION_CURSOR_ADDR, sizeof(uint32_t) * 2)) {
            uint32_t* p2_cursor = (uint32_t*)FM2K::CharSelect::Memory::P2_SELECTION_CURSOR_ADDR;
            p2_cursor[0] = remote_state_.p2_cursor_x;
            p2_cursor[1] = remote_state_.p2_cursor_y;
        }
        if (!IsBadWritePtr((void*)FM2K::CharSelect::Memory::PLAYER_CHARACTER_SELECTION_ADDR, sizeof(uint32_t) * 2)) {
            uint32_t* character_selection = (uint32_t*)FM2K::CharSelect::Memory::PLAYER_CHARACTER_SELECTION_ADDR;
            character_selection[1] = remote_state_.p2_character;
        }
        if (!IsBadWritePtr((void*)FM2K::CharSelect::Memory::P2_CONFIRMED_STATUS_ADDR, sizeof(uint32_t))) {
            *(uint32_t*)FM2K::CharSelect::Memory::P2_CONFIRMED_STATUS_ADDR = remote_state_.p2_confirmed;
        }
        if (!IsBadWritePtr((void*)FM2K::CharSelect::Memory::P2_VARIANT_ADDR, sizeof(uint32_t))) {
            *(uint32_t*)FM2K::CharSelect::Memory::P2_VARIANT_ADDR = remote_state_.p2_variant;
        }
        if (!IsBadWritePtr((void*)FM2K::CharSelect::Memory::P2_COLOR_ADDR, sizeof(uint32_t))) {
            *(uint32_t*)FM2K::CharSelect::Memory::P2_COLOR_ADDR = remote_state_.p2_color;
        }
    } else {
        // Client controls P2, apply remote P1 state
        if (!IsBadWritePtr((void*)FM2K::CharSelect::Memory::P1_SELECTION_CURSOR_ADDR, sizeof(uint32_t) * 2)) {
            uint32_t* p1_cursor = (uint32_t*)FM2K::CharSelect::Memory::P1_SELECTION_CURSOR_ADDR;
            p1_cursor[0] = remote_state_.p1_cursor_x;
            p1_cursor[1] = remote_state_.p1_cursor_y;
        }
        if (!IsBadWritePtr((void*)FM2K::CharSelect::Memory::PLAYER_CHARACTER_SELECTION_ADDR, sizeof(uint32_t) * 2)) {
            uint32_t* character_selection = (uint32_t*)FM2K::CharSelect::Memory::PLAYER_CHARACTER_SELECTION_ADDR;
            character_selection[0] = remote_state_.p1_character;
        }
        if (!IsBadWritePtr((void*)FM2K::CharSelect::Memory::P1_CONFIRMED_STATUS_ADDR, sizeof(uint32_t))) {
            *(uint32_t*)FM2K::CharSelect::Memory::P1_CONFIRMED_STATUS_ADDR = remote_state_.p1_confirmed;
        }
        if (!IsBadWritePtr((void*)FM2K::CharSelect::Memory::P1_VARIANT_ADDR, sizeof(uint32_t))) {
            *(uint32_t*)FM2K::CharSelect::Memory::P1_VARIANT_ADDR = remote_state_.p1_variant;
        }
        if (!IsBadWritePtr((void*)FM2K::CharSelect::Memory::P1_COLOR_ADDR, sizeof(uint32_t))) {
            *(uint32_t*)FM2K::CharSelect::Memory::P1_COLOR_ADDR = remote_state_.p1_color;
        }
    }
}

void CharSelectSync::ForceResync() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Forcing CSS resync");
    
    // Reset sync state
    in_sync_ = true;
    desync_frames_ = 0;
    confirmation_sent_ = false;
    confirmation_received_ = false;
    
    // Send current state immediately
    SendLocalState();
}

void CharSelectSync::PackStateMessage(CSSStateMessage& msg, const State::CharacterSelectState& state) {
    msg.p1_cursor_x = state.p1_cursor_x;
    msg.p1_cursor_y = state.p1_cursor_y;
    msg.p2_cursor_x = state.p2_cursor_x;
    msg.p2_cursor_y = state.p2_cursor_y;
    msg.p1_selected_char = state.p1_character;
    msg.p2_selected_char = state.p2_character;
    msg.p1_confirmed = state.p1_confirmed;
    msg.p2_confirmed = state.p2_confirmed;
}

void CharSelectSync::UnpackStateMessage(const CSSStateMessage& msg, State::CharacterSelectState& state) {
    state.p1_cursor_x = msg.p1_cursor_x;
    state.p1_cursor_y = msg.p1_cursor_y;
    state.p2_cursor_x = msg.p2_cursor_x;
    state.p2_cursor_y = msg.p2_cursor_y;
    state.p1_character = msg.p1_selected_char;
    state.p2_character = msg.p2_selected_char;
    state.p1_confirmed = msg.p1_confirmed;
    state.p2_confirmed = msg.p2_confirmed;
}

} // namespace CSS
} // namespace FM2K