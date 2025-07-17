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

// Character selection state with confirmation tracking
struct CharacterSelectState {
    // Cursor positions
    uint32_t p1_cursor_x;
    uint32_t p1_cursor_y;
    uint32_t p2_cursor_x;
    uint32_t p2_cursor_y;
    
    // Selected characters (0 = not selected)
    uint32_t p1_selected_char;
    uint32_t p2_selected_char;
    
    // Confirmation status from FM2K memory
    uint32_t p1_confirmed;  // From 0x47019C
    uint32_t p2_confirmed;  // From 0x4701A0
    
    bool BothPlayersConfirmed() const {
        return p1_confirmed == 1 && p2_confirmed == 1;
    }
    
    bool AnyPlayerConfirmed() const {
        return p1_confirmed == 1 || p2_confirmed == 1;
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
        return current_phase_ == GamePhase::IN_BATTLE && 
               previous_phase_ == GamePhase::CHARACTER_SELECT;
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
        // CRITICAL: Disable ALL rollback until we're actually in battle
        // This prevents desyncs during menu navigation
        if (current_phase_ != GamePhase::IN_BATTLE) {
            return true;  // Always stabilize during menus
        }
        
        // For battle: Wait for sync confirmation from both clients
        // Only enable rollback after both clients confirm battle entry
        return !battle_sync_confirmed_;
    }
    
    // Get the frame when battle started (for sync purposes)
    uint32_t GetBattleStartFrame() const { return battle_start_frame_; }
    
    // Battle synchronization control
    void ConfirmBattleSync() { battle_sync_confirmed_ = true; }
    bool IsBattleSyncConfirmed() const { return battle_sync_confirmed_; }
    void RequestBattleSync();
    
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