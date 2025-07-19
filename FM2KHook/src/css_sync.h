#pragma once

#include "common.h"
#include "game_state_machine.h"
#include <cstdint>

namespace FM2K {
namespace CSS {

// Character Select Screen synchronization manager
class CharSelectSync {
public:
    CharSelectSync();
    
    // Called every frame during character select
    void Update();
    
    // Network sync methods
    void SendLocalState();
    void ReceiveRemoteState();
    void ApplyRemoteState();
    
    // Confirmation handshake method
    void ReceiveRemoteConfirmation() { confirmation_received_ = true; }
    
    // Query sync status
    bool IsInSync() const { return in_sync_; }
    uint32_t GetDesyncFrames() const { return desync_frames_; }
    
    // Force resync if needed (e.g., after network hiccup)
    void ForceResync();
    
private:
    // Read current CSS state from memory
    State::CharacterSelectState ReadCurrentState();
    
    // Apply lockstep synchronization
    void ApplyLockstepSync();
    
    // Handle character confirmation synchronization
    void HandleCharacterConfirmation();
    
    // State tracking
    State::CharacterSelectState local_state_;
    State::CharacterSelectState remote_state_;
    State::CharacterSelectState last_sent_state_;
    
    // Sync status
    bool in_sync_;
    uint32_t desync_frames_;
    uint32_t last_sync_frame_;
    
    // Confirmation tracking
    bool confirmation_sent_;
    bool confirmation_received_;
    
    // Network message types for CSS
    struct CSSStateMessage {
        uint32_t frame_number;
        uint32_t p1_cursor_x;
        uint32_t p1_cursor_y;
        uint32_t p2_cursor_x;
        uint32_t p2_cursor_y;
        uint32_t p1_selected_char;
        uint32_t p2_selected_char;
        uint32_t p1_confirmed;
        uint32_t p2_confirmed;
    };
    
    // Helper to pack/unpack network messages
    void PackStateMessage(CSSStateMessage& msg, const State::CharacterSelectState& state);
    void UnpackStateMessage(const CSSStateMessage& msg, State::CharacterSelectState& state);
};

// Global instance
extern CharSelectSync g_css_sync;

} // namespace CSS
} // namespace FM2K