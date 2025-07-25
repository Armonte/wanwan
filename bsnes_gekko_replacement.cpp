// BSNES-STYLE GEKKONET PROCESSING
// This is the exact equivalent of BSNES's netplayRun() function for FM2K
if (use_gekko && gekko_initialized && gekko_session) {
    
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
        // Online session: Each client sends only their player's input
        if (::player_index == 0) {
            uint16_t p1_input = (uint16_t)(live_p1_input & 0x7FF);
            gekko_add_local_input(gekko_session, local_player_handle, &p1_input);
        } else {
            uint16_t p2_input = (uint16_t)(live_p2_input & 0x7FF);
            gekko_add_local_input(gekko_session, local_player_handle, &p2_input);
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
        }
    }
    
    // STEP 4: Always process updates (SaveEvent, LoadEvent, AdvanceEvent)
    // This is the core of BSNES gekko_update_session processing
    gekko_network_poll(gekko_session);
    int update_count = 0;
    auto updates = gekko_update_session(gekko_session, &update_count);
    
    // Reset frame advance flag - will be set by AdvanceEvent if we should advance
    can_advance_frame = false;
    use_networked_inputs = false;
    
    for (int i = 0; i < update_count; i++) {
        auto update = updates[i];
        switch (update->type) {
            case SaveEvent:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: SaveEvent frame %d", update->data.save.frame);
                // Minimal state like BSNES - just 4 bytes
                *update->data.save.checksum = 0;
                *update->data.save.state_len = sizeof(int32_t);
                memcpy(update->data.save.state, &update->data.save.frame, sizeof(int32_t));
                break;
                
            case LoadEvent:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: LoadEvent frame %d", update->data.load.frame);
                // TODO: Implement rollback state loading
                break;
                
            case AdvanceEvent:
                // This is the key - only advance when GekkoNet says so (like BSNES emulator->run())
                can_advance_frame = true;
                use_networked_inputs = true;
                gekko_frame_control_enabled = true;
                
                // Copy networked inputs from GekkoNet (like BSNES memcpy)
                if (update->data.adv.inputs && update->data.adv.input_size >= sizeof(uint16_t) * 2) {
                    uint16_t* networked_inputs = (uint16_t*)update->data.adv.inputs;
                    p1_networked_input = networked_inputs[0];
                    p2_networked_input = networked_inputs[1];
                    
                    static uint32_t advance_counter = 0;
                    if (++advance_counter % 300 == 0) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: AdvanceEvent #%d - P1=0x%04X P2=0x%04X", 
                                   advance_counter, p1_networked_input, p2_networked_input);
                    }
                }
                break;
        }
    }
    
    // STEP 5: CRITICAL - Block frame processing if no AdvanceEvent (like BSNES)
    if (gekko_frame_control_enabled && !can_advance_frame) {
        // This is exactly like BSNES - if no AdvanceEvent, don't run the emulator
        static uint32_t block_counter = 0;
        if (++block_counter % 120 == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Blocking frame - waiting for AdvanceEvent (#%d)", block_counter);
        }
        return 0; // Don't process this frame
    }
}