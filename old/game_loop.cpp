#include "game_loop.h"
#include "../input/input_manager.h"
#include "../states/vs_transition.h"
#include "../states/online_setup.h"
#include <stdio.h>
#include <chrono>
#include <thread>

// Timing (like OnlineSession.cpp)
using micro = std::chrono::microseconds;
using gduration = std::chrono::duration<unsigned int, std::ratio<1, 60>>;
using gtime_point = std::chrono::time_point<std::chrono::steady_clock>;
using slow_frame = std::chrono::duration<unsigned int, std::ratio<1, 59>>;
using normal_frame = std::chrono::duration<unsigned int, std::ratio<1, 60>>;
using fast_frame = std::chrono::duration<unsigned int, std::ratio<1, 61>>;
using gclock = std::chrono::steady_clock;

float GetFrameTime(float frames_ahead) {
    if (frames_ahead >= 0.75f) {
        return std::chrono::duration<float>(slow_frame(1)).count();
    }
    else {
        return std::chrono::duration<float>(normal_frame(1)).count();
    }
}

// Offline-only game logic (uses same logic as online)
void process_offline_logic(GameState* state, 
                          BootSplashContext* splash, TitleScreenContext* title,
                          OnlineSetupContext* online_setup, MenuContext* menu,
                          CharacterSelectContext* charsel, GameplayContext* gameplay,
                          GraphicsContext* graphics, unsigned char p1_input, unsigned char p2_input) {
    
    unsigned char in0 = p1_input;  // P1 input
    unsigned char in1 = p2_input;  // P2 input
    
    // For single-player states (title, menu), combine inputs
    unsigned char combined = p1_input | p2_input;
    
    // Use SAME game logic as online mode
    switch (*state) {
        case STATE_BOOT_SPLASH:
            update_boot_splash(splash, in0, in1);
            if (splash->done) {
                *state = STATE_TITLE_SCREEN;
                title->selected = 0;
                title->done = false;
                title->inactivity_timer = 0;
                title->just_entered = true;
                title->prev_input = 0;
            }
            break;
            
        case STATE_TITLE_SCREEN:
            update_title_screen(title, combined, 0);  // Single-player navigation
            if (title->done) {
                if (title->selected == 0) {  // START (offline)
                    *state = STATE_MENU;
                    menu->selection = 0;
                    menu->inactivity_timer = 0;
                    menu->transition_requested = false;
                    menu->just_entered = true;
                    menu->prev_input = 0;
                } else if (title->selected == 1) {  // ONLINE
                    *state = STATE_ONLINE_SETUP;
                    online_setup_init(online_setup);
                } else {  // EXIT
                    *state = STATE_EXIT_GAME;
                }
            }
            break;
            
        case STATE_ONLINE_SETUP:
            update_online_setup(online_setup, combined);  // Single-player navigation
            if (combined & INPUT_B && online_setup->selection == 2) {
                // Back to title
                *state = STATE_TITLE_SCREEN;
                title->selected = 1;  // Stay on Online option
                title->done = false;
                title->just_entered = true;
                title->prev_input = 0;
            }
            break;
            
        case STATE_MENU:
            update_menu(menu, in0, in1, graphics);  // Pass both P1 and P2 inputs
            if (menu->transition_requested) {
                menu->transition_requested = false;
                
                if (menu->state == MENU_MAIN) {
                    switch (menu->selection) {
                        case 0:  // STORY
                        case 1:  // VERSUS
                        case 2:  // SURVIVAL
                        case 3:  // TIME ATTACK
                        case 5:  // PRACTICE
                        case 8:  // WATCH
                            // Regular character select (not team battle)
                            *state = STATE_CHARACTER_SELECT;
                            character_select_cleanup(charsel);  // Clean up first
                            character_select_init(charsel, "assets", false);  // false = not team battle
                            charsel->selected_p1 = 0;
                            charsel->selected_p2 = 1;
                            charsel->done = false;
                            charsel->just_entered = true;
                            charsel->prev_input_p1 = 0;
                            charsel->prev_input_p2 = 0;
                            charsel->inactivity_timer = 0;
                            break;
                        case 4:  // TEAM BATTLE 
                            menu->state = MENU_TEAM_BATTLE;
                            menu->selection = 0;
                            menu->just_entered = true;
                            break;
                        case 6:  // OPTIONS
                            menu->state = MENU_OPTIONS;
                            menu->selection = 0;
                            menu->just_entered = true;
                            break;
                        case 7:  // RANKINGS
                            menu->state = MENU_RANKINGS;
                            menu->selection = 0;
                            menu->just_entered = true;
                            break;
                        case 10: // EXIT
                            *state = STATE_EXIT_GAME;
                            break;
                    }
                } else if (menu->state == MENU_TEAM_BATTLE) {
                    // Team battle sub-menu selections
                    switch (menu->selection) {
                        case 0:  // STORY (team battle)
                        case 1:  // VERSUS (team battle)
                            // Team battle character select
                            *state = STATE_CHARACTER_SELECT;
                            character_select_cleanup(charsel);  // Clean up first
                            character_select_init(charsel, "assets", true);  // true = team battle mode
                            charsel->selected_p1 = 0;
                            charsel->selected_p2 = 1;
                            charsel->done = false;
                            charsel->just_entered = true;
                            charsel->prev_input_p1 = 0;
                            charsel->prev_input_p2 = 0;
                            charsel->inactivity_timer = 0;
                            charsel->team_battle_mode = true;
                            break;
                    }
                }
                // Note: OPTIONS and RANKINGS states can be handled by the menu system internally
            }
            
            // Back button - return to main menu from sub-menus
            if (menu->state != MENU_MAIN && (combined & INPUT_B)) {
                menu->state = MENU_MAIN;
                menu->selection = 0;
                menu->just_entered = true;
                menu->prev_input = 0;
            }
            break;
            
        case STATE_CHARACTER_SELECT:
            update_character_select(charsel, in0, in1);
            if (charsel->done) {
                if (charsel->selected_character_p1 == -1) {
                    *state = STATE_MENU;
                    menu->state = MENU_MAIN;
                    menu->selection = 0;
                    menu->just_entered = true;
                    menu->prev_input = 0;
                } else {
                    // Characters selected - start VS transition!
                    *state = STATE_VS_TRANSITION;
                    
                    // Get character data
                    int p1_char_id, p2_char_id;
                    if (charsel->team_battle_mode) {
                        p1_char_id = (charsel->p1_selection_count > 0) ? charsel->p1_team[0] : -1;
                        p2_char_id = (charsel->p2_selection_count > 0) ? charsel->p2_team[0] : -1;
                    } else {
                        p1_char_id = charsel->current_p1_pattern;
                        p2_char_id = charsel->current_p2_pattern;
                    }
                    
                    // Start simple VS transition (no rollback state needed)
                    start_vs_transition(p1_char_id, p2_char_id, charsel->team_battle_mode,
                                       charsel->p1_team, charsel->p2_team,
                                       charsel->p1_selection_count, charsel->p2_selection_count, 0);
                    
                    printf("? CHARACTER SELECT: Started VS transition P1=%d, P2=%d, Team=%s\n", 
                           p1_char_id, p2_char_id, charsel->team_battle_mode ? "YES" : "NO");
                }
                // Reset character select
                charsel->selected_p1 = 0;
                charsel->selected_p2 = 1;
                charsel->done = false;
                charsel->just_entered = false;
                charsel->prev_input_p1 = 0;
                charsel->prev_input_p2 = 0;
                charsel->inactivity_timer = 0;
                charsel->selected_character_p1 = -1;
                charsel->selected_character_p2 = -1;
                charsel->p1_confirmed = false;
                charsel->p2_confirmed = false;
            }
            break;
            
        case STATE_VS_TRANSITION:
            // Check if VS transition is done (no rollback state needed)
            if (is_vs_transition_done(0)) { // Frame 0 is placeholder - will be calculated internally
                *state = STATE_GAMEPLAY;
                reset_vs_transition();
                
                // Initialize gameplay context for battle
                gameplay_init(gameplay);
                printf("? VS TRANSITION: Complete, moving to gameplay! Gameplay initialized.\n");
            }
            break;
            
        case STATE_GAMEPLAY:
            update_gameplay(gameplay, in0, in1);
            break;
            
        case STATE_EXIT_GAME:
            extern volatile int g_game_should_quit;
            g_game_should_quit = 1;
            break;
    }
}

// Process game events for online session
void process_game_events(GekkoSession* sess, GameState* state, 
                        BootSplashContext* splash, TitleScreenContext* title,
                        OnlineSetupContext* online_setup, MenuContext* menu,
                        CharacterSelectContext* charsel, GameplayContext* gameplay,
                        RollbackGameState* rollback_state, GraphicsContext* graphics) {
    
    // Check for session events (desyncs, disconnections)
    int session_event_count = 0;
    GekkoSessionEvent **session_events = gekko_session_events(sess, &session_event_count);
    for (int i = 0; i < session_event_count; i++) {
        GekkoSessionEvent* event = session_events[i];
        if (event->type == DesyncDetected) {
            printf("? DESYNC! Frame:%d, Handle:%d, Local:%u, Remote:%u\n", 
                   event->data.desynced.frame, event->data.desynced.remote_handle, 
                   event->data.desynced.local_checksum, event->data.desynced.remote_checksum);
        }
        if (event->type == PlayerDisconnected) {
            printf("? PLAYER DISCONNECTED: Handle %d\n", event->data.disconnected.handle);
            extern volatile int g_game_should_quit;
            g_game_should_quit = 1;
        }
    }
    
    // Process game events
    int count = 0;
    GekkoGameEvent **updates = gekko_update_session(sess, &count);
    
    for (int i = 0; i < count; i++) {
        GekkoGameEvent* ev = updates[i];
        
        switch (ev->type) {
            case SaveEvent:
                save_game_state(rollback_state, *state, splash, title, online_setup, menu, charsel, gameplay);
                gekko_save_state(rollback_state, ev);
                break;
                
            case LoadEvent:
                gekko_load_state(rollback_state, ev);
                load_game_state(rollback_state, state, splash, title, online_setup, menu, charsel, gameplay);
                break;
                
            case AdvanceEvent: {
                // Update global frame counter for rendering
                extern int g_current_game_frame;
                g_current_game_frame = ev->data.adv.frame;
                
                // Extract inputs from both players
                unsigned char in0 = ev->data.adv.inputs[0];  // P1's input  
                unsigned char in1 = ev->data.adv.inputs[1];  // P2's input
                
                // Game state processing
                switch (*state) {
                    case STATE_BOOT_SPLASH:
                        update_boot_splash(splash, in0, in1);
                        if (splash->done) {
                            *state = STATE_TITLE_SCREEN;
                            title->selected = 0;
                            title->done = false;
                            title->inactivity_timer = 0;
                            title->just_entered = true;
                            title->prev_input = 0;
                        }
                        break;
                        
                    case STATE_TITLE_SCREEN:
                        update_title_screen(title, in0, in1);
                        if (title->done) {
                            if (title->selected == 0) {  // START (offline)
                                *state = STATE_MENU;
                                menu->selection = 0;
                                menu->inactivity_timer = 0;
                                menu->transition_requested = false;
                                menu->just_entered = true;
                                menu->prev_input = 0;
                            } else if (title->selected == 1) {  // ONLINE
                                *state = STATE_ONLINE_SETUP;
                                online_setup_init(online_setup);
                            } else {  // EXIT
                                *state = STATE_EXIT_GAME;
                            }
                        }
                        break;
                        
                    case STATE_ONLINE_SETUP:
                        update_online_setup(online_setup, in0 | in1);  // Either player can navigate
                        if ((in0 | in1) & INPUT_B && online_setup->selection == 2) {
                            // Back to title
                            *state = STATE_TITLE_SCREEN;
                            title->selected = 1;  // Stay on Online option
                            title->done = false;
                            title->just_entered = true;
                            title->prev_input = 0;
                        }
                        // TODO: Handle actual connection initiation
                        break;
                        
                    case STATE_MENU:
                        update_menu(menu, in0, in1, graphics);
                        if (menu->transition_requested) {
                            menu->transition_requested = false;
                            
                            if (menu->state == MENU_MAIN) {
                                switch (menu->selection) {
                                    case 0:  // STORY
                                    case 1:  // VERSUS
                                    case 2:  // SURVIVAL
                                    case 3:  // TIME ATTACK
                                    case 5:  // PRACTICE
                                    case 8:  // WATCH
                                        // Regular character select (not team battle)
                                        *state = STATE_CHARACTER_SELECT;
                                        character_select_cleanup(charsel);  // Clean up first
                                        character_select_init(charsel, "assets", false);  // false = not team battle
                                        charsel->selected_p1 = 0;
                                        charsel->selected_p2 = 1;
                                        charsel->done = false;
                                        charsel->just_entered = true;
                                        charsel->prev_input_p1 = 0;
                                        charsel->prev_input_p2 = 0;
                                        charsel->inactivity_timer = 0;
                                        break;
                                    case 4:  // TEAM BATTLE 
                                        menu->state = MENU_TEAM_BATTLE;
                                        menu->selection = 0;
                                        menu->just_entered = true;
                                        break;
                                    case 6:  // OPTIONS
                                        menu->state = MENU_OPTIONS;
                                        menu->selection = 0;
                                        menu->just_entered = true;
                                        break;
                                    case 7:  // RANKINGS
                                        menu->state = MENU_RANKINGS;
                                        menu->selection = 0;
                                        menu->just_entered = true;
                                        break;
                                    case 10: // EXIT
                                        *state = STATE_EXIT_GAME;
                                        break;
                                }
                            } else if (menu->state == MENU_TEAM_BATTLE) {
                                // Team battle sub-menu selections
                                switch (menu->selection) {
                                    case 0:  // STORY (team battle)
                                    case 1:  // VERSUS (team battle)
                                        // Team battle character select
                                        *state = STATE_CHARACTER_SELECT;
                                        character_select_cleanup(charsel);  // Clean up first
                                        character_select_init(charsel, "assets", true);  // true = team battle mode
                                        charsel->selected_p1 = 0;
                                        charsel->selected_p2 = 1;
                                        charsel->done = false;
                                        charsel->just_entered = true;
                                        charsel->prev_input_p1 = 0;
                                        charsel->prev_input_p2 = 0;
                                        charsel->inactivity_timer = 0;
                                        charsel->team_battle_mode = true;
                                        break;
                                }
                            }
                            // Note: OPTIONS and RANKINGS states can be handled by the menu system internally
                        }
                        
                        // Back button - return to main menu from sub-menus
                        if (menu->state != MENU_MAIN && ((in0 | in1) & INPUT_B)) {
                            menu->state = MENU_MAIN;
                            menu->selection = 0;
                            menu->just_entered = true;
                            menu->prev_input = 0;
                        }
                        break;
                        
                    case STATE_CHARACTER_SELECT:
                        update_character_select(charsel, in0, in1);
                        if (charsel->done) {
                            if (charsel->selected_character_p1 == -1) {
                                *state = STATE_MENU;
                                menu->state = MENU_MAIN;
                                menu->selection = 0;
                                menu->just_entered = true;
                                menu->prev_input = 0;
                            } else {
                                // Characters selected - start VS transition!
                                *state = STATE_VS_TRANSITION;
                                
                                // Get character data
                                int p1_char_id, p2_char_id;
                                if (charsel->team_battle_mode) {
                                    p1_char_id = (charsel->p1_selection_count > 0) ? charsel->p1_team[0] : -1;
                                    p2_char_id = (charsel->p2_selection_count > 0) ? charsel->p2_team[0] : -1;
                                } else {
                                    p1_char_id = charsel->current_p1_pattern;
                                    p2_char_id = charsel->current_p2_pattern;
                                }
                                
                                // Start simple VS transition (using current frame)
                                start_vs_transition(p1_char_id, p2_char_id, charsel->team_battle_mode,
                                                   charsel->p1_team, charsel->p2_team,
                                                   charsel->p1_selection_count, charsel->p2_selection_count, 
                                                   ev->data.adv.frame);
                                
                                printf("? ONLINE CHARACTER SELECT: Started VS transition P1=%d, P2=%d, Team=%s at frame %d\n", 
                                       p1_char_id, p2_char_id, charsel->team_battle_mode ? "YES" : "NO", ev->data.adv.frame);
                            }
                            // Reset character select
                            charsel->selected_p1 = 0;
                            charsel->selected_p2 = 1;
                            charsel->done = false;
                            charsel->just_entered = false;
                            charsel->prev_input_p1 = 0;
                            charsel->prev_input_p2 = 0;
                            charsel->inactivity_timer = 0;
                            charsel->selected_character_p1 = -1;
                            charsel->selected_character_p2 = -1;
                            charsel->p1_confirmed = false;
                            charsel->p2_confirmed = false;
                        }
                        break;
                        
                    case STATE_VS_TRANSITION:
                        // VS transition doesn't need input processing, but we need to advance the frame
                        // so the transition timer works properly in online mode
                        
                        // Check if VS transition is done (purely deterministic)
                        if (is_vs_transition_done(ev->data.adv.frame)) {
                            *state = STATE_GAMEPLAY;
                            reset_vs_transition();
                            
                            // Initialize gameplay context for battle
                            gameplay_init(gameplay);
                            printf("? ONLINE VS TRANSITION: Complete, moving to gameplay at frame %d! Gameplay initialized.\n", ev->data.adv.frame);
                        }
                        break;
                        
                    case STATE_GAMEPLAY:
                        update_gameplay(gameplay, in0, in1);
                        break;
                        
                    case STATE_EXIT_GAME:
                        extern volatile int g_game_should_quit;
                        g_game_should_quit = 1;
                        break;
                }
                break;
            }
            default:
                printf("Unknown GekkoNet event type: %d\n", ev->type);
                break;
        }
    }
} 