#include "css_sync.h"
#include "globals.h"
#include "gekkonet.h"
#include <SDL3/SDL_log.h>
#include <cstring>

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
    // Only operate during character select phase
    if (State::g_game_state_machine.GetCurrentPhase() != State::GamePhase::CHARACTER_SELECT) {
        return;
    }
    
    // Read current state from memory
    local_state_ = ReadCurrentState();
    
    // Update the state machine with current CSS state
    State::g_game_state_machine.UpdateCharacterSelect(local_state_);
    
    // Only sync if we're in a network session
    if (!gekko_initialized || !gekko_session_started) {
        return;
    }
    
    // SIMPLIFIED: Use lockstep synchronization for character select
    ApplyLockstepSync();
    
    // Handle character confirmation synchronization
    HandleCharacterConfirmation();
}

State::CharacterSelectState CharSelectSync::ReadCurrentState() {
    State::CharacterSelectState state;
    
    // Read cursor positions
    state.p1_cursor_x = *(uint32_t*)FM2K::State::Memory::P1_CSS_CURSOR_X_ADDR;
    state.p1_cursor_y = *(uint32_t*)FM2K::State::Memory::P1_CSS_CURSOR_Y_ADDR;
    state.p2_cursor_x = *(uint32_t*)FM2K::State::Memory::P2_CSS_CURSOR_X_ADDR;
    state.p2_cursor_y = *(uint32_t*)FM2K::State::Memory::P2_CSS_CURSOR_Y_ADDR;
    
    // Read selected characters
    state.p1_selected_char = *(uint32_t*)FM2K::State::Memory::P1_SELECTED_CHAR_ADDR;
    state.p2_selected_char = *(uint32_t*)FM2K::State::Memory::P2_SELECTED_CHAR_ADDR;
    
    // Read confirmation status
    state.p1_confirmed = *(uint32_t*)FM2K::State::Memory::P1_CSS_CONFIRMED_ADDR;
    state.p2_confirmed = *(uint32_t*)FM2K::State::Memory::P2_CSS_CONFIRMED_ADDR;
    
    return state;
}

void CharSelectSync::ApplyLockstepSync() {
    // SIMPLIFIED LOCKSTEP: Just ensure both clients see the same state
    // Let GekkoNet handle the frame synchronization
    
    // Log significant state changes
    static State::CharacterSelectState last_logged_state = {};
    bool state_changed = (memcmp(&local_state_, &last_logged_state, sizeof(State::CharacterSelectState)) != 0);
    
    if (state_changed) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "CSS Lockstep: P1_cursor=(%d,%d) P2_cursor=(%d,%d) chars=(%d,%d) confirmed=(%d,%d)",
            local_state_.p1_cursor_x, local_state_.p1_cursor_y,
            local_state_.p2_cursor_x, local_state_.p2_cursor_y,
            local_state_.p1_selected_char, local_state_.p2_selected_char,
            local_state_.p1_confirmed, local_state_.p2_confirmed);
            
        last_logged_state = local_state_;
        in_sync_ = true;
        desync_frames_ = 0;
    }
}

void CharSelectSync::HandleCharacterConfirmation() {
    if (!gekko_session_started) return;

    // 1. Check if the local player has confirmed their selection
    bool local_player_confirmed = false;
    if (is_host) {
        local_player_confirmed = (local_state_.p1_confirmed == 1);
    } else {
        local_player_confirmed = (local_state_.p2_confirmed == 1);
    }

    // 2. If confirmed and we haven't sent our signal yet, send it.
    if (local_player_confirmed && !confirmation_sent_) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Local player confirmed. Sending 0xFF signal.");
        confirmation_sent_ = true;
        
        // This is the special input signal.
        uint8_t confirmation_input = 0xFF;
        gekko_add_local_input(gekko_session, local_player_handle, &confirmation_input);
    }

    // 3. Check if the handshake is complete.
    if (confirmation_sent_ && confirmation_received_ && !State::g_game_state_machine.IsCharacterSelectionConfirmed()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Handshake complete! Both players confirmed.");
        
        // Tell the state machine it's safe to transition.
        State::g_game_state_machine.ConfirmCharacterSelection();
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
        *(uint32_t*)FM2K::State::Memory::P2_CSS_CURSOR_X_ADDR = remote_state_.p2_cursor_x;
        *(uint32_t*)FM2K::State::Memory::P2_CSS_CURSOR_Y_ADDR = remote_state_.p2_cursor_y;
        *(uint32_t*)FM2K::State::Memory::P2_SELECTED_CHAR_ADDR = remote_state_.p2_selected_char;
        *(uint32_t*)FM2K::State::Memory::P2_CSS_CONFIRMED_ADDR = remote_state_.p2_confirmed;
    } else {
        // Client controls P2, apply remote P1 state
        *(uint32_t*)FM2K::State::Memory::P1_CSS_CURSOR_X_ADDR = remote_state_.p1_cursor_x;
        *(uint32_t*)FM2K::State::Memory::P1_CSS_CURSOR_Y_ADDR = remote_state_.p1_cursor_y;
        *(uint32_t*)FM2K::State::Memory::P1_SELECTED_CHAR_ADDR = remote_state_.p1_selected_char;
        *(uint32_t*)FM2K::State::Memory::P1_CSS_CONFIRMED_ADDR = remote_state_.p1_confirmed;
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
    msg.p1_selected_char = state.p1_selected_char;
    msg.p2_selected_char = state.p2_selected_char;
    msg.p1_confirmed = state.p1_confirmed;
    msg.p2_confirmed = state.p2_confirmed;
}

void CharSelectSync::UnpackStateMessage(const CSSStateMessage& msg, State::CharacterSelectState& state) {
    state.p1_cursor_x = msg.p1_cursor_x;
    state.p1_cursor_y = msg.p1_cursor_y;
    state.p2_cursor_x = msg.p2_cursor_x;
    state.p2_cursor_y = msg.p2_cursor_y;
    state.p1_selected_char = msg.p1_selected_char;
    state.p2_selected_char = msg.p2_selected_char;
    state.p1_confirmed = msg.p1_confirmed;
    state.p2_confirmed = msg.p2_confirmed;
}

} // namespace CSS
} // namespace FM2K