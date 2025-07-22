#include "game_state_machine.h"
#include "globals.h"
// #include "css_sync.h"  // Removed CSS filtering
#include <SDL3/SDL_log.h>

// Forward declaration for frame counter
extern uint32_t g_frame_counter;

namespace FM2K {
namespace State {

// Global instance
GameStateMachine g_game_state_machine;

GameStateMachine::GameStateMachine()
    : current_phase_(GamePhase::UNKNOWN)
    , previous_phase_(GamePhase::UNKNOWN)
    , sync_strategy_(SyncStrategy::NONE)
    , char_select_state_{}
    , phase_changed_(false)
    , frames_in_phase_(0)
    , last_game_mode_(0)
    , is_network_session_(false)
    , battle_start_frame_(0)
    , battle_sync_confirmed_(false)
    , battle_sync_frame_(0)
    , char_selection_confirmed_(false)
{
}

void GameStateMachine::Update(uint32_t current_game_mode) {
    // Reset transition flag
    phase_changed_ = false;
    
    // Detect phase based on game mode
    GamePhase new_phase = GamePhase::UNKNOWN;
    
    if (current_game_mode >= 1000 && current_game_mode < 2000) {
        new_phase = GamePhase::TITLE_SCREEN;
    } else if (current_game_mode >= 2000 && current_game_mode < 3000) {
        new_phase = GamePhase::CHARACTER_SELECT;
    } else if (current_game_mode >= 3000 && current_game_mode < 4000) {
        new_phase = GamePhase::IN_BATTLE;
    }
    
    // Check for phase transition
    if (new_phase != current_phase_) {
        previous_phase_ = current_phase_;
        current_phase_ = new_phase;
        phase_changed_ = true;
        frames_in_phase_ = 0;
        
        // Update sync strategy
        sync_strategy_ = DetermineSyncStrategy(current_phase_);
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "Game phase transition: %d -> %d (mode: %d)",
            static_cast<int>(previous_phase_), 
            static_cast<int>(current_phase_),
            current_game_mode);
            
        // Clear character select state when leaving CSS
        if (previous_phase_ == GamePhase::CHARACTER_SELECT) {
            char_select_state_ = {};
            char_selection_confirmed_ = false;
        }
        
        // CRITICAL: Disable rollback during phase transitions to prevent desync
        // Re-enable after stabilization period
        if (new_phase == GamePhase::CHARACTER_SELECT) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                "CHARACTER_SELECT transition - disabling rollback for stabilization");
            
            // CSS sync removed
        } else if (new_phase == GamePhase::IN_BATTLE) {
            battle_start_frame_ = g_frame_counter;
            battle_sync_confirmed_ = false;  // Reset sync confirmation
            battle_sync_frame_ = 0;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                "IN_BATTLE transition at frame %u - starting 600-frame stabilization period (lockstep mode)", 
                battle_start_frame_);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                "Battle rollback will be enabled after stabilization at frame %u", 
                battle_start_frame_ + 600);
            
            // Request synchronization from the network session
            if (is_network_session_) {
                RequestBattleSync();
            } else {
                // Single player - immediately confirm
                battle_sync_confirmed_ = true;
            }
        }
    } else {
        frames_in_phase_++;
    }
    
    last_game_mode_ = current_game_mode;
}

void GameStateMachine::UpdateCharacterSelect(const CharacterSelectState& css_state) {
    // Track changes in confirmation status
    bool prev_p1_confirmed = char_select_state_.p1_confirmed == 1;
    bool prev_p2_confirmed = char_select_state_.p2_confirmed == 1;
    
    char_select_state_ = css_state;
    
    // Log confirmation changes
    if (!prev_p1_confirmed && css_state.p1_confirmed == 1) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "P1 confirmed character selection: %d", css_state.p1_character);
    }
    if (!prev_p2_confirmed && css_state.p2_confirmed == 1) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "P2 confirmed character selection: %d", css_state.p2_character);
    }
    
    // Check if both players are ready to transition
    if (css_state.BothPlayersConfirmed() && !char_selection_confirmed_) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "Both players confirmed - ready for battle transition");
        
        // Auto-confirm character selection if both players confirmed locally
        // This allows the transition to proceed
        if (is_network_session_) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                "CSS: Auto-confirming character selection for network session");
            char_selection_confirmed_ = true;
        }
    }
}

SyncStrategy GameStateMachine::DetermineSyncStrategy(GamePhase phase) const {
    if (!is_network_session_) {
        return SyncStrategy::NONE;
    }
    
    switch (phase) {
        case GamePhase::TITLE_SCREEN:
            // Lockstep for menu navigation
            return SyncStrategy::LOCKSTEP;
            
        case GamePhase::CHARACTER_SELECT:
            // Lockstep for character selection to ensure both see same state
            return SyncStrategy::LOCKSTEP;
            
        case GamePhase::IN_BATTLE:
            // Gradual rollback enablement with stabilization period
            if (battle_start_frame_ > 0) {
                uint32_t frames_in_battle = g_frame_counter - battle_start_frame_;
                const uint32_t STABILIZATION_FRAMES = 600; // 6 seconds at 100 FPS
                
                if (frames_in_battle < STABILIZATION_FRAMES) {
                    // Stay in lockstep during stabilization period
                    return SyncStrategy::LOCKSTEP;
                } else if (frames_in_battle == STABILIZATION_FRAMES) {
                    // Log the transition to rollback (only once)
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                        "Battle stabilization complete at frame %u (%u frames in battle) - enabling rollback netcode", 
                        g_frame_counter, frames_in_battle);
                }
            }
            // Full rollback after stabilization
            return SyncStrategy::ROLLBACK;
            
        default:
            return SyncStrategy::NONE;
    }
}

void GameStateMachine::RequestBattleSync() {
    // Send a sync request through GekkoNet
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        "Requesting battle synchronization at frame %u", g_frame_counter);
    
    // Store the frame when sync was requested
    if (battle_sync_frame_ == 0) {
        battle_sync_frame_ = g_frame_counter;
    }
    
    // Auto-confirm after a reasonable wait (3 seconds = 300 frames at 100 FPS)
    if (g_frame_counter - battle_sync_frame_ > 300) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
            "Battle sync timeout - auto-confirming after %u frames", 
            g_frame_counter - battle_sync_frame_);
        battle_sync_confirmed_ = true;
    }
}

bool GameStateMachine::IsInBattleStabilization() const {
    if (current_phase_ != GamePhase::IN_BATTLE || battle_start_frame_ == 0) {
        return false;
    }
    
    uint32_t frames_in_battle = g_frame_counter - battle_start_frame_;
    const uint32_t STABILIZATION_FRAMES = 600; // 6 seconds at 100 FPS
    
    return frames_in_battle < STABILIZATION_FRAMES;
}

uint32_t GameStateMachine::GetFramesInBattle() const {
    if (current_phase_ != GamePhase::IN_BATTLE || battle_start_frame_ == 0) {
        return 0;
    }
    
    return g_frame_counter - battle_start_frame_;
}

} // namespace State
} // namespace FM2K