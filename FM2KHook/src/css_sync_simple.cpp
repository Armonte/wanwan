#include "css_sync.h"
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
    , css_frame_count_(0)
    , last_input_frame_(0)
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
        last_input_frame_ = 0;
        css_reset = true;
    }
    
    // Increment CSS frame counter for input filtering
    css_frame_count_++;
    
    // Read current state for confirmation tracking
    local_state_ = ReadCurrentState();
    
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

void CharSelectSync::HandleCharacterConfirmation() {
    if (!gekko_session_started) return;

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

    // Check if handshake is complete
    if (confirmation_sent_ && confirmation_received_) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Handshake complete! Both players confirmed.");
        // Transition to battle will be handled by game state machine
    }
}

// All the complex CCCaster-style methods are now simplified or removed
void CharSelectSync::UpdateCSSTimingAndValidation(uint32_t css_frames) {
    // Simplified - no complex timing validation
}

uint32_t CharSelectSync::ValidateAndFilterCSSInput(uint32_t raw_input, uint8_t player_num, uint32_t css_frames) {
    // CCCaster-style input filtering to prevent desyncs
    uint32_t filtered_input = raw_input;
    
    // CRITICAL: Prevent confirmation inputs for first 150 frames (CCCaster's "moon selector desync" fix)
    if (css_frames < 150) {
        // Remove confirm/start buttons during lockout period
        filtered_input &= ~(0x10 | 0x20);  // Remove START and BUTTON1 (common confirm buttons)
    }
    
    // CRITICAL: Prevent rapid confirm/cancel inputs (2-3 frame lockout after any input)
    uint32_t frames_since_last_input = css_frames - last_input_frame_;
    if (frames_since_last_input < 3) {
        // Remove confirm/cancel buttons if we had recent input
        filtered_input &= ~(0x10 | 0x20 | 0x40);  // Remove START, BUTTON1, BUTTON2
    }
    
    // Update last input frame if we have any input
    if (raw_input != 0) {
        last_input_frame_ = css_frames;
    }
    
    return filtered_input;
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