#include "hooks.h"
#include "globals.h"
#include "gekkonet_hooks.h"
#include "state_manager.h"
#include "logging.h"
#include "shared_mem.h"
#include "game_state_machine.h"
#include "css_sync.h"
#include "object_tracker.h"
#include "object_analysis.h"
#include "object_pool_scanner.h"
#include "boot_object_analyzer.cpp"
#include <windows.h>
#include <mmsystem.h>

// ARCHITECTURE FIX: Real input capture following CCCaster/GekkoNet pattern
static void CaptureRealInputs() {
    // Following the pattern from GekkoNet SDL2 example and CCCaster
    // This captures actual keyboard/controller input before the game processes it
    
    // For now, we'll read the inputs directly from the game's memory addresses
    // In the future, this could be enhanced to use direct keyboard/controller APIs
    
    // BACK TO WORKING APPROACH: Use Hook_GetPlayerInput to capture inputs at source
    // Just like dllmain_orig.cpp - inputs are captured when FM2K calls Hook_GetPlayerInput
    // Don't override them here - let the input hooks do their job
    
    // DEBUG: Log non-zero input captures (very reduced frequency)
    static uint32_t input_capture_count = 0;
    if ((live_p1_input != 0 || live_p2_input != 0) && (++input_capture_count % 600 == 0)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT_CAPTURE: P1=0x%02X P2=0x%02X frame=%u", 
                   live_p1_input & 0xFF, live_p2_input & 0xFF, g_frame_counter);
    }
}

// Use global function pointers from globals.h

// CRITICAL FIX: Convert 8-bit network inputs to 11-bit game format
static uint32_t ConvertNetworkInputToGameFormat(uint32_t network_input) {
    uint32_t game_input = 0;
    
    // Convert 8-bit network format to 11-bit game format
    // Network format: 0x01=LEFT, 0x02=RIGHT, 0x04=UP, 0x08=DOWN, 0x10=START, 0x20=BUTTON1, 0x40=BUTTON2, 0x80=BUTTON3
    // Game format: 0x001=LEFT, 0x002=RIGHT, 0x004=UP, 0x008=DOWN, 0x010=BUTTON1, 0x020=BUTTON2, 0x040=BUTTON3, 0x080=BUTTON4, etc.
    
    if (network_input & 0x01) game_input |= 0x001;  // LEFT
    if (network_input & 0x02) game_input |= 0x002;  // RIGHT
    if (network_input & 0x04) game_input |= 0x004;  // UP
    if (network_input & 0x08) game_input |= 0x008;  // DOWN
    if (network_input & 0x10) game_input |= 0x010;  // BUTTON1 (START)
    if (network_input & 0x20) game_input |= 0x020;  // BUTTON2
    if (network_input & 0x40) game_input |= 0x040;  // BUTTON3
    if (network_input & 0x80) game_input |= 0x080;  // BUTTON4
    
    // DEBUG: Log conversion for non-zero inputs
    static uint32_t conversion_count = 0;
    conversion_count++;
    if (network_input != 0 || conversion_count % 200 == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT_CONVERSION: network=0x%02X → game=0x%03X (count=%u)", 
                   network_input & 0xFF, game_input & 0x7FF, conversion_count);
    }
    
    return game_input;
}

// New hook for boot-to-character-select hack
// This hook modifies the game's initialization to boot directly to character select screen
// instead of showing the title screen and splash screens. It does this by:
// 1. Setting the character select mode flag to 1 (vs player mode instead of vs CPU)
// 2. Changing the initialization object byte from 0x11 to 0x0A to skip to character select
void ApplyBootToCharacterSelectPatches() {
    // Change initialization object from 0x11 to 0x0A to boot to character select
    uint8_t* init_object_ptr = (uint8_t*)0x409CD9;
    if (!IsBadReadPtr(init_object_ptr, sizeof(uint16_t))) {
        // Make the memory writable
        DWORD old_protect;
        if (VirtualProtect(init_object_ptr, 2, PAGE_EXECUTE_READWRITE, &old_protect)) {
            // Write the instruction: 6A 0A (push 0x0A)
            init_object_ptr[0] = 0x6A;  // push instruction
            init_object_ptr[1] = 0x0A;  // immediate value
            
            // Restore original protection
            VirtualProtect(init_object_ptr, 2, old_protect, &old_protect);
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Wrote instruction 6A 0A at 0x409CD9");
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to make memory writable at 0x409CD9");
        }
    }
}

// Hook implementations
int __cdecl Hook_GetPlayerInput(int player_id, int input_type) {
    // CRITICAL FIX: Both clients read P1 controls locally, but map to their network slot
    // This matches fighting game conventions - everyone uses same local controls (WASD/arrow keys)
    
    int original_input = 0;
    
    if (player_id == 0) {
        // P1 slot
        if (::is_host) {
            // Host: Read keyboard for P1, store in live_p1_input
            original_input = original_get_player_input ? original_get_player_input(0, input_type) : 0;
            live_p1_input = original_input;
        } else {
            // Client: P1 slot gets 0 (remote player), keyboard goes to live_p2_input
            original_input = original_get_player_input ? original_get_player_input(0, input_type) : 0;
            live_p2_input = original_input;  // Store for network transmission
            original_input = 0;  // FM2K P1 slot gets 0 (remote player)
        }
    } else if (player_id == 1) {
        // P2 slot
        if (::is_host) {
            // Host: P2 slot gets 0 (remote player input comes from network)
            original_input = 0;
        } else {
            // Client: P2 slot gets local keyboard input (already captured above)
            original_input = live_p2_input;  // Use already captured input
        }
    }
    
    // DEBUG: Verify hook is being called and check raw inputs (reduced frequency)
    static uint32_t hook_call_count = 0;
    hook_call_count++;
    if (hook_call_count % 500 == 0 || (original_input != 0 && hook_call_count % 50 == 0)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK_GETINPUT: P%d type=%d orig=0x%02X calls=%u", 
                   player_id, input_type, original_input & 0xFF, hook_call_count);
    }
    
    // During CSS: Return synchronized inputs for proper cursor movement
    if (FM2K::State::g_game_state_machine.GetCurrentPhase() == FM2K::State::GamePhase::CHARACTER_SELECT) {
        if (use_networked_inputs && gekko_initialized && gekko_session) {
            // Use synchronized inputs during CSS for both players
            if (player_id == 0) {
                // CRITICAL FIX: Convert 8-bit network inputs to 11-bit game format
                uint32_t converted_input = ConvertNetworkInputToGameFormat(networked_p1_input);
                
                // CRITICAL FIX: Manual cursor position management for P1
                static uint32_t last_p1_input = 0xFF;
                if (networked_p1_input != last_p1_input) {
                    // Force cursor position update when input changes
                    if (!IsBadWritePtr((void*)0x424E50, sizeof(uint32_t) * 2)) {
                        uint32_t* p1_cursor = (uint32_t*)0x424E50;
                        
                        // Get current cursor position
                        uint32_t current_x = p1_cursor[0];
                        uint32_t current_y = p1_cursor[1];
                        
                        // Calculate new position based on input
                        if (networked_p1_input & 0x02) { // RIGHT
                            p1_cursor[0] = current_x + 1;
                        } else if (networked_p1_input & 0x01) { // LEFT
                            p1_cursor[0] = (current_x > 0) ? current_x - 1 : 0;
                        }
                        
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS_CURSOR_FORCE: P1 input=0x%02X, pos=(%d,%d)→(%d,%d)", 
                                   networked_p1_input & 0xFF, current_x, current_y, p1_cursor[0], p1_cursor[1]);
                    }
                    last_p1_input = networked_p1_input;
                }
                
                // DEBUG: Log P1 input routing during CSS (reduced frequency)
                static uint32_t p1_route_count = 0;
                p1_route_count++;
                if (p1_route_count % 100 == 0 || networked_p1_input != 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS_INPUT_ROUTE: P1 returning networked=0x%02X → converted=0x%03X (player_id=%d)", 
                               networked_p1_input & 0xFF, converted_input & 0x7FF, player_id);
                }
                return converted_input;
            } else if (player_id == 1) {
                // CRITICAL FIX: Convert 8-bit network inputs to 11-bit game format
                uint32_t converted_input = ConvertNetworkInputToGameFormat(networked_p2_input);
                
                // CRITICAL FIX: Manual cursor position management for P2
                static uint32_t last_p2_input = 0xFF;
                if (networked_p2_input != last_p2_input) {
                    // Force cursor position update when input changes
                    if (!IsBadWritePtr((void*)0x424E58, sizeof(uint32_t) * 2)) {
                        uint32_t* p2_cursor = (uint32_t*)0x424E58;
                        
                        // Get current cursor position
                        uint32_t current_x = p2_cursor[0];
                        uint32_t current_y = p2_cursor[1];
                        
                        // Calculate new position based on input
                        if (networked_p2_input & 0x02) { // RIGHT
                            p2_cursor[0] = current_x + 1;
                        } else if (networked_p2_input & 0x01) { // LEFT
                            p2_cursor[0] = (current_x > 0) ? current_x - 1 : 0;
                        }
                        
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS_CURSOR_FORCE: P2 input=0x%02X, pos=(%d,%d)→(%d,%d)", 
                                   networked_p2_input & 0xFF, current_x, current_y, p2_cursor[0], p2_cursor[1]);
                    }
                    last_p2_input = networked_p2_input;
                }
                
                // DEBUG: Log P2 input routing during CSS (reduced frequency)
                static uint32_t p2_route_count = 0;
                p2_route_count++;
                if (p2_route_count % 100 == 0 || networked_p2_input != 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS_INPUT_ROUTE: P2 returning networked=0x%02X → converted=0x%03X (player_id=%d)", 
                               networked_p2_input & 0xFF, converted_input & 0x7FF, player_id);
                }
                return converted_input;
            }
        }
        // Fallback to original input if no network sync
        return original_input;
    }
    
    // During battle: Use synchronized networked inputs if available
    if (use_networked_inputs && gekko_initialized && gekko_session) {
        if (player_id == 0) {
            return ConvertNetworkInputToGameFormat(networked_p1_input);
        } else if (player_id == 1) {
            return ConvertNetworkInputToGameFormat(networked_p2_input);
        }
    }
    
    // Fall back to original input (like dllmain_orig.cpp)
    return original_input;
}

int __cdecl Hook_ProcessGameInputs() {
    // In lockstep/rollback mode, the game's frame advancement is handled inside the AdvanceEvent.
    // We do nothing here to allow GekkoNet to control the frame pacing.
    if (!waiting_for_gekko_advance) {
        // If not waiting for GekkoNet, we're in a non-networked or pre-session state.
        // Call the original function to let the game run normally.
        if (original_process_inputs) {
            original_process_inputs();
        }
        g_frame_counter++;
    }
    
    // Early logging to verify hook works (only first few frames)
    if (g_frame_counter <= 3) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Frame %u", g_frame_counter);
    }
    
    // GekkoNet rollback control (only if session is active)
    if (gekko_initialized && gekko_session && gekko_session_started) {
        // ARCHITECTURE FIX: Proper input capture following CCCaster/GekkoNet pattern
        // 1. CAPTURE: Read actual controller/keyboard inputs (like CCCaster's updateControls)
        CaptureRealInputs();
        
        // Update CSS synchronization
        FM2K::CSS::g_css_sync.Update();
        
        // 2. SEND: Send local input to GekkoNet (like GekkoNet example's gekko_add_local_input)
        // GEKKONET-STYLE: Only send OUR keyboard input, regardless of player slot
        uint8_t local_input;
        const char* input_source;
        if (::is_host) {
            local_input = (uint8_t)(live_p1_input & 0xFF); // Host's keyboard input
            input_source = "HOST_KEYBOARD";
        } else {
            local_input = (uint8_t)(live_p2_input & 0xFF); // Client's keyboard input  
            input_source = "CLIENT_KEYBOARD";
        }
        
        // DEBUG: Enhanced input sending debug - log all non-zero inputs and periodic status
        static uint32_t debug_frame_count = 0;
        static uint8_t last_sent_input = 0xFF;
        debug_frame_count++;
        
        bool should_log_send = false;
        if (local_input != 0 && debug_frame_count % 100 == 0) should_log_send = true; // Log non-zero every 100 frames
        if (local_input != last_sent_input) should_log_send = true; // Log changes
        if (debug_frame_count % 600 == 0) should_log_send = true; // Periodic (every 10 seconds)
        
        if (should_log_send) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT_SEND: %s handle=%d sending=0x%02X (live_p1=0x%02X, live_p2=0x%02X) frame=%u", 
                       input_source, local_player_handle, local_input, 
                       live_p1_input & 0xFF, live_p2_input & 0xFF, g_frame_counter);
            last_sent_input = local_input;
        }
        
        gekko_add_local_input(gekko_session, local_player_handle, &local_input);
        
        // INPUT TIMING LOGGING: Log input changes with frame numbers
        static uint8_t last_local_input = 0;
        if (local_input != last_local_input) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT TIMING: Handle %d frame %u - input changed 0x%02X → 0x%02X", 
                       local_player_handle, g_frame_counter, last_local_input, local_input);
            last_local_input = local_input;
        }
        
        // Process GekkoNet events following the example pattern
        gekko_network_poll(gekko_session);
        
        // First handle session events (disconnects, desyncs)
        int session_event_count = 0;
        auto session_events = gekko_session_events(gekko_session, &session_event_count);
        for (int i = 0; i < session_event_count; i++) {
            auto event = session_events[i];
            if (event->type == DesyncDetected) {
                auto desync = event->data.desynced;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DESYNC: frame %d, remote handle %d, local checksum %u, remote checksum %u", 
                           desync.frame, desync.remote_handle, desync.local_checksum, desync.remote_checksum);
            } else if (event->type == PlayerDisconnected) {
                auto disco = event->data.disconnected;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "DISCONNECT: player handle %d", disco.handle);
            }
        }
        
        // Then handle game updates  
        int update_count = 0;
        auto updates = gekko_update_session(gekko_session, &update_count);
        
        for (int i = 0; i < update_count; i++) {
            auto update = updates[i];
            
            switch (update->type) {
                case AdvanceEvent: {
                    // CRITICAL DEBUG: Log the exact inputs received from GekkoNet
                    uint8_t received_p1 = update->data.adv.inputs[0];
                    uint8_t received_p2 = update->data.adv.inputs[1];
                    
                    // Only log ADVANCE_EVENT_RAW for non-zero inputs or occasionally
                    static uint32_t advance_log_count = 0;
                    if ((received_p1 != 0 || received_p2 != 0) && (++advance_log_count % 200 == 0)) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                   "ADVANCE_EVENT_RAW: Frame %u - GekkoNet delivered P1=0x%02X, P2=0x%02X (orig_player=%d, is_host=%s)", 
                                   update->data.adv.frame, received_p1, received_p2, original_player_index, is_host ? "YES" : "NO");
                    }
                    
                    // Always apply the synchronized inputs first.
                    networked_p1_input = received_p1;
                    networked_p2_input = received_p2;
                    use_networked_inputs = true;

                    // Check if the remote player sent a confirmation signal.
                    uint8_t remote_input = is_host ? networked_p2_input : networked_p1_input;
                    if (remote_input == 0xFF) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ADVANCE EVENT: Remote player sent 0xFF confirmation signal");
                        FM2K::CSS::g_css_sync.ReceiveRemoteConfirmation();
                        
                        // IMPORTANT: Filter out 0xFF from normal gameplay inputs
                        // Replace 0xFF with 0x00 to prevent invalid game inputs
                        if (is_host) {
                            networked_p2_input = 0x00;
                        } else {
                            networked_p1_input = 0x00;
                        }
                        
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ADVANCE EVENT: Filtered out 0xFF confirmation signal for gameplay");
                    }

                    // DEBUG: Log received inputs when they change or have non-zero values
                    static uint8_t last_p1_input = 0xFF;
                    static uint8_t last_p2_input = 0xFF;
                    static uint32_t last_advance_log_frame = 0;
                    static uint32_t input_hold_frames = 0;
                    
                    bool input_changed = (networked_p1_input != last_p1_input || networked_p2_input != last_p2_input);
                    bool has_input = (networked_p1_input != 0 || networked_p2_input != 0);
                    bool periodic_log = (update->data.adv.frame - last_advance_log_frame >= 600); // Log every 600 frames (10 seconds)
                    
                    // CRITICAL FIX: Reduce logging frequency to prevent console flooding
                    if (input_changed) {
                        // Log input changes immediately
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ADVANCE_EVENT_PROCESSED: Frame %u - Using P1=0x%02X, P2=0x%02X (changed=%s)", 
                                   update->data.adv.frame, networked_p1_input, networked_p2_input, "YES");
                        last_p1_input = networked_p1_input;
                        last_p2_input = networked_p2_input;
                        last_advance_log_frame = update->data.adv.frame;
                        input_hold_frames = 0;
                    } else if (has_input) {
                        // For held inputs, only log every 300 frames (5 seconds) to prevent flooding
                        input_hold_frames++;
                        if (input_hold_frames % 300 == 0) {
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ADVANCE_EVENT_HELD: Frame %u - Holding P1=0x%02X, P2=0x%02X (hold_frames=%u)", 
                                       update->data.adv.frame, networked_p1_input, networked_p2_input, input_hold_frames);
                        }
                    } else if (periodic_log) {
                        // Periodic status log when no inputs
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ADVANCE_EVENT_IDLE: Frame %u - No inputs (P1=0x%02X, P2=0x%02X)", 
                                   update->data.adv.frame, networked_p1_input, networked_p2_input);
                        last_advance_log_frame = update->data.adv.frame;
                    }

                    // CRITICAL TIMING ANALYSIS: Track input changes and their effects
                    static uint8_t last_p1_direction = 0xFF;
                    static uint8_t last_p2_direction = 0xFF;
                    uint8_t p1_direction = networked_p1_input & 0x0F;  // Extract direction bits
                    uint8_t p2_direction = networked_p2_input & 0x0F;  // Extract direction bits
                    
                    bool p1_direction_changed = (p1_direction != last_p1_direction);
                    bool p2_direction_changed = (p2_direction != last_p2_direction);
                    
                    if (p1_direction_changed || p2_direction_changed) {
                        // CRITICAL FIX: Only log direction changes, not held inputs
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                   "DIRECTION_CHANGE: Frame %u - P1: 0x%02X→0x%02X, P2: 0x%02X→0x%02X (p1_changed=%s, p2_changed=%s)", 
                                   update->data.adv.frame, 
                                   last_p1_direction, p1_direction,
                                   last_p2_direction, p2_direction,
                                   p1_direction_changed ? "YES" : "NO",
                                   p2_direction_changed ? "YES" : "NO");
                        
                        last_p1_direction = p1_direction;
                        last_p2_direction = p2_direction;
                    }

                    // DEBUG: Before calling original_process_inputs, log current game state
                    if (has_input || input_changed) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                   "ADVANCE_GAME_STEP: About to call original_process_inputs() with networked inputs active");
                    }

                    // Now, let the original game code run with the synchronized inputs.
                    if (original_process_inputs) {
                        original_process_inputs();
                    }
                    g_frame_counter++;
                    
                    // CRITICAL: Track cursor position changes after input processing
                    if (p1_direction_changed || p2_direction_changed) {
                        // Read cursor positions after input processing
                        uint32_t p1_cursor_x = 0, p1_cursor_y = 0;
                        uint32_t p2_cursor_x = 0, p2_cursor_y = 0;
                        
                        if (!IsBadReadPtr((void*)0x424E50, sizeof(uint32_t) * 2)) {
                            uint32_t* p1_cursor = (uint32_t*)0x424E50;
                            p1_cursor_x = p1_cursor[0];
                            p1_cursor_y = p1_cursor[1];
                        }
                        
                        if (!IsBadReadPtr((void*)0x424E58, sizeof(uint32_t) * 2)) {
                            uint32_t* p2_cursor = (uint32_t*)0x424E58;
                            p2_cursor_x = p2_cursor[0];
                            p2_cursor_y = p2_cursor[1];
                        }
                        
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                   "CURSOR_AFTER_INPUT: Frame %u - P1_cursor=(%d,%d) P2_cursor=(%d,%d) after direction change", 
                                   update->data.adv.frame, p1_cursor_x, p1_cursor_y, p2_cursor_x, p2_cursor_y);
                    }
                    
                    // DEBUG: After game step
                    if (has_input || input_changed) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                   "ADVANCE_GAME_DONE: original_process_inputs() completed, frame now %u", g_frame_counter);
                    }
                    break;
                }
                    
                case SaveEvent: {
                    // Query the state machine to determine the current strategy
                    auto strategy = FM2K::State::g_game_state_machine.GetSyncStrategy();

                    if (strategy == FM2K::State::SyncStrategy::ROLLBACK) {
                        // We are in active, stable battle. Perform a full state save.
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SaveEvent: Full Rollback Save at frame %u", update->data.save.frame);
                        
                        try {
                            // Scan active objects in the pool with error checking
                            auto active_objects = FM2K::ObjectPool::Scanner::ScanActiveObjects();
                            
                            // Create rollback state
                            FM2K::ObjectPool::ObjectPoolState pool_state;
                            pool_state.frame_number = g_frame_counter;
                            pool_state.active_object_count = static_cast<uint32_t>(active_objects.size());
                            pool_state.objects = std::move(active_objects);
                            
                            // Calculate serialization size
                            uint32_t data_size = pool_state.GetSerializedSize();
                            
                            // Verify we don't exceed GekkoNet buffer limits
                            const uint32_t MAX_GEKKO_BUFFER = 4096;
                            if (data_size > MAX_GEKKO_BUFFER) {
                                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                                           "SaveEvent: State size %u > %u, reducing objects", 
                                           data_size, MAX_GEKKO_BUFFER);
                                
                                // Reduce to fit in buffer
                                size_t max_objects = (MAX_GEKKO_BUFFER - 8) / sizeof(FM2K::ObjectPool::CompactObject);
                                if (pool_state.objects.size() > max_objects) {
                                    pool_state.objects.resize(max_objects);
                                    pool_state.active_object_count = static_cast<uint32_t>(max_objects);
                                    data_size = pool_state.GetSerializedSize();
                                }
                            }
                            
                            // Set GekkoNet save data
                            if (update->data.save.state_len) *update->data.save.state_len = data_size;
                            if (update->data.save.checksum) *update->data.save.checksum = g_frame_counter;
                            
                            if (update->data.save.state) {
                                if (pool_state.SerializeTo(update->data.save.state, data_size)) {
                                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                               "Battle SaveEvent frame %u: %u objects, %u bytes", 
                                               update->data.save.frame, pool_state.active_object_count, data_size);
                                } else {
                                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                                               "SaveEvent ERROR: Serialization failed for frame %u", 
                                               update->data.save.frame);
                                }
                            }
                            
                        } catch (...) {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                                       "SaveEvent CRASH: Exception caught in frame %u", 
                                       update->data.save.frame);
                            
                            // Emergency fallback
                            if (update->data.save.state_len) *update->data.save.state_len = 8;
                            if (update->data.save.checksum) *update->data.save.checksum = 0xFFFFFFFF;
                            if (update->data.save.state) {
                                memset(update->data.save.state, 0xFF, 8);
                            }
                        }
                    } else {
                        // We are in lockstep (menus, CSS, or transition). Perform a minimal "dummy" save.
                        // GekkoNet requires a state buffer, but its contents don't matter for lockstep.
                        // Only log lockstep saves periodically to reduce spam
                        static uint32_t last_lockstep_log_frame = 0;
                        if (update->data.save.frame - last_lockstep_log_frame >= 300) { // Log every 300 frames (5 seconds at 60fps)
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SaveEvent: Lockstep (Minimal) Save at frame %u", update->data.save.frame);
                            last_lockstep_log_frame = update->data.save.frame;
                        }
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
                        
                        try {
                            // Validate load data
                            if (!update->data.load.state || update->data.load.state_len < 8) {
                                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                                           "LoadEvent: Invalid state data for frame %u", 
                                           update->data.load.frame);
                                break;
                            }
                            
                            // Deserialize object pool state from GekkoNet buffer
                            FM2K::ObjectPool::ObjectPoolState pool_state;
                            if (pool_state.DeserializeFrom(update->data.load.state, update->data.load.state_len)) {
                                
                                // Restore frame counter
                                g_frame_counter = pool_state.frame_number;
                                
                                // CRITICAL: Clear the entire pool before restoring to prevent stale objects
                                FM2K::ObjectPool::Scanner::ClearObjectPool();
                                
                                // Restore all objects to their exact slots
                                uint32_t restored_count = 0;
                                for (const auto& obj : pool_state.objects) {
                                    if (FM2K::ObjectPool::Scanner::RestoreObjectToSlot(obj)) {
                                        restored_count++;
                                    }
                                }
                                
                                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                           "Battle LoadEvent to frame %u: %u/%u objects restored", 
                                           pool_state.frame_number, restored_count, pool_state.active_object_count);
                                
                            } else {
                                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                                           "LoadEvent ERROR: Failed to deserialize state for frame %u", 
                                           update->data.load.frame);
                            }
                            
                        } catch (...) {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                                       "LoadEvent CRASH: Exception caught in frame %u", 
                                       update->data.load.frame);
                        }
                    } else {
                        // In lockstep mode, we NEVER load state. The game progresses naturally.
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LoadEvent: Ignored during Lockstep frame %u", update->data.load.frame);
                    }
                    break;
                }
            }
        }
    }
    
    return 0; // Return 0 as the game's frame advancement is handled by GekkoNet
}

int __cdecl Hook_UpdateGameState() {
    // Monitor game state transitions for rollback management
    MonitorGameStateTransitions();
    
    // TEMPORARILY DISABLED: Boot analysis (causing console spam and crashes)
    // Track boot sequence objects during early frames
    // static uint32_t update_count = 0;
    // update_count++;
    // if (update_count <= 10 || (update_count % 100 == 0 && update_count <= 1000)) {
    //     FM2K::BootAnalysis::AnalyzeBootSequenceObject();
    // }
    
    // Original logic for GekkoNet session management
    if (gekko_initialized && !gekko_session_started) {
        return 0;
    }
    
    if (original_update_game) {
        return original_update_game();
    }
    return 0;
}

BOOL __cdecl Hook_RunGameLoop() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** REIMPLEMENTING FM2K MAIN LOOP WITH GEKKONET CONTROL ***");
    
    // Set character select mode flag after memory clearing
    uint8_t* char_select_mode_ptr = (uint8_t*)FM2K::State::Memory::CHARACTER_SELECT_MODE_ADDR;
    if (!IsBadReadPtr(char_select_mode_ptr, sizeof(uint8_t))) {
        DWORD old_protect;
        if (VirtualProtect(char_select_mode_ptr, sizeof(uint8_t), PAGE_READWRITE, &old_protect)) {
            *char_select_mode_ptr = 1;
            VirtualProtect(char_select_mode_ptr, sizeof(uint8_t), old_protect, &old_protect);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Set character select mode flag to 1 after memory clearing");
        }
    }
    
    if (!gekko_initialized) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing GekkoNet...");
        if (!InitializeGekkoNet()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet failed, using original loop");
            return original_run_game_loop ? original_run_game_loop() : FALSE;
        }
        
        // TEMPORARILY DISABLED: Object tracker initialization (debugging crashes)
        // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing adaptive object tracker...");
        // FM2K::ObjectTracking::g_object_tracker.Initialize();
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet and object tracking initialized!");
    }
    
    // FM2K timing variables (from IDA analysis of run_game_loop at 0x405AD0)
    uint32_t* g_frame_time_ms = (uint32_t*)0x447EE4;        // Fixed 10ms (100 FPS)
    uint32_t* g_last_frame_time = (uint32_t*)0x447EE8;      // Last frame timestamp
    uint32_t* g_frame_sync_flag = (uint32_t*)0x447EEC;      // Frame sync state
    uint32_t* g_frame_time_delta = (uint32_t*)0x447EF0;     // Frame timing delta
    uint32_t* g_frame_skip_count = (uint32_t*)0x447EF4;     // Frame skip counter
    
    // EXACT FM2K INITIALIZATION (from decompiled run_game_loop)
    *g_frame_time_ms = 10;                                  // Initialize game loop timing - Fixed 100 FPS (10ms per frame)
    *g_last_frame_time = timeGetTime();
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Running initial 8 warmup frames...");
    int init_loop_count = 8;                                // Initial game state warmup - Run 8 frames of game logic
    do {
        if (original_update_game) {
            original_update_game();                         // update_game_state() call
        }
        --init_loop_count;
    } while (init_loop_count);
    
    *g_last_frame_time = timeGetTime();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Warmup complete, starting GekkoNet-controlled main loop...");
    
    // Wait for GekkoNet connection before starting main game loop
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Waiting for GekkoNet connection...");
    uint32_t connection_attempts = 0;
    while (!AllPlayersValid() && connection_attempts < 1500) {  // 15 second timeout
        gekko_network_poll(gekko_session);
        int temp_count = 0;
        gekko_update_session(gekko_session, &temp_count);
        
        // Process Windows messages during connection
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return TRUE;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        
        Sleep(10);
        connection_attempts++;
        
        if (connection_attempts % 100 == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Connection attempt %u/1500...", connection_attempts);
        }
    }
    
    if (!AllPlayersValid()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Connection timeout! Falling back to original loop.");
        return original_run_game_loop ? original_run_game_loop() : FALSE;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet connected! Calling original FM2K loop...");
    gekko_session_started = true;
    
    // SIMPLE APPROACH: Just call the original run_game_loop
    // Our Hook_ProcessGameInputs will handle the rollback logic
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Delegating to original FM2K main loop...");
    
    if (original_run_game_loop) {
        return original_run_game_loop();
    }
    
    return FALSE;
}

bool InitializeHooks() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing MinHook...");
    
    MH_STATUS mh_init = MH_Initialize();
    if (mh_init != MH_OK && mh_init != MH_ERROR_ALREADY_INITIALIZED) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: MH_Initialize failed: %d", mh_init);
        return false;
    }
    
    if (IsBadCodePtr((FARPROC)FM2K::State::Memory::PROCESS_INPUTS_ADDR) || IsBadCodePtr((FARPROC)FM2K::State::Memory::GET_PLAYER_INPUT_ADDR) || 
        IsBadCodePtr((FARPROC)FM2K::State::Memory::UPDATE_GAME_ADDR) || IsBadCodePtr((FARPROC)FM2K::State::Memory::RUN_GAME_LOOP_ADDR)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Target addresses are invalid or not yet mapped");
        return false;
    }
    
    void* inputFuncAddr = (void*)FM2K::State::Memory::PROCESS_INPUTS_ADDR;
    MH_STATUS status1 = MH_CreateHook(inputFuncAddr, (void*)Hook_ProcessGameInputs, (void**)&original_process_inputs);
    if (status1 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to create input hook: %d", status1);
        MH_Uninitialize();
        return false;
    }
    
    MH_STATUS enable1 = MH_EnableHook(inputFuncAddr);
    if (enable1 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to enable input hook: %d", enable1);
        MH_Uninitialize();
        return false;
    }
    
    void* getInputFuncAddr = (void*)FM2K::State::Memory::GET_PLAYER_INPUT_ADDR;
    MH_STATUS status_getinput = MH_CreateHook(getInputFuncAddr, (void*)Hook_GetPlayerInput, (void**)&original_get_player_input);
    if (status_getinput != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to create get_player_input hook: %d", status_getinput);
        MH_Uninitialize();
        return false;
    }

    MH_STATUS enable_getinput = MH_EnableHook(getInputFuncAddr);
    if (enable_getinput != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to enable get_player_input hook: %d", enable_getinput);
        MH_Uninitialize();
        return false;
    }
    
    void* updateFuncAddr = (void*)FM2K::State::Memory::UPDATE_GAME_ADDR;
    MH_STATUS status2 = MH_CreateHook(updateFuncAddr, (void*)Hook_UpdateGameState, (void**)&original_update_game);
    if (status2 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to create update hook: %d", status2);
        MH_Uninitialize();
        return false;
    }
    
    MH_STATUS enable2 = MH_EnableHook(updateFuncAddr);
    if (enable2 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to enable update hook: %d", enable2);
        MH_Uninitialize();
        return false;
    }
    
    void* runGameLoopFuncAddr = (void*)FM2K::State::Memory::RUN_GAME_LOOP_ADDR;
    MH_STATUS status3 = MH_CreateHook(runGameLoopFuncAddr, (void*)Hook_RunGameLoop, (void**)&original_run_game_loop);
    if (status3 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to create run_game_loop hook: %d", status3);
        MH_Uninitialize();
        return false;
    }
    
    MH_STATUS enable3 = MH_EnableHook(runGameLoopFuncAddr);
    if (enable3 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to enable run_game_loop hook: %d", enable3);
        MH_Uninitialize();
        return false;
    }
    
    // Apply boot-to-character-select patches directly
    ApplyBootToCharacterSelectPatches();
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SUCCESS FM2K HOOK: BSNES-level architecture installed successfully!");
    return true;
}

void ShutdownHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Hooks shut down");
}

// Game state monitoring implementation
void MonitorGameStateTransitions() {
    // Read current game mode values from memory
    uint32_t* game_mode_ptr = (uint32_t*)FM2K::State::Memory::GAME_MODE_ADDR;
    uint32_t* fm2k_mode_ptr = (uint32_t*)FM2K::State::Memory::FM2K_GAME_MODE_ADDR;
    uint32_t* char_select_ptr = (uint32_t*)FM2K::State::Memory::CHARACTER_SELECT_MODE_ADDR;
    
    // Safely read values
    uint32_t new_game_mode = 0xFFFFFFFF;
    uint32_t new_fm2k_mode = 0xFFFFFFFF;
    uint32_t new_char_select = 0xFFFFFFFF;
    
    if (!IsBadReadPtr(game_mode_ptr, sizeof(uint32_t))) {
        new_game_mode = *game_mode_ptr;
    }
    if (!IsBadReadPtr(fm2k_mode_ptr, sizeof(uint32_t))) {
        new_fm2k_mode = *fm2k_mode_ptr;
    }
    if (!IsBadReadPtr(char_select_ptr, sizeof(uint32_t))) {
        new_char_select = *char_select_ptr;
    }
    
    // Update the game state machine with current mode
    if (new_game_mode != 0xFFFFFFFF) {
        FM2K::State::g_game_state_machine.Update(new_game_mode);
    }
    
    // Check for state transitions and log them
    bool state_changed = false;
    if (new_game_mode != current_game_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: game_mode changed from %s (0x%08X) to %s (0x%08X)", 
                   GetGameModeString(current_game_mode), current_game_mode,
                   GetGameModeString(new_game_mode), new_game_mode);
        current_game_mode = new_game_mode;
        state_changed = true;
        
        // Log character select state when in CSS mode (2000-2999)
        if (new_game_mode >= 2000 && new_game_mode < 3000) {
            uint32_t* menu_sel_ptr = (uint32_t*)FM2K::State::Memory::MENU_SELECTION_ADDR;
            uint32_t* p1_cursor_x_ptr = (uint32_t*)FM2K::State::Memory::P1_CSS_CURSOR_X_ADDR;
            uint32_t* p1_cursor_y_ptr = (uint32_t*)FM2K::State::Memory::P1_CSS_CURSOR_Y_ADDR;
            uint32_t* p2_cursor_x_ptr = (uint32_t*)FM2K::State::Memory::P2_CSS_CURSOR_X_ADDR;
            uint32_t* p2_cursor_y_ptr = (uint32_t*)FM2K::State::Memory::P2_CSS_CURSOR_Y_ADDR;
            uint32_t* p1_char_ptr = (uint32_t*)FM2K::State::Memory::P1_SELECTED_CHAR_ADDR;
            uint32_t* p2_char_ptr = (uint32_t*)FM2K::State::Memory::P2_SELECTED_CHAR_ADDR;
            uint32_t* p1_confirmed_ptr = (uint32_t*)FM2K::State::Memory::P1_CSS_CONFIRMED_ADDR;
            uint32_t* p2_confirmed_ptr = (uint32_t*)FM2K::State::Memory::P2_CSS_CONFIRMED_ADDR;
            
            if (!IsBadReadPtr(menu_sel_ptr, sizeof(uint32_t)) && !IsBadReadPtr(p1_cursor_x_ptr, sizeof(uint32_t)) && 
                !IsBadReadPtr(p1_cursor_y_ptr, sizeof(uint32_t)) && !IsBadReadPtr(p2_cursor_x_ptr, sizeof(uint32_t)) && 
                !IsBadReadPtr(p2_cursor_y_ptr, sizeof(uint32_t)) && !IsBadReadPtr(p1_char_ptr, sizeof(uint32_t)) && 
                !IsBadReadPtr(p2_char_ptr, sizeof(uint32_t)) && !IsBadReadPtr(p1_confirmed_ptr, sizeof(uint32_t)) && 
                !IsBadReadPtr(p2_confirmed_ptr, sizeof(uint32_t))) {
                
                uint32_t menu_sel = *menu_sel_ptr;
                uint32_t p1_cursor_x = *p1_cursor_x_ptr;
                uint32_t p1_cursor_y = *p1_cursor_y_ptr;
                uint32_t p2_cursor_x = *p2_cursor_x_ptr;
                uint32_t p2_cursor_y = *p2_cursor_y_ptr;
                uint32_t p1_char = *p1_char_ptr;
                uint32_t p2_char = *p2_char_ptr;
                uint32_t p1_confirmed = *p1_confirmed_ptr;
                uint32_t p2_confirmed = *p2_confirmed_ptr;
                
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS STATE: menu=%d, P1_cursor=(%d,%d), P2_cursor=(%d,%d), P1_char=%d, P2_char=%d, confirmed=(%d,%d)", 
                           menu_sel, p1_cursor_x, p1_cursor_y, p2_cursor_x, p2_cursor_y, p1_char, p2_char, p1_confirmed, p2_confirmed);
            }
        }
    }
    
    if (new_fm2k_mode != current_fm2k_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: fm2k_mode changed from 0x%08X to 0x%08X", 
                   current_fm2k_mode, new_fm2k_mode);
        current_fm2k_mode = new_fm2k_mode;
        state_changed = true;
    }
    
    if (new_char_select != current_char_select_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: char_select_mode changed from 0x%08X to 0x%08X", 
                   current_char_select_mode, new_char_select);
        current_char_select_mode = new_char_select;
        state_changed = true;
    }
    
    // Manage rollback activation based on state changes
    if (state_changed) {
        ManageRollbackActivation(new_game_mode, new_fm2k_mode, new_char_select);
    }
    
    // Mark as initialized after first read
    if (!game_state_initialized) {
        game_state_initialized = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: Initial state - game_mode=0x%08X, fm2k_mode=0x%08X, char_select=0x%08X", 
                   new_game_mode, new_fm2k_mode, new_char_select);
    }
}

void ManageRollbackActivation(uint32_t game_mode, uint32_t fm2k_mode, uint32_t char_select_mode) {
    // SIMPLIFIED: Use state machine to determine rollback activation
    bool should_activate_rollback = FM2K::State::g_game_state_machine.ShouldEnableRollback();
    bool should_use_lockstep = FM2K::State::g_game_state_machine.ShouldUseLockstep();
    bool in_stabilization = FM2K::State::g_game_state_machine.IsInTransitionStabilization();
    
    // Determine if we need to be waiting for GekkoNet to advance the frame
    bool needs_frame_sync = (should_activate_rollback || should_use_lockstep) && !in_stabilization;

    // CRITICAL: Disable rollback during transition stabilization to prevent desyncs
    if (in_stabilization && waiting_for_gekko_advance) {
        waiting_for_gekko_advance = false;
        rollback_active = false;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "FM2K STATE: Disabling frame sync for stabilization (phase: %d, frames: %d)", 
            static_cast<int>(FM2K::State::g_game_state_machine.GetCurrentPhase()),
            FM2K::State::g_game_state_machine.GetFramesInCurrentPhase());
    }
    
    // Activate frame synchronization if we need either rollback or lockstep
    if (needs_frame_sync && !waiting_for_gekko_advance) {
        waiting_for_gekko_advance = true;
        rollback_active = should_activate_rollback; // Only set rollback_active if we're actually in battle
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "FM2K STATE: Activating %s sync (game_mode=0x%X)", 
            rollback_active ? "ROLLBACK" : "LOCKSTEP",
            game_mode);
        
    } else if (!needs_frame_sync && waiting_for_gekko_advance) {
        // Deactivate frame synchronization (returning to menu, etc.)
        waiting_for_gekko_advance = false;
        rollback_active = false;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: Deactivating frame sync (game_mode=0x%X)", game_mode);
    }
}

bool ShouldActivateRollback(uint32_t game_mode, uint32_t fm2k_mode) {
    // UPDATED: Use state machine logic instead of always returning true
    // This function is legacy - the state machine handles this now
    return FM2K::State::g_game_state_machine.ShouldEnableRollback();
}

const char* GetGameModeString(uint32_t mode) {
    switch (mode) {
        case 0xFFFFFFFF: return "UNINITIALIZED";
        case 0x0: return "STARTUP";
        default:
            if (mode >= 1000 && mode < 2000) return "TITLE_SCREEN";
            if (mode >= 2000 && mode < 3000) return "CHARACTER_SELECT";
            if (mode >= 3000 && mode < 4000) return "IN_BATTLE";
            return "UNKNOWN";
    }
} 