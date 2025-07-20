#include "css_sync.h"
#include "css_tcp_sync.h"
#include "gekkonet_hooks.h"
#include "globals.h"
#include "gekkonet.h"
#include <SDL3/SDL_log.h>
#include <cstring>

// Simplified CSS addresses - only what we need for confirmation
namespace FM2K {
namespace CharSelect {
namespace Memory {
    constexpr uintptr_t P1_CONFIRMED_STATUS_ADDR = 0x47019C;
    constexpr uintptr_t P2_CONFIRMED_STATUS_ADDR = 0x4701A0;
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
    , handshake_completed_(false)
    , css_frame_count_(0)
{
}

void CharSelectSync::Update() {
    // Only sync if we're in a network session
    if (!gekko_initialized || !gekko_session_started) {
        return;
    }
    
    // Reset CSS frame counter when first entering character select
    static bool css_reset = false;
    if (!css_reset) {
        css_frame_count_ = 0;
        css_reset = true;
    }
    
    // TEMPORARILY DISABLE TCP sync until we fix the crash
    static bool tcp_initialized = false;
    if (false && !tcp_initialized) {  // Disabled for now
        InitializeTCPCursorSync();
        tcp_initialized = true;
    }
    
    // Increment CSS frame counter for input filtering
    css_frame_count_++;
    
    // Read current state for confirmation tracking
    local_state_ = ReadCurrentState();
    
    // TCP Cursor Synchronization - DISABLED until crash is fixed
    // UpdateTCPCursorSync();
    
    // Handle confirmation handshake
    HandleCharacterConfirmation();
}

State::CharacterSelectState CharSelectSync::ReadCurrentState() {
    State::CharacterSelectState state;
    
    // SIMPLIFIED: Only read confirmation status for 0xFF handshake
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
    
    // Set all other fields to defaults - we don't track them
    state.p1_cursor_x = 0;
    state.p1_cursor_y = 0;
    state.p2_cursor_x = 0;
    state.p2_cursor_y = 0;
    state.p1_character = 0;
    state.p2_character = 0;
    state.selected_stage = 0;
    state.p1_variant = 0;
    state.p2_variant = 0;
    state.p1_color = 0;
    state.p2_color = 0;
    
    return state;
}

void CharSelectSync::ApplyLockstepSync() {
    // SIMPLIFIED: Just read confirmation status for handshake
    local_state_ = ReadCurrentState();
    
    // No state logging or complex processing - let GekkoNet handle input sync
    in_sync_ = true;
    desync_frames_ = 0;
}

void CharSelectSync::ProcessCSSInputs() {
    // SIMPLIFIED: No complex CSS input processing
    // Let Hook_GetPlayerInput handle everything
}

void CharSelectSync::SendLocalState() {
    // No need to send CSS state - GekkoNet handles input sync
}

void CharSelectSync::ReceiveRemoteState() {
    // No need to receive CSS state - GekkoNet handles input sync
}

void CharSelectSync::ApplyRemoteState() {
    // No need to apply CSS state - GekkoNet handles input sync
}

void CharSelectSync::ReceiveRemoteConfirmation() {
    confirmation_received_ = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Remote player confirmed character selection");
}

void CharSelectSync::ForceResync() {
    in_sync_ = false;
    desync_frames_ = 0;
}

void CharSelectSync::ResetForNewCSSSession() {
    confirmation_sent_ = false;
    confirmation_received_ = false;
    handshake_completed_ = false;
    css_frame_count_ = 0;
    in_sync_ = true;
    desync_frames_ = 0;
    
    // Keep TCP cursor sync alive for faster reconnection
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Reset for new character select session (TCP sync stays active)");
}

void CharSelectSync::HandleCharacterConfirmation() {
    if (!gekko_session_started || handshake_completed_) return;

    // Check if the local player has confirmed their selection
    bool local_player_confirmed = false;
    uint8_t local_player_num = ::is_host ? 1 : 2;
    
    if (::is_host) {
        local_player_confirmed = (local_state_.p1_confirmed == 1);
    } else {
        local_player_confirmed = (local_state_.p2_confirmed == 1);
    }

    // If confirmed and we haven't sent our signal yet, send 0xFF
    if (local_player_confirmed && !confirmation_sent_) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Local player confirmed. Sending 0xFF signal.");
        confirmation_sent_ = true;
        
        // Send 0xFF confirmation signal through GekkoNet
        uint8_t confirmation_input = 0xFF;
        gekko_add_local_input(gekko_session, local_player_handle, &confirmation_input);
    }

    // Check if handshake is complete (only log once)
    if (confirmation_sent_ && confirmation_received_ && !handshake_completed_) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Handshake complete! Both players confirmed.");
        handshake_completed_ = true;
        // Transition to battle will be handled by game state machine
    }
}

// All the complex CCCaster-style methods are now simplified or removed
void CharSelectSync::UpdateCSSTimingAndValidation(uint32_t css_frames) {
    // Simplified - no complex timing validation
}

uint32_t CharSelectSync::ValidateAndFilterCSSInput(uint32_t raw_input, uint8_t player_num, uint32_t css_frames) {
    // STATELESS CCCaster-style input filtering compatible with rollback netcode
    uint32_t filtered_input = raw_input;
    
    // FM2K Input Masks (11-bit input structure)
    const uint32_t BUTTON1_MASK = 0x10;  // 0001 0000 - Primary confirm button
    const uint32_t BUTTON2_MASK = 0x20;  // 0010 0000 - Secondary confirm/cancel
    const uint32_t BUTTON3_MASK = 0x40;  // 0100 0000 - Additional action button
    const uint32_t BUTTON4_MASK = 0x80;  // 1000 0000 - Extra button
    
    const uint32_t CONFIRM_BUTTONS = BUTTON1_MASK | BUTTON2_MASK;
    const uint32_t ACTION_BUTTONS = BUTTON1_MASK | BUTTON2_MASK | BUTTON3_MASK;
    const uint32_t ALL_BUTTONS = ACTION_BUTTONS | BUTTON4_MASK;
    
    // PHASE 1: Initial lockout period - MINIMAL for testing
    const uint32_t INITIAL_LOCKOUT_FRAMES = 10;  // Minimal lockout for testing
    if (css_frames < INITIAL_LOCKOUT_FRAMES) {
        // Block all confirmation inputs during initial period
        filtered_input &= ~CONFIRM_BUTTONS;
        
        // Log lockout activity (occasionally)
        if (raw_input & CONFIRM_BUTTONS && css_frames % 30 == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                "CSS Input Filter: Blocking confirm input during lockout (frame %u/%u)", 
                css_frames, INITIAL_LOCKOUT_FRAMES);
        }
    }
    
    // PHASE 2: Frame-based input rate limiting (STATELESS approach)
    // Instead of tracking last_input_frame_, use modulo arithmetic for deterministic filtering
    const uint32_t INPUT_RATE_DIVISOR = 2; // Allow inputs every 2 frames (minimal filtering)
    const uint32_t INPUT_RATE_OFFSET = 2;  // Offset >= divisor means no blocking (all frames allowed)
    
    // Only allow action buttons on specific frames to prevent rapid inputs
    if (css_frames >= INITIAL_LOCKOUT_FRAMES) {
        uint32_t frame_in_cycle = css_frames % INPUT_RATE_DIVISOR;
        if (frame_in_cycle < INPUT_RATE_OFFSET) {
            // This condition is now never true (offset >= divisor), so no blocking
            filtered_input &= ~ACTION_BUTTONS;
            
            if (raw_input & ACTION_BUTTONS && css_frames % 30 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS Input Filter P%u: BLOCKING at frame %u, cycle pos %u (input=0x%02X)",
                    player_num, css_frames, frame_in_cycle, raw_input);
            }
        } else {
            // Input allowed - log occasionally for debugging
            if (raw_input & ACTION_BUTTONS && css_frames % 30 == 5) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS Input Filter P%u: ALLOWING at frame %u, cycle pos %u (input=0x%02X)",
                    player_num, css_frames, frame_in_cycle, raw_input);
            }
        }
    }
    
    // PHASE 3: Movement smoothing (STATELESS approach)
    // Prevent simultaneous movement and action to avoid file loading issues
    const uint32_t MOVEMENT_MASK = 0x0F; // left, right, up, down
    bool has_movement = (raw_input & MOVEMENT_MASK) != 0;
    bool has_action = (raw_input & ACTION_BUTTONS) != 0;
    
    if (has_movement && has_action && css_frames >= INITIAL_LOCKOUT_FRAMES) {
        // Use frame parity to determine which input to allow
        if (css_frames % 2 == 0) {
            // Even frames: Allow movement, block actions
            filtered_input &= ~ACTION_BUTTONS;
        } else {
            // Odd frames: Allow actions, block movement
            filtered_input &= ~MOVEMENT_MASK;
        }
    }
    
    // PHASE 4: Special handling for confirm buttons after initial lockout
    // Add extra safety for confirm inputs to prevent accidental selections
    if (css_frames >= INITIAL_LOCKOUT_FRAMES && css_frames < (INITIAL_LOCKOUT_FRAMES + 30)) {
        // Grace period after lockout - only allow confirm on every 3rd frame
        if (css_frames % 3 != 0) {
            filtered_input &= ~CONFIRM_BUTTONS;
        }
    }
    
    // PHASE 5: Validation logging (debug mode)
    if (raw_input != filtered_input) {
        static uint32_t filter_log_counter = 0;
        if (++filter_log_counter % 50 == 0) { // Log every 50th filter event
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                "CSS Input Filter: 0x%04X -> 0x%04X (frame %u, player %u)",
                raw_input, filtered_input, css_frames, player_num);
        }
    }
    
    return filtered_input;
}

void CharSelectSync::InitializeTCPCursorSync() {
    // Initialize TCP cursor sync using GekkoNet connection info
    uint16_t base_port = GetGekkoLocalPort();
    const char* remote_ip = GetGekkoRemoteIP();
    
    // Both should use HOST's base port (7000) for TCP sync
    // Host: server on 7100, Client: connect to host's 7100
    uint16_t tcp_port = 7000;  // Always use host's port as base
    
    bool tcp_success = g_tcp_cursor_sync.Initialize(::is_host, tcp_port, remote_ip);
    if (tcp_success) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: TCP cursor sync initialized successfully - %s on port %u -> %s", 
            ::is_host ? "SERVER" : "CLIENT", tcp_port + 100, remote_ip);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: Failed to initialize TCP cursor sync");
    }
}

void CharSelectSync::UpdateTCPCursorSync() {
    // Safety check - only run during CSS and when connected
    if (!g_tcp_cursor_sync.IsConnected()) return;
    
    // Only update every few frames to reduce load
    static uint32_t update_counter = 0;
    if (++update_counter % 30 != 0) return;  // Update every 30th frame (much less frequent)
    
    // Wait a bit after connection before sending data
    static uint32_t connection_stabilization = 0;
    if (connection_stabilization < 50) {
        connection_stabilization++;
        return;  // Skip first 50 calls to let connection stabilize
    }
    
    try {
        // Read local cursor position from memory
        uint32_t local_cursor_x = 0, local_cursor_y = 0;
        uint32_t local_character = 0, local_confirmed = 0;
        
        if (::is_host) {
            // P1 (host) cursor
            if (!IsBadReadPtr((void*)0x4700CC, sizeof(uint32_t))) local_cursor_x = *(uint32_t*)0x4700CC;
            if (!IsBadReadPtr((void*)0x4700D0, sizeof(uint32_t))) local_cursor_y = *(uint32_t*)0x4700D0;
            if (!IsBadReadPtr((void*)0x4700D4, sizeof(uint32_t))) local_character = *(uint32_t*)0x4700D4;
            if (!IsBadReadPtr((void*)0x47019C, sizeof(uint32_t))) local_confirmed = *(uint32_t*)0x47019C;
        } else {
            // P2 (client) cursor
            if (!IsBadReadPtr((void*)0x470120, sizeof(uint32_t))) local_cursor_x = *(uint32_t*)0x470120;
            if (!IsBadReadPtr((void*)0x470124, sizeof(uint32_t))) local_cursor_y = *(uint32_t*)0x470124;
            if (!IsBadReadPtr((void*)0x470128, sizeof(uint32_t))) local_character = *(uint32_t*)0x470128;
            if (!IsBadReadPtr((void*)0x4701A0, sizeof(uint32_t))) local_confirmed = *(uint32_t*)0x4701A0;
        }
        
        // Send local cursor state to remote
        g_tcp_cursor_sync.SendCursorUpdate(local_cursor_x, local_cursor_y, local_character, local_confirmed);
        
        // Get remote cursor state and apply to memory
        uint32_t remote_cursor_x, remote_cursor_y, remote_character, remote_confirmed;
        g_tcp_cursor_sync.GetRemoteCursor(remote_cursor_x, remote_cursor_y, remote_character, remote_confirmed);
        
        // Apply remote cursor to appropriate player memory (with bounds checking)
        if (::is_host) {
            // Host receives P2 cursor - validate reasonable ranges
            if (remote_cursor_x < 1000 && remote_cursor_y < 1000) {
                if (!IsBadWritePtr((void*)0x470120, sizeof(uint32_t))) *(uint32_t*)0x470120 = remote_cursor_x;
                if (!IsBadWritePtr((void*)0x470124, sizeof(uint32_t))) *(uint32_t*)0x470124 = remote_cursor_y;
            }
            if (remote_character < 100) {
                if (!IsBadWritePtr((void*)0x470128, sizeof(uint32_t))) *(uint32_t*)0x470128 = remote_character;
            }
            if (remote_confirmed <= 1) {
                if (!IsBadWritePtr((void*)0x4701A0, sizeof(uint32_t))) *(uint32_t*)0x4701A0 = remote_confirmed;
            }
        } else {
            // Client receives P1 cursor - validate reasonable ranges
            if (remote_cursor_x < 1000 && remote_cursor_y < 1000) {
                if (!IsBadWritePtr((void*)0x4700CC, sizeof(uint32_t))) *(uint32_t*)0x4700CC = remote_cursor_x;
                if (!IsBadWritePtr((void*)0x4700D0, sizeof(uint32_t))) *(uint32_t*)0x4700D0 = remote_cursor_y;
            }
            if (remote_character < 100) {
                if (!IsBadWritePtr((void*)0x4700D4, sizeof(uint32_t))) *(uint32_t*)0x4700D4 = remote_character;
            }
            if (remote_confirmed <= 1) {
                if (!IsBadWritePtr((void*)0x47019C, sizeof(uint32_t))) *(uint32_t*)0x47019C = remote_confirmed;
            }
        }
    } catch (...) {
        // Catch any exceptions to prevent crashes
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Exception in UpdateTCPCursorSync");
    }
}

void CharSelectSync::ShutdownTCPCursorSync() {
    g_tcp_cursor_sync.Shutdown();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: TCP cursor sync shutdown");
}

bool CharSelectSync::IsInInputLockout(uint32_t css_frames) {
    // Simplified - no lockout
    return false;
}

bool CharSelectSync::CanPlayerConfirm(uint8_t player, uint32_t css_frames) {
    // Simplified - always allow confirmation
    return true;
}

bool CharSelectSync::CanPlayerCancel(uint8_t player, uint32_t css_frames) {
    // Simplified - always allow cancel
    return true;
}

bool CharSelectSync::HasRecentButtonInHistory(uint8_t player, uint32_t button_mask, uint32_t start_offset, uint32_t end_offset) {
    // Simplified - no button history
    return false;
}

void CharSelectSync::UpdateButtonHistory(uint8_t player_num, uint32_t input) {
    // Simplified - no button history
}

void CharSelectSync::LogCSSInputState(uint32_t css_frames) {
    // Simplified - no logging
}

} // namespace CSS
} // namespace FM2K