#pragma once

#include "common.h"
#include <cstdint>

namespace FM2K {
namespace State {

// FM2K Game States based on game_mode values at 0x470054
enum class GamePhase : uint32_t {
    UNKNOWN = 0,
    TITLE_SCREEN = 1000,       // Title menu (mode 1000)
    CHARACTER_SELECT = 2000,   // Character selection screen (mode 2000)
    IN_BATTLE = 3000,          // Active combat (mode 3000)
};

// Synchronization strategy for each phase
enum class SyncStrategy {
    NONE,           // No sync needed (single player)
    LOCKSTEP,       // Frame-perfect sync (menus, char select)
    ROLLBACK,       // Full rollback netcode (battle)
};

// Character selection state with comprehensive tracking (CCCaster-style)
struct CharacterSelectState {
    // === Core Selection Data ===
    // Character/stage selections (similar to CC_P1/P2_CHARACTER_ADDR)
    uint32_t p1_character;      // Currently selected character ID
    uint32_t p2_character;      // Currently selected character ID  
    uint32_t selected_stage;    // Stage selection (similar to CC_STAGE_SELECTOR_ADDR)
    
    // Cursor/selector positions (similar to CC_P1/P2_CHARA_SELECTOR_ADDR)
    uint32_t p1_cursor_x;       // Character grid cursor X
    uint32_t p1_cursor_y;       // Character grid cursor Y
    uint32_t p2_cursor_x;       // Character grid cursor X
    uint32_t p2_cursor_y;       // Character grid cursor Y
    
    // Character variations (similar to CC_P1/P2_MOON_SELECTOR_ADDR, CC_P1/P2_COLOR_SELECTOR_ADDR)
    uint32_t p1_variant;        // Character variant/style (moon equivalent)
    uint32_t p2_variant;        // Character variant/style
    uint32_t p1_color;          // Color palette selection
    uint32_t p2_color;          // Color palette selection
    
    // === Selection State Tracking (CCCaster-style) ===
    // Selection modes (similar to CC_P1/P2_SELECTOR_MODE_ADDR)
    enum class SelectionMode : uint32_t {
        SELECTING_CHARACTER = 0,    // Still choosing character
        CHARACTER_CONFIRMED = 1,    // Character locked in
        FULLY_READY = 2            // Ready to start battle
    };
    
    SelectionMode p1_selection_mode;
    SelectionMode p2_selection_mode;
    
    // Confirmation status from FM2K memory
    uint32_t p1_confirmed;  // From 0x47019C
    uint32_t p2_confirmed;  // From 0x4701A0
    
    // === Timing and Validation (CCCaster-style) ===
    uint32_t frames_in_css;             // Frames since entering character select
    uint32_t p1_last_input_frame;       // Last frame P1 changed selection
    uint32_t p2_last_input_frame;       // Last frame P2 changed selection
    uint32_t p1_last_mode_change_frame; // Last frame P1 changed selection mode
    uint32_t p2_last_mode_change_frame; // Last frame P2 changed selection mode
    
    // Input validation flags
    bool p1_can_confirm;        // P1 allowed to confirm (timing-gated)
    bool p2_can_confirm;        // P2 allowed to confirm
    bool p1_can_cancel;         // P1 allowed to go back
    bool p2_can_cancel;         // P2 allowed to go back
    
    // === Network Sync Status ===
    uint32_t checksum;          // State checksum for validation
    uint32_t sync_frame;        // Frame this state was synced
    
    // === Helper Methods ===
    bool BothPlayersConfirmed() const {
        return p1_confirmed == 1 && p2_confirmed == 1;
    }
    
    bool AnyPlayerConfirmed() const {
        return p1_confirmed == 1 || p2_confirmed == 1;
    }
    
    bool BothPlayersReady() const {
        return p1_selection_mode == SelectionMode::FULLY_READY && 
               p2_selection_mode == SelectionMode::FULLY_READY;
    }
    
    bool PlayerCanConfirm(uint8_t player) const {
        return (player == 1) ? p1_can_confirm : p2_can_confirm;
    }
    
    bool PlayerCanCancel(uint8_t player) const {
        return (player == 1) ? p1_can_cancel : p2_can_cancel;
    }
    
    // Calculate state checksum for sync validation
    uint32_t CalculateChecksum() const {
        uint32_t hash = 0;
        hash ^= p1_character * 17;
        hash ^= p2_character * 23;
        hash ^= selected_stage * 29;
        hash ^= static_cast<uint32_t>(p1_selection_mode) * 31;
        hash ^= static_cast<uint32_t>(p2_selection_mode) * 37;
        return hash;
    }
};

// State machine for tracking game flow
class GameStateMachine {
public:
    GameStateMachine();
    
    // Update state based on current game mode
    void Update(uint32_t current_game_mode);
    
    // Query current state
    GamePhase GetCurrentPhase() const { return current_phase_; }
    GamePhase GetPreviousPhase() const { return previous_phase_; }
    SyncStrategy GetSyncStrategy() const { return sync_strategy_; }
    
    // Character select specific
    void UpdateCharacterSelect(const CharacterSelectState& css_state);
    const CharacterSelectState& GetCharSelectState() const { return char_select_state_; }
    bool IsCharacterSelectionComplete() const {
        return current_phase_ == GamePhase::CHARACTER_SELECT && 
               char_select_state_.BothPlayersConfirmed();
    }
    
    // Transition detection
    bool HasTransitioned() const { return phase_changed_; }
    bool IsTransitioningToBattle() const {
        // The transition is only valid if the handshake is complete.
        return phase_changed_ &&
               current_phase_ == GamePhase::IN_BATTLE &&
               previous_phase_ == GamePhase::CHARACTER_SELECT &&
               char_selection_confirmed_;
    }
    
    // Frame tracking for transitions
    uint32_t GetFramesInCurrentPhase() const { return frames_in_phase_; }
    
    // Network sync control
    bool ShouldEnableRollback() const {
        // Only enable rollback during battle, and only after stabilization
        return current_phase_ == GamePhase::IN_BATTLE && frames_in_phase_ > 60;
    }
    
    bool ShouldUseLockstep() const {
        return current_phase_ == GamePhase::CHARACTER_SELECT ||
               (current_phase_ == GamePhase::TITLE_SCREEN && IsNetworkSession());
    }
    
    // Check if we're in a transition stabilization period
    bool IsInTransitionStabilization() const {
        // After a phase change, wait for a brief period for the game state to become stable.
        if (frames_in_phase_ < 60) { // 60 frames = ~1 second stabilization window
            return true;
        }
    
        // Special case for battle: we are not stable until both clients have confirmed battle entry.
        if (current_phase_ == GamePhase::IN_BATTLE) {
            return !battle_sync_confirmed_;
        }

        // Otherwise, we are considered stable.
        return false;
    }
    
    // Get the frame when battle started (for sync purposes)
    uint32_t GetBattleStartFrame() const { return battle_start_frame_; }
    
    // Battle transition tracking
    bool IsInBattleStabilization() const;
    uint32_t GetFramesInBattle() const;
    
    // Battle synchronization control
    void ConfirmBattleSync() { battle_sync_confirmed_ = true; }
    bool IsBattleSyncConfirmed() const { return battle_sync_confirmed_; }
    void RequestBattleSync();
    
    // Character selection confirmation
    void ConfirmCharacterSelection() { 
        char_selection_confirmed_ = true; 
    }
    bool IsCharacterSelectionConfirmed() const { return char_selection_confirmed_; }
    
private:
    GamePhase current_phase_;
    GamePhase previous_phase_;
    SyncStrategy sync_strategy_;
    
    CharacterSelectState char_select_state_;
    
    bool phase_changed_;
    uint32_t frames_in_phase_;
    uint32_t last_game_mode_;
    bool is_network_session_;
    uint32_t battle_start_frame_;
    bool battle_sync_confirmed_;
    uint32_t battle_sync_frame_;
    bool char_selection_confirmed_;
    
    // Helper to determine sync strategy for a phase
    SyncStrategy DetermineSyncStrategy(GamePhase phase) const;
    
    // Check if we're in a network session (vs local play)
    bool IsNetworkSession() const { return is_network_session_; }
    
public:
    void SetNetworkSession(bool is_network) { is_network_session_ = is_network; }
};


// Global state machine instance
extern GameStateMachine g_game_state_machine;

} // namespace State
} // namespace FM2K