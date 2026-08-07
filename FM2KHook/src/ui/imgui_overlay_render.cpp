// imgui_overlay_render.cpp -- ImGui DEBUG PANEL (RenderDebugOverlay).
// Split from imgui_overlay.cpp; THIS is the Slint-replaceable UI. Reads
// shared overlay state via imgui_overlay_internal.h; called once per frame
// from Hook_EndScene (core). Engine-agnostic.
#include "imgui_overlay.h"
#include "imgui_overlay_internal.h"
#include "fc_hud.h"
#include "netplay.h"          // Netplay_IsConnected/IsActive/GetInput/GetCSSInput
#include "globals.h"          // g_frame_counter, g_player_index
#include "savestate.h"        // SaveState_* roundtrip test
#include "../hooks/per_game_patches.h"
#include <imgui.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Memory addresses for debug display
namespace DebugAddrs {
    // CSS cursor positions
    constexpr uintptr_t CSS_P1_CURSOR_X = 0x470020;  // stage_positions[0]
    constexpr uintptr_t CSS_P2_CURSOR_X = 0x470024;  // stage_positions[1]

    // Player positions in battle (object pool)
    constexpr uintptr_t OBJECT_POOL = 0x4701E0;
    constexpr size_t OBJECT_SIZE = 382;
    constexpr size_t OBJ_X_OFFSET = 8;
    constexpr size_t OBJ_Y_OFFSET = 12;

    // Input tracking
    constexpr uintptr_t INPUT_BUFFER_INDEX = 0x447EE0;

    // Timer
    constexpr uintptr_t ROUND_TIMER = 0x470068;
}

// Read maintas/boxing/aspect_ratio from cnc-ddraw's ini once. They
// don't change without a launcher restart (the user has to relaunch

void RenderDebugOverlay() {
    if (!g_overlay_visible) return;

    // Clamp the debug window to the cnc-ddraw game rect so it floats
    // strictly over the live game pixels -- never over the black
    // letterbox/pillarbox margins. The rect comes from
    // pDevice->GetViewport() captured in Hook_EndScene right before
    // this call. Default to a 400x500 floating window if we haven't
    // captured a valid viewport yet (first frame after init).
    if (g_game_viewport.Width > 0 && g_game_viewport.Height > 0) {
        // Pin the window position + max bounds to the game rect.
        // ImGuiCond_Always so the window can't be dragged outside
        // (works on every frame, overrides user moves). Default size
        // takes ~70% of the rect so there's still some game visible.
        ImGui::SetNextWindowPos(
            ImVec2((float)g_game_viewport.X + 8.0f,
                   (float)g_game_viewport.Y + 8.0f),
            ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(
            ImVec2((float)g_game_viewport.Width  * 0.7f,
                   (float)g_game_viewport.Height * 0.85f),
            ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(160.0f, 100.0f),
            ImVec2((float)g_game_viewport.Width,
                   (float)g_game_viewport.Height));
    } else {
        ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);
    }
    ImGui::Begin("FM2K Debug [F9]", &g_overlay_visible);

    // Game-rect debug toggle -- at the top so it's easy to find while
    // testing the rect detection across window sizes / fullscreen.
    ImGui::Checkbox("Show game-rect outline (debug)", &g_show_game_rect_debug);
    if (g_game_viewport.Width > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f),
            "rect: %lu,%lu  %lux%lu",
            (unsigned long)g_game_viewport.X,
            (unsigned long)g_game_viewport.Y,
            (unsigned long)g_game_viewport.Width,
            (unsigned long)g_game_viewport.Height);
    }
    ImGui::Separator();

    // Always show critical sync info at top
    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    uint32_t rng = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
    uint32_t buf_idx = *(uint32_t*)DebugAddrs::INPUT_BUFFER_INDEX;

    ImGui::TextColored(ImVec4(1,1,0,1), "=== SYNC STATUS ===");
    ImGui::Text("Player: P%d | Mode: %u | Frame: %u",
        g_player_index + 1, game_mode, g_frame_counter);
    ImGui::Text("RNG: 0x%08X | BufIdx: %u", rng, buf_idx);
    ImGui::Text("Netplay: %s | Active: %s",
        Netplay_IsConnected() ? "CONN" : "DISC",
        Netplay_IsActive() ? "YES" : "NO");
    ImGui::Separator();

    if (ImGui::BeginTabBar("DebugTabs")) {
        // HUD Tab - tunable runtime style for the always-on overlay.
        // First in the tab bar because it's the most likely thing the
        // user wants to touch when iterating on the in-game look.
        if (ImGui::BeginTabItem("HUD")) {
            fc_hud::StyleControls& s = fc_hud::Style();
            ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                "In-game HUD style -- applies live each frame.");
            ImGui::Spacing();

            ImGui::SliderFloat("Scale", &s.scale, 0.3f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Multiplies the entire HUD (top bar height, fonts, "
                    "chat box). Default 1.0 = scaled to game-rect height.");
            }
            ImGui::SliderFloat("Top-bar opacity", &s.bar_opacity,
                               0.0f, 1.0f, "%.2f");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Visibility");
            ImGui::Checkbox("Show top bar (names, ping, fps)", &s.show_top_bar);
            ImGui::Checkbox("Show chat history",               &s.show_chat);
            ImGui::Checkbox("Show system message",             &s.show_system_message);

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Button("Reset to defaults")) {
                s = fc_hud::StyleControls{};
            }

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                "Tab opens chat input (game window must be focused).");

            ImGui::EndTabItem();
        }

        // CSS Tab - for debugging character select desync
        if (ImGui::BeginTabItem("CSS")) {
            uint32_t p1_cursor = *(uint32_t*)DebugAddrs::CSS_P1_CURSOR_X;
            uint32_t p2_cursor = *(uint32_t*)DebugAddrs::CSS_P2_CURSOR_X;
            uint16_t timer = *(uint16_t*)DebugAddrs::ROUND_TIMER;

            ImGui::TextColored(ImVec4(0,1,1,1), "Character Select State");
            ImGui::Text("P1 Cursor Position: %u", p1_cursor);
            ImGui::Text("P2 Cursor Position: %u", p2_cursor);
            ImGui::Text("Timer: %u", timer);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(1,0.5,0,1), "CSS Input (from control channel)");
            ImGui::Text("Local CSS Input:  0x%03X", Netplay_GetCSSInput(g_player_index));
            ImGui::Text("Remote CSS Input: 0x%03X", Netplay_GetCSSInput(1 - g_player_index));

            ImGui::EndTabItem();
        }

        // Battle Tab - for debugging battle desync
        if (ImGui::BeginTabItem("Battle")) {
            // Read player positions from object pool
            uint8_t* obj_pool = (uint8_t*)DebugAddrs::OBJECT_POOL;

            int32_t p1_x = *(int32_t*)(obj_pool + 0 * DebugAddrs::OBJECT_SIZE + DebugAddrs::OBJ_X_OFFSET);
            int32_t p1_y = *(int32_t*)(obj_pool + 0 * DebugAddrs::OBJECT_SIZE + DebugAddrs::OBJ_Y_OFFSET);
            int32_t p2_x = *(int32_t*)(obj_pool + 1 * DebugAddrs::OBJECT_SIZE + DebugAddrs::OBJ_X_OFFSET);
            int32_t p2_y = *(int32_t*)(obj_pool + 1 * DebugAddrs::OBJECT_SIZE + DebugAddrs::OBJ_Y_OFFSET);

            // HP source differs by engine. FM2K has globals at fixed addresses;
            // FM95 stores HP per-object inside the pool slot (offset 72 = pos
            // field reused as HP for character objects). Pull from whichever
            // applies -- globals.h's FM95 ifdef sets ADDR_P*_HP to 0 sentinel
            // so a direct deref would crash.
            uint32_t p1_hp_val = 0, p2_hp_val = 0;
            if constexpr (FM2K::kIsFM2K) {
                p1_hp_val = *(uint32_t*)FM2K::ADDR_P1_HP;
                p2_hp_val = *(uint32_t*)FM2K::ADDR_P2_HP;
            } else {
                // FM95: read HP from the per-player main object's +72 field.
                // pool[0] / pool[1] hold the player main objects.
                p1_hp_val = *(uint32_t*)(obj_pool + 0 * DebugAddrs::OBJECT_SIZE + 72);
                p2_hp_val = *(uint32_t*)(obj_pool + 1 * DebugAddrs::OBJECT_SIZE + 72);
            }

            ImGui::TextColored(ImVec4(0,1,0,1), "Player Positions (CRITICAL FOR DESYNC)");
            ImGui::Columns(2);

            ImGui::Text("P1 Position:");
            ImGui::Text("  X: %d", p1_x);
            ImGui::Text("  Y: %d", p1_y);
            ImGui::Text("  HP: %u", p1_hp_val);

            ImGui::NextColumn();

            ImGui::Text("P2 Position:");
            ImGui::Text("  X: %d", p2_x);
            ImGui::Text("  Y: %d", p2_y);
            ImGui::Text("  HP: %u", p2_hp_val);

            ImGui::Columns(1);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(1,0.5,0,1), "Synced Inputs (from GekkoNet)");
            ImGui::Text("P1 Input: 0x%03X", Netplay_GetInput(0));
            ImGui::Text("P2 Input: 0x%03X", Netplay_GetInput(1));

            ImGui::EndTabItem();
        }

        // Input Tab - raw input display
        if (ImGui::BeginTabItem("Input")) {
            uint16_t p1_input = *(uint16_t*)FM2K::ADDR_P1_INPUT;
            uint16_t p2_input = *(uint16_t*)FM2K::ADDR_P2_INPUT;

            ImGui::TextColored(ImVec4(1,1,0,1), "Game Memory Inputs");
            ImGui::Columns(2);

            ImGui::Text("P1: 0x%03X", p1_input);
            ImGui::Text("  L:%d R:%d U:%d D:%d",
                (p1_input>>0)&1, (p1_input>>1)&1, (p1_input>>2)&1, (p1_input>>3)&1);
            ImGui::Text("  A:%d B:%d C:%d D:%d",
                (p1_input>>4)&1, (p1_input>>5)&1, (p1_input>>6)&1, (p1_input>>7)&1);

            ImGui::NextColumn();

            ImGui::Text("P2: 0x%03X", p2_input);
            ImGui::Text("  L:%d R:%d U:%d D:%d",
                (p2_input>>0)&1, (p2_input>>1)&1, (p2_input>>2)&1, (p2_input>>3)&1);
            ImGui::Text("  A:%d B:%d C:%d D:%d",
                (p2_input>>4)&1, (p2_input>>5)&1, (p2_input>>6)&1, (p2_input>>7)&1);

            ImGui::Columns(1);
            ImGui::EndTabItem();
        }

        // Memory Tab - raw memory inspection
        if (ImGui::BeginTabItem("Memory")) {
            ImGui::TextColored(ImVec4(1,0,1,1), "Critical Addresses");

            ImGui::Text("GAME_MODE:    0x%08X = %u", FM2K::ADDR_GAME_MODE, *(uint32_t*)FM2K::ADDR_GAME_MODE);
            ImGui::Text("RANDOM_SEED:  0x%08X = 0x%08X", FM2K::ADDR_RANDOM_SEED, *(uint32_t*)FM2K::ADDR_RANDOM_SEED);
            ImGui::Text("INPUT_BUF_IDX:0x%08X = %u", DebugAddrs::INPUT_BUFFER_INDEX, *(uint32_t*)DebugAddrs::INPUT_BUFFER_INDEX);
            ImGui::Text("P1_INPUT:     0x%08X = 0x%03X", FM2K::ADDR_P1_INPUT, *(uint16_t*)FM2K::ADDR_P1_INPUT);
            ImGui::Text("P2_INPUT:     0x%08X = 0x%03X", FM2K::ADDR_P2_INPUT, *(uint16_t*)FM2K::ADDR_P2_INPUT);
            ImGui::Text("ROUND_TIMER:  0x%08X = %u", DebugAddrs::ROUND_TIMER, *(uint16_t*)DebugAddrs::ROUND_TIMER);

            ImGui::EndTabItem();
        }

        // Test Tab - savestate verification
        if (ImGui::BeginTabItem("Test")) {
            ImGui::TextColored(ImVec4(1,1,0,1), "Rollback Verification [F10]");
            ImGui::Separator();

            // Current state snapshot
            StateSnapshot current = SaveState_CaptureSnapshot();
            ImGui::Text("Current State:");
            ImGui::Text("  RNG: 0x%08X", current.rng_seed);
            ImGui::Text("  P1: (%d, %d) HP=%u", current.p1_x, current.p1_y, current.p1_hp);
            ImGui::Text("  P2: (%d, %d) HP=%u", current.p2_x, current.p2_y, current.p2_hp);
            ImGui::Text("  Full Checksum: 0x%08X", current.checksum);
            ImGui::Separator();

            // Run test button
            if (ImGui::Button("Run Roundtrip Test [F10]")) {
                g_last_snapshot_before = SaveState_CaptureSnapshot();
                g_last_test_passed = SaveState_TestRoundtrip();
                g_last_snapshot_after = SaveState_CaptureSnapshot();
                SaveState_CompareSnapshots(g_last_snapshot_before, g_last_snapshot_after,
                    g_last_test_diff, sizeof(g_last_test_diff));
                g_last_test_ran = true;
            }

            ImGui::SameLine();
            if (g_last_test_ran) {
                if (g_last_test_passed) {
                    ImGui::TextColored(ImVec4(0,1,0,1), "PASSED");
                } else {
                    ImGui::TextColored(ImVec4(1,0,0,1), "FAILED");
                }
            }

            // Show last test results
            if (g_last_test_ran) {
                ImGui::Separator();
                ImGui::Text("Last Test Results:");
                ImGui::Text("Before: RNG=0x%08X P1=(%d,%d) chk=0x%08X",
                    g_last_snapshot_before.rng_seed,
                    g_last_snapshot_before.p1_x, g_last_snapshot_before.p1_y,
                    g_last_snapshot_before.checksum);
                ImGui::Text("After:  RNG=0x%08X P1=(%d,%d) chk=0x%08X",
                    g_last_snapshot_after.rng_seed,
                    g_last_snapshot_after.p1_x, g_last_snapshot_after.p1_y,
                    g_last_snapshot_after.checksum);

                if (!g_last_test_passed && g_last_test_diff[0]) {
                    ImGui::TextColored(ImVec4(1,0.5,0,1), "Differences:");
                    ImGui::TextWrapped("%s", g_last_test_diff);
                }
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}
