# Implementation Plan: `cccaster`-style Netplay for FM2KHook

## 1. High-Level Goal

Our goal is to implement a hybrid netplay model similar to `cccaster`'s for maximum stability and performance. This involves:

1.  **Lockstep Synchronization**: For all menus and the Character Select Screen (CSS). This approach is simple and prevents desyncs during stable, non-critical game states.
2.  **Rollback Synchronization**: Enabled *only* during active battle gameplay. This focuses the performance-intensive work of state saving and loading where it's most needed.
3.  **Explicit Confirmation Handshake**: To manage the transition from Character Select to Battle, preventing race conditions where one player moves on before the other is ready.

## 2. Core Synchronization Principles

### Frame-Perfect Input (The Foundation of Lockstep)

> **User Question:** "this all needs to hinge on both clients receive inputs from remote player for same frame right?"

Yes, absolutely. This is the most critical principle. The entire system relies on GekkoNet's ability to provide synchronized inputs. Here is the flow:

1.  Each frame, our hook calls `gekko_add_local_input()` to submit the local player's controller state for that specific frame.
2.  Our main loop then calls `gekko_update()`.
3.  GekkoNet will **not** fire an `AdvanceEvent` until it has successfully received the input from the remote player for that *exact same frame*.
4.  The `AdvanceEvent` contains the synchronized inputs for *both* players.
5.  Our hook then injects these inputs into the game.

This process guarantees that both game clients execute the same frame with the same inputs, achieving frame-perfect lockstep synchronization.

### The `0xFF` Confirmation Signal

> **User Question:** "how will we generate this and are we already doing so?"

We are **not** generating it yet. This is a new piece of logic we will add.

*   **How it's Generated**: The `0xFF` value will be used as a special "meta" input. When our code in `css_sync.cpp` reads from game memory and detects that the local player has confirmed their character (e.g., `p1_confirmed == 1`), it will call `gekko_add_local_input()` with `0xFF` for that frame **instead of the actual controller input**.
*   **Why it Works**: Since `0xFF` is not a valid controller input value, we can use it as a unique signal to the other player that a confirmation event has occurred, without interfering with normal gameplay inputs. The remote player will see this `0xFF` in the `AdvanceEvent` and know their opponent has confirmed.

## 3. Implementation Steps

Here is the concrete plan for modifying the codebase.

### Step 1: Implement State-Aware Saves & Loads in `hooks.cpp`

The `GameStateMachine` will act as the brain, telling our GekkoNet event handlers whether to perform a full rollback save or a minimal lockstep "dummy" save.

**File to Edit:** `wanwan/FM2KHook/src/hooks.cpp`

**In `Hook_ProcessGameInputs`:**

```cpp
// Inside the gekko_update loop...
case SaveEvent: {
    // Query the state machine to determine the current strategy
    auto strategy = FM2K::State::g_game_state_machine.GetSyncStrategy();

    if (strategy == FM2K::State::SyncStrategy::ROLLBACK) {
        // We are in active, stable battle. Perform a full state save.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SaveEvent: Full Rollback Save at frame %u", update->data.save.frame);
        // --> PLACE YOUR EXISTING FULL STATE-SAVING LOGIC HERE <--
        // Example: FM2K::State::SaveStateFast(...);
    } else {
        // We are in lockstep (menus, CSS, or transition). Perform a minimal "dummy" save.
        // GekkoNet requires a state buffer, but its contents don't matter for lockstep.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SaveEvent: Lockstep (Minimal) Save at frame %u", update->data.save.frame);
        if (update->data.save.state_len) *update->data.save.state_len = 8; // A small, non-zero size.
        if (update->data.save.checksum) *update->data.save.checksum = 0xDEADBEEF + update->data.save.frame;
        if (update->data.save.state) {
            // Fill with a placeholder value for clarity in debugging.
            memset(update->data.save.state, 0xAA, 8);
        }
    }
    break;
}

case LoadEvent: {
    auto strategy = FM2K::State::g_game_state_machine.GetSyncStrategy();

    if (strategy == FM2K::State::SyncStrategy::ROLLBACK) {
        // Only load state if we are in a rollback-enabled phase.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LoadEvent: Full Rollback Load to frame %u", update->data.load.frame);
        // --> PLACE YOUR EXISTING STATE-LOADING LOGIC HERE <--
        // Example: FM2K::State::RestoreStateFast(...);
    } else {
        // In lockstep mode, we NEVER load state. The game progresses naturally.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LoadEvent: Ignored during Lockstep frame %u", update->data.load.frame);
    }
    break;
}
```

### Step 2: Implement the Confirmation Handshake

This involves `css_sync` sending the signal and `hooks` receiving it.

**File to Edit:** `wanwan/FM2KHook/src/css_sync.cpp` & `css_sync.h`

```cpp
// In css_sync.h -> Add a new public method to the CharSelectSync class
public:
    void ReceiveRemoteConfirmation() { confirmation_received_ = true; }

// In css_sync.cpp -> Update HandleCharacterConfirmation
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
```

**File to Edit:** `wanwan/FM2KHook/src/hooks.cpp`

**In `Hook_ProcessGameInputs`:**

```cpp
// Inside the gekko_update loop...
case AdvanceEvent: {
    // Always apply the synchronized inputs first.
    networked_p1_input = update->data.adv.inputs[0];
    networked_p2_input = update->data.adv.inputs[1];
    use_networked_inputs = true;

    // Check if the remote player sent a confirmation signal.
    uint8_t remote_input = is_host ? networked_p2_input : networked_p1_input;
    if (remote_input == 0xFF) {
        FM2K::CSS::g_css_sync.ReceiveRemoteConfirmation();
    }

    // Now, let the original game code run with the synchronized inputs.
    if (original_process_inputs) {
        original_process_inputs();
    }
    g_frame_counter++;
    break;
}
```

### Step 3: Update the Game State Machine

Finally, we update the state machine to respect the confirmation handshake.

**File to Edit:** `wanwan/FM2KHook/src/game_state_machine.h`

```cpp
// In the GameStateMachine class definition...

// Add this line to the public section:
void ConfirmCharacterSelection() { char_selection_confirmed_ = true; }

// Add this line to the private member variables:
bool char_selection_confirmed_ = false;

// Find the IsTransitioningToBattle() method and modify it:
bool IsTransitioningToBattle() const {
    // The transition is only valid if the handshake is complete.
    return phase_changed_ &&
           current_phase_ == GamePhase::IN_BATTLE &&
           previous_phase_ == GamePhase::CHARACTER_SELECT &&
           char_selection_confirmed_;
}

// You should also reset the flag when leaving the battle phase.
// A good place is in the Update() method when transitioning AWAY from IN_BATTLE.
``` 