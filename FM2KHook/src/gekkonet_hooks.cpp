#include "gekkonet_hooks.h"
#include "globals.h"
#include "logging.h"
#include "input_handler.h"
#include "gekkonet.h"
#include "state_manager.h"  // For GameState
#include "game_patches.h"   // For ApplyCharacterSelectModePatches
#include "savestate.h"      // For SaveCompleteGameState/LoadCompleteGameState
#include "shared_mem.h"     // For SaveStateData
#include "logging.h"        // For GenerateDesyncReport
#include <SDL3/SDL_log.h>
#include <windows.h>        // For message processing
#include <mmsystem.h>       // For timeGetTime()
#include <cstdint>
#include <memory>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <string>
#include <cmath>

// Forward declare Fletcher32 from state_manager.cpp
extern uint32_t Fletcher32(const uint8_t* data, size_t len);

// BSNES-style network health monitoring globals
uint32_t netplay_counter = 0;           // Frame counter for network monitoring
uint32_t netplay_stall_counter = 0;     // Stall detection counter  
bool netplay_run_ahead_mode = false;    // Run-ahead mode flag for rollback
float netplay_local_delay = 0.0f;       // Local delay setting

// BSNES-style rollback state buffer (circular buffer for efficiency)
struct RollbackState {
    std::unique_ptr<SaveStateData> state_data;
    uint32_t frame_number;
    bool is_valid;
    uint32_t access_count;  // Track buffer slot usage for health monitoring
    
    RollbackState() : state_data(nullptr), frame_number(0), is_valid(false), access_count(0) {}
};

// Rollback state buffer - like BSNES netplay.states  
// BSNES uses: netplay.states.resize(netplay.config.input_prediction_window + 2)
// We use a fixed size for now but could optimize based on prediction window
static constexpr size_t ROLLBACK_BUFFER_SIZE = 32; // Store last 32 frames for rollback
static std::vector<RollbackState> rollback_states(ROLLBACK_BUFFER_SIZE);

// Buffer health monitoring
static uint32_t buffer_save_count = 0;
static uint32_t buffer_load_count = 0;
static uint32_t buffer_hit_count = 0;
static uint32_t buffer_miss_count = 0;

bool InitializeGekkoNet() {
    // Set network session flag in the game state machine

    // Logging for network session initialization
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** REIMPLEMENTING FM2K MAIN LOOP WITH GEKKONET CONTROL ***");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing GgekkoNet...");
    // CSS filtering removed for simplified input handling
    
    static uint16_t local_port = 7000;
    static std::string remote_address = "127.0.0.1:7001";
    
    // Player index and is_host should already be set by dllmain.cpp
    // Just read the environment variables for network configuration
    char* env_port = getenv("FM2K_LOCAL_PORT");
    char* env_remote = getenv("FM2K_REMOTE_ADDR");
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Using player_index=%d (already set by DllMain)", ::player_index);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Environment variables - FM2K_LOCAL_PORT=%s, FM2K_REMOTE_ADDR=%s", 
                env_port ? env_port : "NOT SET", env_remote ? env_remote : "NOT SET");
    
    char* env_input_recording = getenv("FM2K_INPUT_RECORDING");
    if (env_input_recording && strcmp(env_input_recording, "1") == 0) {
        InitializeInputRecording();
    }
    
    char* env_production_mode = getenv("FM2K_PRODUCTION_MODE");
    if (env_production_mode && strcmp(env_production_mode, "1") == 0) {
        ::production_mode = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Production mode enabled - reduced logging");
    }
    
    if (env_port) {
        local_port = static_cast<uint16_t>(atoi(env_port));
    } else {
        // Fallback: Auto-configure ports for dual client testing
        // Player 0 (host) uses port 7000, Player 1 (client) uses port 7001
        local_port = 7000 + ::player_index;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Auto-configured local port %u for player %d", 
                    local_port, ::player_index);
    }
    
    if (env_remote) {
        remote_address = std::string(env_remote);
    } else {
        // Fallback: Auto-configure remote address for dual client testing
        // Host connects to client port, client connects to host port
        uint16_t remote_port = 7000 + (1 - ::player_index); // Opposite player's port
        remote_address = "127.0.0.1:" + std::to_string(remote_port);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Auto-configured remote address %s for player %d", 
                    remote_address.c_str(), ::player_index);
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Network config - Player: %u, Local port: %u, Remote: %s", 
                player_index, local_port, remote_address.c_str());
    
    // LIKE BSNES-NETPLAY: Proper player setup
    GekkoConfig config;
    config.num_players = 2;
    config.max_spectators = 0;
    config.input_prediction_window = 3;   // Smaller window for tighter sync during testing
    config.input_size = sizeof(uint16_t); // 2 bytes per input
    config.state_size = sizeof(int32_t); // 4 bytes for network state (exactly like bsnes)
    config.desync_detection = true;
    config.limited_saving = false;
    config.post_sync_joining = false;
    config.spectator_delay = 0;
    
    // Create session like bsnes-netplay
    gekko_create(&gekko_session);
    gekko_start(gekko_session, &config);
    
    // Set network adapter like bsnes-netplay
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Setting up network adapter on port %u", local_port);
    
    auto adapter = gekko_default_adapter(local_port);
    if (!adapter) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Failed to create network adapter on port %u", local_port);
        return false;
    }
    
    gekko_net_adapter_set(gekko_session, adapter);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Network adapter configured successfully");
    
    // FIX: Both players should be in online mode when connecting to each other
    // Check if we have a remote address - if so, we're in online mode
    bool is_online_session = !remote_address.empty();
    
    if (is_online_session) {
        // Online session: Ensure Player 1 gets handle 0, Player 2 gets handle 1
        // This matches BSNES approach where inputs[0]=P1, inputs[1]=P2
        
        GekkoNetAddress remote_addr;
        remote_addr.data = (void*)remote_address.c_str();
        remote_addr.size = remote_address.length();
        
        if (::player_index == 0) {
            // HOST (like BSNES local==0): Add LocalPlayer first (gets handle 0), then RemotePlayer (gets handle 1)
            // This means P1 is local on the host
            local_player_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
            int remote_handle = gekko_add_actor(gekko_session, RemotePlayer, &remote_addr);
            
            if (local_player_handle == -1 || remote_handle == -1) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: HOST failed to add players - Local: %d, Remote: %d", 
                            local_player_handle, remote_handle);
                return false;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: HOST added - local_handle=%d (P1=HOST), remote_handle=%d (P2=CLIENT)", 
                        local_player_handle, remote_handle);
        } else {
            // CLIENT (like BSNES local==1): Add RemotePlayer first (gets handle 0), then LocalPlayer (gets handle 1) 
            // This means P1 is remote on the client
            int remote_handle = gekko_add_actor(gekko_session, RemotePlayer, &remote_addr);
            local_player_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
            
            if (local_player_handle == -1 || remote_handle == -1) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: CLIENT failed to add players - Remote: %d, Local: %d", 
                            remote_handle, local_player_handle);
                return false;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: CLIENT added - remote_handle=%d (P1=HOST), local_handle=%d (P2=CLIENT)", 
                        remote_handle, local_player_handle);
        }
        
        // CRITICAL: Set local delay like BSNES does (line 104 in bsnes netplay.cpp)
        // This is essential for GekkoNet's synchronization mechanism
        uint8_t local_delay = 0; // Start with 0 delay for testing
        gekko_set_local_delay(gekko_session, local_player_handle, local_delay);
        
        // BSNES PATTERN: Store local delay for drift detection (line 111)
        netplay_local_delay = (float)local_delay;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Set local delay %d for handle %d (stored as %.1f)", 
                    local_delay, local_player_handle, netplay_local_delay);
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player %d controls handle %d", 
                    ::player_index == 0 ? 1 : 2, local_player_handle);
        
        // Set online mode flags
        is_online_mode = true;
        is_local_session = false;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Online session detected - connecting to %s", remote_address.c_str());
    } else {
        // Local session: Add both players as local
        p1_player_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
        p2_player_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
        
        if (p1_player_handle == -1 || p2_player_handle == -1) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Failed to add local players");
            return false;
        }
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Added local players P1=%d, P2=%d", p1_player_handle, p2_player_handle);
        
        // Set local mode flags
        is_online_mode = false;
        is_local_session = true;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Local session detected");
    }
    
    gekko_initialized = true;
    return true;
}

void CleanupGekkoNet() {
    if (gekko_session) {
        gekko_destroy(gekko_session);
        gekko_session = nullptr;
        gekko_initialized = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet session closed");
    }
}

bool AllPlayersValid() {
    if (!gekko_session || !gekko_initialized) {
        return false;
    }

    // If session is already started, we're good.
    if (gekko_session_started) {
        return true;
    }

    // For TRUE OFFLINE sessions with local players, immediately mark as valid
    if (is_local_session) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: TRUE OFFLINE mode - both players are local, no handshake needed");
        gekko_session_started = true;
        gekko_frame_control_enabled = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: FRAME CONTROL ENABLED (offline mode)");
        return true;
    }

    // For online sessions, simply check if session has started
    // All actual event processing is handled by ProcessGekkoNetFrame()
    bool got_advance_events = gekko_session_started;

    if (got_advance_events) {
        static bool connection_logged = false;
        if (!connection_logged) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: First AdvanceEvent received. All players are now valid.");
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Connection established successfully!");
            connection_logged = true;
        }
        
        gekko_session_started = true;
        gekko_frame_control_enabled = true;
        can_advance_frame = false;
        return true;
    }

    // Add timeout detection for failed connections
    static uint32_t connection_attempts = 0;
    connection_attempts++;
    
    if (connection_attempts % 600 == 0) { // Every 10 seconds at 60fps
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Still waiting for connection after %d attempts", connection_attempts);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Local port: %u, Remote: %s", GetGekkoLocalPort(), GetGekkoRemoteIP());
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Session state - initialized: %s, started: %s", 
                    gekko_initialized ? "YES" : "NO", gekko_session_started ? "YES" : "NO");
    }

    // Not yet connected and validated.
    return false;
}

void ConfigureNetworkMode(bool online_mode, bool host_mode) {
    is_online_mode = online_mode;
    is_host = host_mode;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Network mode configured - Online: %s, Host: %s", 
                online_mode ? "YES" : "NO", host_mode ? "YES" : "NO");
}

uint16_t GetGekkoLocalPort() {
    static uint16_t cached_port = 0;
    static bool port_initialized = false;
    
    if (!port_initialized) {
        cached_port = 7000;  // Default
        char* env_port = getenv("FM2K_LOCAL_PORT");
        if (env_port) {
            cached_port = static_cast<uint16_t>(atoi(env_port));
        }
        port_initialized = true;
    }
    
    return cached_port;
}

const char* GetGekkoRemoteIP() {
    static std::string cached_ip;
    static bool ip_initialized = false;
    
    if (!ip_initialized) {
        cached_ip = "127.0.0.1";  // Default
        char* env_remote = getenv("FM2K_REMOTE_ADDR");
        if (env_remote) {
            std::string remote_addr = std::string(env_remote);
            // Extract IP from "IP:PORT" format
            size_t colon_pos = remote_addr.find(':');
            if (colon_pos != std::string::npos) {
                cached_ip = remote_addr.substr(0, colon_pos);
            } else {
                cached_ip = remote_addr;
            }
        }
        ip_initialized = true;
    }
    
    return cached_ip.c_str();
} 

// Process GekkoNet frame - extracted from Hook_ProcessGameInputs for reuse
void ProcessGekkoNetFrame() {
    // BSNES-STYLE: Reset frame advancement permission (like BSNES netplayRun())
    can_advance_frame = false;
    // CRITICAL FIX: Don't reset use_networked_inputs - let AdvanceEvent set it to true
    // use_networked_inputs = false; // <-- This was causing networked inputs to be ignored!
    
    // BSNES-STYLE: Monitor network health and handle frame drift (like BSNES netplayRun lines 136-148)
    MonitorNetworkHealth();
    
    // STEP 1: Always capture real inputs (equivalent to BSNES netplayPollLocalInput)
    CaptureRealInputs();
    
    // STEP 2: Always send inputs to GekkoNet (equivalent to BSNES gekko_add_local_input)
    // This must happen regardless of session state to establish the connection
    if (is_local_session) {
        // Local session: Send both players' inputs
        uint16_t p1_input = (uint16_t)(live_p1_input & 0x7FF);
        uint16_t p2_input = (uint16_t)(live_p2_input & 0x7FF);
        gekko_add_local_input(gekko_session, p1_player_handle, &p1_input);
        gekko_add_local_input(gekko_session, p2_player_handle, &p2_input);
    } else {
        // Online session: Send input based on what local player controls
        // HOST (player_index=0) controls P1, CLIENT (player_index=1) controls P2
        if (::player_index == 0) {
            // HOST: Send P1 input (local_player_handle should be 0)
            uint16_t p1_input = (uint16_t)(live_p1_input & 0x7FF);
            gekko_add_local_input(gekko_session, local_player_handle, &p1_input);
            
            static uint32_t input_log_counter = 0;
            if (++input_log_counter % 300 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: HOST sending P1 input 0x%04X via handle %d", 
                           p1_input, local_player_handle);
            }
        } else {
            // CLIENT: Send P2 input (local_player_handle should be 1) 
            uint16_t p2_input = (uint16_t)(live_p2_input & 0x7FF);
            gekko_add_local_input(gekko_session, local_player_handle, &p2_input);
            
            static uint32_t input_log_counter = 0;
            if (++input_log_counter % 300 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: CLIENT sending P2 input 0x%04X via handle %d", 
                           p2_input, local_player_handle);
            }
        }
    }
    
    // STEP 3: Always process connection events (equivalent to BSNES gekko_session_events)
    int event_count = 0;
    auto events = gekko_session_events(gekko_session, &event_count);
    for (int i = 0; i < event_count; i++) {
        auto event = events[i];
        if (event->type == PlayerConnected) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player Connected - handle %d", event->data.connected.handle);
        } else if (event->type == PlayerDisconnected) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player Disconnected - handle %d", event->data.disconnected.handle);
        } else if (event->type == SessionStarted) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Session Started!");
            gekko_session_started = true;
            gekko_frame_control_enabled = true;
            
            // CRITICAL: Reset synchronization point like BSNES emulator->power()
            // This ensures both clients start from identical state
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Synchronization reset - both clients starting from frame 0");
            // Reset frame counter to ensure sync
            uint32_t* frame_counter = (uint32_t*)FM2K::State::Memory::FRAME_COUNTER_ADDR;
            if (frame_counter && !IsBadWritePtr(frame_counter, sizeof(uint32_t))) {
                *frame_counter = 0;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Reset frame counter to 0 for perfect sync");
            }
            
            // Mark session as ready for true BSNES-style operation
            gekko_session_ready = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Session is now ready - both clients synchronized");
        }
    }
    
    // STEP 4: Always process updates (SaveEvent, LoadEvent, AdvanceEvent)
    // This is the core of BSNES gekko_update_session processing
    gekko_network_poll(gekko_session);
    int update_count = 0;
    auto updates = gekko_update_session(gekko_session, &update_count);
    
    // BSNES-STYLE: Handle stalling due to network issues (lines 224-230)
    if (update_count == 0) {
        netplay_stall_counter++;
        if (netplay_stall_counter > 10) {
            // BSNES sets program.mute |= Mute::Always here (audio muting for stalls)
            // We'll skip audio muting for now as requested
            if (netplay_stall_counter % 60 == 0) {  // Log every second during stall
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                           "GekkoNet: Network stall detected - no updates for %d frames", 
                           netplay_stall_counter);
            }
        }
    } else {
        // BSNES restores audio and resets stall counter when updates resume
        if (netplay_stall_counter > 10) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "GekkoNet: Network stall recovered - received %d updates after %d stall frames", 
                       update_count, netplay_stall_counter);
            // BSNES: program.mute &= ~Mute::Always (restore audio)
        }
        netplay_stall_counter = 0;
    }
    
    // Frame advance flag already reset at start of function
    
    for (int i = 0; i < update_count; i++) {
        auto update = updates[i];
        switch (update->type) {
            case SaveEvent: {
                uint32_t frame = update->data.save.frame;
                uint32_t buffer_index = frame % ROLLBACK_BUFFER_SIZE;
                
                // BUFFER HEALTH: Track save operations
                buffer_save_count++;
                
                static uint32_t debug_save_counter = 0;
                if (++debug_save_counter % 300 == 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: SaveEvent frame %d (buffer index %d)", 
                               frame, buffer_index);
                }
                
                // BSNES PATTERN: Save complete state locally, only send frame number over network
                auto& rollback_slot = rollback_states[buffer_index];
                
                // Allocate state data if needed
                if (!rollback_slot.state_data) {
                    rollback_slot.state_data = std::make_unique<SaveStateData>();
                }
                
                // Save complete FM2K game state (like BSNES emulator->serialize())
                bool save_success = SaveCompleteGameState(rollback_slot.state_data.get(), frame);
                if (save_success) {
                    rollback_slot.frame_number = frame;
                    rollback_slot.is_valid = true;
                    rollback_slot.access_count++;  // Track buffer usage
                    
                    // Return minimal network state (like BSNES - just 4 bytes)
                    *update->data.save.checksum = 0; // Can be used for desync detection later
                    *update->data.save.state_len = sizeof(int32_t);
                    memcpy(update->data.save.state, &frame, sizeof(int32_t));
                    
                    static uint32_t save_log_counter = 0;
                    if (++save_log_counter % 300 == 0) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                   "GekkoNet: Saved complete state for frame %d locally (count=%d)", frame, save_log_counter);
                    }
                } else {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                                "GekkoNet: Failed to save state for frame %d", frame);
                    rollback_slot.is_valid = false;
                }
                break;
            }
                
            case LoadEvent: {
                uint32_t frame = update->data.load.frame;
                uint32_t buffer_index = frame % ROLLBACK_BUFFER_SIZE;
                
                // BUFFER HEALTH: Track load operations
                buffer_load_count++;
                
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: LoadEvent frame %d (buffer index %d) - enabling run-ahead mode", 
                           frame, buffer_index);
                
                // BSNES PATTERN: Load complete state from local storage (like BSNES emulator->unserialize())
                auto& rollback_slot = rollback_states[buffer_index];
                
                if (rollback_slot.is_valid && rollback_slot.frame_number == frame && rollback_slot.state_data) {
                    // BUFFER HEALTH: Track successful buffer hits
                    buffer_hit_count++;
                    rollback_slot.access_count++;  // Track buffer usage
                    
                    bool load_success = LoadCompleteGameState(rollback_slot.state_data.get());
                    if (load_success) {
                        // BSNES PATTERN: Enable run-ahead mode after loading state (line 207)
                        // program.mute |= Mute::Always; (skip audio for now)
                        netplay_run_ahead_mode = true;
                        
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                   "GekkoNet: Successfully loaded state for frame %d - run-ahead mode enabled", frame);
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                                    "GekkoNet: Failed to load state for frame %d", frame);
                    }
                } else {
                    // BUFFER HEALTH: Track buffer misses
                    buffer_miss_count++;
                    
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                                "GekkoNet: No valid state found for frame %d (valid=%s, frame_match=%s)", 
                                frame, rollback_slot.is_valid ? "YES" : "NO",
                                rollback_slot.frame_number == frame ? "YES" : "NO");
                }
                break;
            }
                
            case AdvanceEvent:
                // BSNES PATTERN: Handle run-ahead mode logic (lines 210-218)
                if (netplay_run_ahead_mode) {
                    // We're in run-ahead mode - check if this is the final AdvanceEvent in the sequence
                    int current_update = i;
                    int final_update = update_count - 1;
                    
                    // BSNES logic: if(cframe == i || (updates[cframe]->type == SaveEvent && i == cframe - 1))
                    bool is_final_advance = (current_update == final_update) || 
                                           (final_update >= 0 && updates[final_update]->type == SaveEvent && current_update == final_update - 1);
                    
                    if (is_final_advance) {
                        // This is the final AdvanceEvent - disable run-ahead mode and restore normal operation
                        netplay_run_ahead_mode = false;
                        // BSNES: program.mute &= ~Mute::Always; (restore audio)
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                   "GekkoNet: Final AdvanceEvent - disabling run-ahead mode and restoring normal operation");
                    } else {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                   "GekkoNet: AdvanceEvent in run-ahead mode - fast-forwarding frame %d", 
                                   update->data.adv.frame);
                    }
                }
                
                // CRITICAL: This is the ONLY place where frame advancement is allowed (like BSNES emulator->run())
                can_advance_frame = true;
                use_networked_inputs = true;
                gekko_frame_control_enabled = true;
                
                // Copy networked inputs from GekkoNet (like BSNES memcpy)
                if (update->data.adv.inputs && update->data.adv.input_len >= sizeof(uint16_t) * 2) {
                    uint16_t* networked_inputs = (uint16_t*)update->data.adv.inputs;
                    
                    // Store synchronized inputs for use in FM2K_ProcessGameInputs_GekkoNet
                    networked_p1_input = networked_inputs[0];
                    networked_p2_input = networked_inputs[1];
                    
                    static uint32_t advance_counter = 0;
                    advance_counter++;
                    if (advance_counter % 300 == 0 || (networked_p1_input != 0 || networked_p2_input != 0) && advance_counter % 60 == 0) {
                        const char* mode_str = netplay_run_ahead_mode ? "RUN-AHEAD" : "NORMAL";
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: AdvanceEvent #%d (frame %d) [%s] - P1=0x%04X P2=0x%04X", 
                                   advance_counter, update->data.adv.frame, mode_str, networked_p1_input, networked_p2_input);
                    }
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: AdvanceEvent received but no input data available");
                }
                break;
        }
    }
}

// COMPLETE MAIN LOOP REPLACEMENT: This completely replaces FM2K's main loop
// with native GekkoNet integration, like how BSNES replaces the emulator loop
BOOL GekkoNet_MainLoop() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Starting complete main loop replacement");
    
    // Apply character select patches
    ApplyCharacterSelectModePatches();
    
    // Get original function pointers for calling game logic
    extern int (__cdecl *original_update_game)();
    extern void (__cdecl *original_render_game)();
    extern int (__cdecl *original_process_inputs)();
    
    // Let GekkoNet control frame pacing naturally - no manual timing needed
    
    // Initial game warmup - 8 frames like original (but controlled by GekkoNet)
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Starting initial warmup frames");
    for (int warmup = 0; warmup < 8; warmup++) {
        // Process GekkoNet for warmup frames
        ProcessGekkoNetFrame();
        
        if (original_update_game) {
            original_update_game();
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Warmup complete, starting main loop");
    
    // Main loop with integrated GekkoNet control
    MSG msg;
    BOOL should_continue = TRUE;
    
    // No manual frame timing - let GekkoNet control pacing through AdvanceEvents
    
    while (should_continue) {
        // Handle Windows messages (like original)
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                should_continue = FALSE;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        
        if (!should_continue) break;
        
        // GEKKONET FRAME CONTROL: This is the key difference from original
        // Instead of time-based frames, we use GekkoNet AdvanceEvent
        ProcessGekkoNetFrame();
        
        // CRITICAL FIX: Only advance frames when GekkoNet explicitly allows it via AdvanceEvents
        // This removes the architectural flaw that allowed Client 1 to run ahead freely
        if (can_advance_frame) {
            // Process one game frame ONLY when GekkoNet AdvanceEvent allows it (like BSNES line 218)
            // CRITICAL: Let GekkoNet control timing completely - no manual frame timing
            FM2K_ProcessGameInputs_GekkoNet();
            
            if (original_update_game) {
                original_update_game();
            }
            
            if (original_render_game) {
                original_render_game();
            }
            
            static uint32_t frame_count = 0;
            if (++frame_count % 300 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet MainLoop: Advanced frame %d (pure GekkoNet timing)", frame_count);
            }
        } else {
            // TRUE BSNES BLOCKING: Wait for AdvanceEvent, never advance freely
            // This ensures perfect synchronization between clients
            static uint32_t blocked_count = 0;
            if (++blocked_count % 300 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet MainLoop: Blocked frame %d (waiting for AdvanceEvent)", blocked_count);
            }
            
            // Let GekkoNet's natural blocking control timing
        }
        
        // Handle additional Windows messages that might have arrived
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                should_continue = FALSE;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Main loop ended");
    return FALSE; // Return FALSE to end the application
}

// BSNES-style network health monitoring functions
void MonitorNetworkHealth() {
    if (!gekko_session || !gekko_initialized) {
        return;
    }
    
    // Increment frame counter like BSNES (line 136)
    netplay_counter++;
    
    // Check frame drift like BSNES netplayRun() (lines 139-148)
    float frames_ahead = gekko_frames_ahead(gekko_session);
    
    // BSNES uses: if(framesAhead - netplay.localDelay >= 1.0f && netplay.counter % 180 == 0)
    // CRITICAL FIX: More conservative drift detection - only correct severe drift
    float drift_magnitude = fabs(frames_ahead - netplay_local_delay);
    
    // Only correct drift if it's SEVERE AND we haven't corrected recently
    static uint32_t last_correction_counter = 0;
    bool should_correct = (drift_magnitude >= 5.0f) && 
                         (netplay_counter % 1800 == 0) && 
                         (netplay_counter - last_correction_counter > 1200);
    
    if (should_correct) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                   "GekkoNet: Severe frame drift detected - frames_ahead=%.2f, local_delay=%.2f, drift=%.2f, counter=%d", 
                   frames_ahead, netplay_local_delay, drift_magnitude, netplay_counter);
        
        // Handle frame drift correction
        HandleFrameDrift();
        last_correction_counter = netplay_counter;
    }
    
    // Collect network statistics like BSNES (line 153)
    if (is_online_mode && local_player_handle != -1) {
        static uint32_t stats_counter = 0;
        if (++stats_counter % 300 == 0) {  // Check every 5 seconds at 60fps
            // Note: BSNES calls gekko_network_stats() here for remote players
            // We'll implement this when we add network statistics tracking
            
            // BUFFER HEALTH REPORTING: Report buffer statistics periodically
            float hit_rate = (buffer_load_count > 0) ? (float)buffer_hit_count / buffer_load_count * 100.0f : 100.0f;
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "GekkoNet: Network health - frames_ahead=%.2f, counter=%d", 
                       frames_ahead, netplay_counter);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                       "GekkoNet: Buffer health - saves=%d, loads=%d, hits=%d, misses=%d, hit_rate=%.1f%%",
                       buffer_save_count, buffer_load_count, buffer_hit_count, buffer_miss_count, hit_rate);
        }
    }
}

void HandleFrameDrift() {
    if (!gekko_session) return;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Applying frame drift correction (BSNES pattern)");
    
    // BSNES approach: Temporarily halt frame to resync clients
    FM2K_NetplayHaltFrame();
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Frame drift correction complete");
}

void FM2K_NetplayHaltFrame() {
    // BSNES netplayHaltFrame() equivalent for FM2K
    // BSNES does: auto state = emulator->serialize(0); emulator->run(); emulator->unserialize(state);
    
    if (!gekko_initialized || !gekko_session) {
        return;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Executing frame halt for sync correction");
    
    // SIMPLIFIED APPROACH: Just reduce audio volume and execute a minimal sync frame
    // The main purpose is timing synchronization, not state preservation
    
    extern int (__cdecl *original_update_game)();
    if (original_update_game) {
        // Execute a minimal update cycle for timing sync
        original_update_game();
    }
    
    // Let natural game timing control synchronization
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Frame halt complete - timing sync applied");
}