#include "initGame.h"
#include "../states/splash.h"
#include "../states/title.h"
#include "../states/menu.h"
#include "../states/character_select.h"
#include "../states/gameplay.h"
#include "../input/input_manager.h"
#include "../states/vs_transition.h"
#include "../states/online_setup.h"
#include "game_loop.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <cstring>  // For memset in C++
#include <chrono>
#include <thread>   // For sleep_for

// Forward declaration of fletcher32 (defined in state_management.c)
extern "C" uint32_t fletcher32(const uint16_t* data, size_t len);

// Functions are already declared in header file

// Global quit flag
volatile int g_game_should_quit = 0;

// Global frame counter for online mode
int g_current_game_frame = 0;

// Frame counter for offline VS transition
static int frame_counter = 0;

// Old VS transition variables removed - now using simplified system

// Timing functions moved to game_loop.c

// Input functions moved to input_manager.c

// Online Setup Functions moved to online_setup.c

// update_online_setup moved to online_setup.c

// render_online_setup and online_setup_cleanup moved to online_setup.c

// VS Transition variables moved to vs_transition.c

// VS transition functions moved to vs_transition.c

// All VS transition functions moved to vs_transition.c

// initiate_connection moved to online_setup.c

// Offline game logic moved to game_loop.c

// Online game event processing moved to game_loop.c

// Timing types needed for main function
using micro = std::chrono::microseconds;
using gduration = std::chrono::duration<unsigned int, std::ratio<1, 60>>;
using gtime_point = std::chrono::time_point<std::chrono::steady_clock>;
using slow_frame = std::chrono::duration<unsigned int, std::ratio<1, 59>>;
using normal_frame = std::chrono::duration<unsigned int, std::ratio<1, 60>>;
using fast_frame = std::chrono::duration<unsigned int, std::ratio<1, 61>>;
using gclock = std::chrono::steady_clock;

// Main game initialization function (like OnlineSession.cpp)
int initGame(SDL_Renderer *renderer, SDL_Window *window) {
    // Use FIXED seed for deterministic behavior - CRITICAL for netcode!
    srand(12345);
    
    // Initialize graphics system
    GraphicsContext graphics = {0};
    if (!graphics_init(renderer, &graphics)) {
        printf("Failed to initialize graphics system\n");
        return 0;
    }
    
    // CRITICAL: Set graphics context for ArgentumGameStates (if being used)
    // This allows the mlfixtest argentum system to use the same graphics context
    #ifdef ARGENTUM_GAME_STATES
    extern void SetArgentumGraphicsContext(void* graphics_context);
    SetArgentumGraphicsContext(&graphics);
    #endif
    
    // CRITICAL: Initialize global VS portrait system at startup (rollback-safe)
    if (!init_global_vs_portraits("assets")) {
        printf("Failed to initialize global VS portrait system\n");
        // Continue anyway - VS transitions will just show placeholder text
    }
    
    // Create single GekkoNet session (like OnlineSession.cpp)
    GekkoSession* sess = NULL;
    int local_handle = 0;  // Handle for local player in online session
    gekko_create(&sess);
    
    GekkoConfig conf = {0};
    conf.num_players = 2;
    conf.input_size = sizeof(char);  // Match OnlineSession.cpp
    conf.max_spectators = 0;
    conf.input_prediction_window = 10;  // Match OnlineSession.cpp
    conf.state_size = sizeof(RollbackGameState);
    conf.desync_detection = true;
    conf.limited_saving = false;
    
    gekko_start(sess, &conf);
    
    // For now, start without network adapter (offline mode)
    // Network will be set up when transitioning to online mode
    
    // Initialize single game state
    GameState state = STATE_BOOT_SPLASH;
    RollbackGameState rollback_state = {};  // C++ style initialization
    
    // Initialize contexts with DETERMINISTIC values
    BootSplashContext splash = {
        .frame_count = 0, .done = false, .splash_text = "MORIMOTO UNIVERSE", .max_frames = 300
    };
    TitleScreenContext title = {0};  // Zero-initialize for determinism
    OnlineSetupContext online_setup = {0};  // Zero-initialize for determinism  
    MenuContext menu;  // Will be initialized properly below
    CharacterSelectContext charsel = {0};
    GameplayContext gameplay;
    memset(&gameplay, 0, sizeof(GameplayContext));
    
    // Zero-initialize menu context properly to avoid enum conversion error
    memset(&menu, 0, sizeof(MenuContext));
    
    // Initialize all contexts
    title_screen_init(&title, "assets");
    online_setup_init(&online_setup);
    menu_init(&menu, "assets");
    character_select_init(&charsel, "assets", false);
    
    // Set IDENTICAL initial states for both instances
    menu.state = MENU_MAIN;
    menu.selection = 0;
    menu.difficulty_setting = 1;
    menu.rounds_setting = 1;
    menu.time_setting = 0;
    menu.inactivity_timer = 0;
    menu.transition_requested = false;
    menu.just_entered = false;
    menu.prev_input = 0;
    
    charsel.selected_p1 = 0;
    charsel.selected_p2 = 1;
    charsel.selected_character_p1 = -1;
    charsel.selected_character_p2 = -1;
    charsel.p1_confirmed = false;
    charsel.p2_confirmed = false;
    charsel.done = false;
    charsel.just_entered = false;
    charsel.prev_input_p1 = 0;
    charsel.prev_input_p2 = 0;
    charsel.inactivity_timer = 0;
    
    int running = 1;
    
    printf(" MOON LIGHTS 2 - GAME STARTED!\n");
    printf(" P1 Controls: WASD + Space/Enter + ZXC\n");
    printf(" P2 Controls: Arrow Keys + Backslash + UIO\n");
    
    // Simple timing for offline mode (60fps target)
    auto last_frame_time = gclock::now();
    const auto target_frame_duration = std::chrono::duration_cast<std::chrono::microseconds>(normal_frame(1));
    
    // Main game loop (simple 60fps timing for offline mode)
    while (running && !g_game_should_quit) {
        auto frame_start = gclock::now();
        
        // SDL event polling
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = 0;
                g_game_should_quit = 1;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = 0;
                g_game_should_quit = 1;
            }
        }
        
        // Get both P1 and P2 inputs for offline mode
        GInput p1_input = get_p1_input();
        GInput p2_input = get_p2_input();
        unsigned char input_p1 = p1_input.input.value;
        unsigned char input_p2 = p2_input.input.value;
        
        // Use unified game logic with both players (same as online mode!)
        process_offline_logic(&state, &splash, &title, &online_setup, &menu, 
                            &charsel, &gameplay, &graphics, input_p1, input_p2);
        
        // Handle VS transition completion check 
        if (state == STATE_VS_TRANSITION) {
            frame_counter++;
            
            // Check if VS transition is done (same logic as online)
            if (is_vs_transition_done(frame_counter)) {
                state = STATE_GAMEPLAY;
                reset_vs_transition();
                frame_counter = 0;  // Reset for next time
                
                // Initialize gameplay context for battle
                gameplay_init(&gameplay);
                printf(" OFFLINE VS TRANSITION: Complete, moving to gameplay! Gameplay initialized.\n");
            }
        } else {
            frame_counter = 0;  // Reset if not in VS state
        }
        
        // Handle online connection initiation (only for offline mode)
        if (state == STATE_ONLINE_SETUP && online_setup.connection_active) {
            static bool connection_initiated = false;
            if (!connection_initiated) {
                printf(" Starting online connection...\n");
                if (initiate_connection(sess, &online_setup, &local_handle)) {
                    printf(" Network configured! Switching to online mode...\n");
                    connection_initiated = true;
                    
                    // CRITICAL: Completely reset ALL contexts to identical baseline state
                    state = STATE_CHARACTER_SELECT;
                    
                    // Note: VS portraits are already loaded globally at startup
                    
                    // Reset splash to clean state
                    splash.frame_count = 0;
                    splash.done = false;
                    splash.max_frames = 300;
                    
                    // Reset title to clean state
                    title.just_entered = false;
                    title.done = false;
                    title.selected = 0;  // Reset to Start option
                    title.inactivity_timer = 0;
                    title.prev_input = 0;
                    
                    // Reset online setup to identical clean state
                    online_setup.just_entered = false;
                    online_setup.selection = 0;
                    strcpy(online_setup.local_port, "7000");
                    strcpy(online_setup.remote_addr, "127.0.0.1:7001");
                    strcpy(online_setup.local_delay, "2");
                    online_setup.editing_field = -1;
                    online_setup.connection_active = false;
                    online_setup.connection_failed = false;
                    online_setup.connection_established = true;  // Mark as established
                    online_setup.prev_input = 0;
                    
                    // Reset menu to clean state
                    menu.state = MENU_MAIN;
                    menu.selection = 0;
                    menu.inactivity_timer = 0;
                    menu.transition_requested = false;
                    menu.just_entered = false;
                    menu.prev_input = 0;
                    menu.difficulty_setting = 1;
                    menu.rounds_setting = 1;
                    menu.time_setting = 0;
                    
                    // Reset character select to clean state
                    charsel.selected_p1 = 0;
                    charsel.selected_p2 = 1;
                    charsel.done = false;
                    charsel.just_entered = true;
                    charsel.prev_input_p1 = 0;
                    charsel.prev_input_p2 = 0;
                    charsel.inactivity_timer = 0;
                    charsel.selected_character_p1 = -1;
                    charsel.selected_character_p2 = -1;
                    charsel.p1_confirmed = false;
                    charsel.p2_confirmed = false;
                    charsel.team_battle_mode = false;
                    charsel.p1_selection_count = 0;
                    charsel.p2_selection_count = 0;
                    memset(charsel.p1_team, 0, sizeof(int) * 3);
                    memset(charsel.p2_team, 0, sizeof(int) * 3);
                    charsel.current_p1_pattern = 0;
                    charsel.current_p2_pattern = 0;
                    
                    save_game_state(&rollback_state, state, &splash, &title, &online_setup, &menu, &charsel, NULL);
                    printf(" Synchronized state saved - both instances in character select\n");
                    
                    // Switch to online timing-based loop
                    goto online_mode;
                } else {
                    printf(" Failed to configure network\n");
                    online_setup.connection_failed = true;
                    online_setup.connection_active = false;
                }
            }
        }
        
        // Render current state
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        
        switch (state) {
            case STATE_BOOT_SPLASH: 
                render_boot_splash(renderer, &splash, &graphics); 
                break;
            case STATE_TITLE_SCREEN: 
                render_title_screen(renderer, &title, &graphics); 
                break;
            case STATE_ONLINE_SETUP: 
                render_online_setup(renderer, &online_setup, &graphics); 
                break;
            case STATE_MENU: 
                render_menu(renderer, &menu, &graphics); 
                break;
            case STATE_CHARACTER_SELECT: 
                render_character_select(renderer, &charsel, &graphics); 
                break;
            case STATE_VS_TRANSITION:
                // Offline mode - use local frame counter
                render_simple_vs_transition(renderer, &graphics, frame_counter);
                break;
            case STATE_GAMEPLAY: 
                render_gameplay(renderer, &gameplay, &graphics); 
                break;
            case STATE_EXIT_GAME: 
                running = 0; 
                break;
        }
        
        SDL_RenderPresent(renderer);
        
        // Simple frame rate limiting to ~60fps
        auto frame_end = gclock::now();
        auto frame_duration = frame_end - frame_start;
        if (frame_duration < target_frame_duration) {
            auto sleep_time = target_frame_duration - frame_duration;
            std::this_thread::sleep_for(sleep_time);
        }
    }
    
    // Online mode with proper timing (like OnlineSession.cpp)
    online_mode:
    if (sess) {
        printf(" ONLINE MODE ACTIVE - Using accumulator timing\n");
        
        // Verify initial state consistency
        uint32_t initial_checksum = fletcher32((uint16_t*)&rollback_state, sizeof(RollbackGameState));
        printf(" Initial online state checksum: %u\n", initial_checksum);
        
        // Timing variables (from OnlineSession.cpp)
        auto curr_time = gclock::now();
        auto prev_time(gclock::now());
        float delta_time = 0.0f;
        float accumulator = 0.0f;
        float frame_time = 0.0f;
        float frames_ahead = 0.0f;
        
        while (running && !g_game_should_quit) {
            curr_time = gclock::now();
            
            frames_ahead = gekko_frames_ahead(sess);
            frame_time = GetFrameTime(frames_ahead);
            
            delta_time = std::chrono::duration<float>(curr_time - prev_time).count();
            prev_time = curr_time;
            
            accumulator += delta_time;
            
            // Network polling (from OnlineSession.cpp)
            gekko_network_poll(sess);
            
            // SDL event polling
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) {
                    running = 0;
                    g_game_should_quit = 1;
                }
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                    running = 0;
                    g_game_should_quit = 1;
                }
            }
            
            // Frame processing loop
            bool should_render = false;  // Only render after all rollback processing is complete
            
            while (accumulator >= frame_time) {
                // Check for session events first (connection status, disconnects, etc.)
                int session_event_count = 0;
                GekkoSessionEvent **session_events = gekko_session_events(sess, &session_event_count);
                for (int i = 0; i < session_event_count; i++) {
                    GekkoSessionEvent* event = session_events[i];
                    
                    // Only log important events, skip spam
                    switch (event->type) {
                        case PlayerConnected:
                            printf(" PLAYER CONNECTED! Handle: %d\n", event->data.connected.handle);
                            online_setup.connection_established = true;
                            break;
                        case SessionStarted:
                            printf(" SESSION STARTED - Ready for gameplay!\n");
                            break;
                        case DesyncDetected:
                            printf("DESYNC! Frame:%d, Handle:%d, Local:%u, Remote:%u\n", 
                                   event->data.desynced.frame, event->data.desynced.remote_handle, 
                                   event->data.desynced.local_checksum, event->data.desynced.remote_checksum);
                            break;
                        case PlayerDisconnected:
                            printf(" PLAYER DISCONNECTED: Handle %d\n", event->data.disconnected.handle);
                            printf(" Returning to offline mode\n");
                            state = STATE_TITLE_SCREEN;
                            title.selected = 1;  // Stay on Online option
                            title.done = false;
                            title.just_entered = true;
                            title.prev_input = 0;
                            goto offline_continue;
                        default:
                            // Skip PlayerSyncing and other spam events
                            break;
                    }
                }
                
                // Get network stats (like OnlineSession.cpp)
                GekkoNetworkStats stats = {0};
                gekko_network_stats(sess, local_handle == 0  1 : 0, &stats);
                
                // Print clean stats occasionally (only if connected)
                static int stat_counter = 0;
                if (online_setup.connection_established && stat_counter % 180 == 0) {  // Every 3 seconds
                    // Clean up weird overflow values from GekkoNet
                    float clean_jitter = (stats.jitter < -1000000 || stats.jitter > 1000000)  0.0f : stats.jitter;
                    float clean_fa = (frames_ahead < -1000 || frames_ahead > 1000)  0.0f : frames_ahead;
                    float clean_avg_ping = (stats.avg_ping < 0 || stats.avg_ping > 10000)  0.0f : stats.avg_ping;
                    
                    printf(" ping: %dms | avg: %.1fms | jitter: %.1fms | frames_ahead: %.1f\n", 
                           stats.last_ping, clean_avg_ping, clean_jitter, clean_fa);
                }
                stat_counter++;
                
                // Add local input to session
                GInput local_input = (local_handle == 0)  get_p1_input() : get_p2_input();
                gekko_add_local_input(sess, local_handle, &local_input);
                
                // Process game events (save/load/advance) 
                process_game_events(sess, &state, &splash, &title, &online_setup, &menu, 
                                   &charsel, &gameplay, &rollback_state, &graphics);
                
                // Check if we should exit online mode
                if (state == STATE_TITLE_SCREEN || state == STATE_EXIT_GAME) {
                    printf(" Exiting online mode, returning to offline\n");
                    gekko_destroy(sess);
                    sess = nullptr;
                    goto offline_continue;
                }
                
                accumulator -= frame_time;
                should_render = true;  // Mark that we should render after processing this frame
            }
            
            // ONLY render if we processed at least one frame (prevents rollback flicker)
            if (should_render) {
                // Render current state
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);
                
                switch (state) {
                    case STATE_BOOT_SPLASH: 
                        render_boot_splash(renderer, &splash, &graphics); 
                        break;
                    case STATE_TITLE_SCREEN: 
                        render_title_screen(renderer, &title, &graphics); 
                        break;
                    case STATE_ONLINE_SETUP: 
                        render_online_setup(renderer, &online_setup, &graphics); 
                        break;
                    case STATE_MENU: 
                        render_menu(renderer, &menu, &graphics); 
                        break;
                    case STATE_CHARACTER_SELECT: 
                        render_character_select(renderer, &charsel, &graphics); 
                        break;
                    case STATE_VS_TRANSITION:
                        // Online mode - use GekkoNet frame counter
                        render_simple_vs_transition(renderer, &graphics, g_current_game_frame);
                        break;
                    case STATE_GAMEPLAY: 
                        render_gameplay(renderer, &gameplay, &graphics); 
                        break;
                    case STATE_EXIT_GAME: 
                        running = 0; 
                        break;
                }
                
                SDL_RenderPresent(renderer);
            }
        }
    }
    
    offline_continue:
    // Continue offline mode if exiting online
    if (running && !g_game_should_quit && !sess) {
        printf(" Continuing in offline mode\n");
        // Continue the offline loop...
    }
    
    // Cleanup
    if (sess) {
        gekko_destroy(sess);
    }
    character_select_cleanup(&charsel);
    menu_cleanup(&menu);
    title_screen_cleanup(&title);
    online_setup_cleanup(&online_setup);
    graphics_cleanup(&graphics);
    cleanup_global_vs_portraits();  // Clean up global portrait system
    
    printf(" GAME ENDED\n");
    return 1;
}
