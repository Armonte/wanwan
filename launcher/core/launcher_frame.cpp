// launcher_frame.cpp -- FM2KLauncher per-frame + small mutators, split out of
// FM2K_RollbackClient.cpp. Pure move of member functions:
//   - HandleEvent()           : SDL event -> ImGui + launcher routing
//   - Update()                : the IPC-critical slice (DLL events, STUN poll,
//                               spec-relay drain, game-termination check)
//   - Render()                : ImGui frame build + present (multi-viewport)
//   - SetSelectedGame/SetGamesRootPaths/SetState/UiWantsContinuousRedraw
#include "SDL3/SDL.h"
#include <SDL3_image/SDL_image.h>
#include "app_icon_data.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "MinHook.h"
#include "FM2K_GameInstance.h"
#include "FM2K_Integration.h"
#include "game_discovery.h"  // game scan/cache/sniff + async discovery (moved out of this file)
#include "FM2K_GameIni.h"
#include "FM2K_Utf8Path.h"
#include "FM2KHook/src/ui/shared_mem.h"
#include "FM2KHook/src/ui/input_binder.h"  // RefreshGamepads() on SDL hot-plug events
#include "FM2KHook/src/netplay/spec_relay_queue.h"  // hub-relay outbound drain (Phase 2c)
#define XXH_INLINE_ALL
#include "vendored/xxhash/xxhash.h"
#include "LocalSession.h"
#include "OnlineSession.h"
#include "FM2K_PortMapper.h"  // --upnp-test self-contained router validation

#include <chrono>
#include <string>
#include <cstring>
#include <cstdlib>  // std::getenv for FM2K_FULL_CRCS perf-run override
#include <optional>
#include <vector>
#include <iostream>
#include <thread>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <system_error>

void FM2KLauncher::HandleEvent(SDL_Event* event) {
    if (!event) return;

    // Let ImGui handle events first
    ImGui_ImplSDL3_ProcessEvent(event);

    // Handle window events - just log them, don't interfere
    if (event->type == SDL_EVENT_WINDOW_MINIMIZED) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SDL_EVENT_WINDOW_MINIMIZED: Window minimized normally");
    } else if (event->type == SDL_EVENT_WINDOW_RESTORED || 
               event->type == SDL_EVENT_WINDOW_SHOWN) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SDL_EVENT_WINDOW_RESTORED/SHOWN: Window restored");
    } else if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        if (event->window.windowID == SDL_GetWindowID(window_)) {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize.x = static_cast<float>(event->window.data1);
            io.DisplaySize.y = static_cast<float>(event->window.data2);
        }
    }

    // Handle discovery completion
    if (event->type == g_event_discovery_complete) {
        auto games_ptr = static_cast<std::vector<FM2K::FM2KGameInfo>*>(event->user.data1);
        if (games_ptr) {
            discovered_games_ = std::move(*games_ptr);
            delete games_ptr;
        }

        discovery_in_progress_ = false;
        if (discovery_thread_) {
            SDL_WaitThread(discovery_thread_, nullptr);
            discovery_thread_ = nullptr;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Async discovery complete: %d games found", (int)discovered_games_.size());
        if (ui_) {
            ui_->SetGames(discovered_games_);
            ui_->SetScanning(false);
        }
        Utils::SaveGameCache(discovered_games_);
    }

    // Only process our events if ImGui isn't capturing input
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureMouse && !io.WantCaptureKeyboard) {
        if (event->type == SDL_EVENT_KEY_DOWN) {
            if (event->key.scancode == SDL_SCANCODE_ESCAPE) {
                running_ = false;
            }
        }
    }
}

// ----- spec hub-relay pid resolution (see FM2K_Launcher_decl.h) -----
//
// Deliberately NOT one shared helper with a direction flag: the two
// precedences are different, and writing them out makes the asymmetry
// legible at both call sites.
uint32_t FM2KLauncher::SpecRelayOutboundPid() const {
    // Host-produced ring. Player slots first; spectator last-resort so a
    // relaying/daisy-chain viewer (which also creates an outbound ring at
    // SpectatorNode_Init) is still serviced when no player game exists.
    if (game_instance_ && game_instance_->IsRunning()) {
        return (uint32_t)game_instance_->GetProcessId();
    }
    if (client1_instance_ && client1_instance_->IsRunning()) {
        // Dev-mode dual-clients fallback -- local-test spectator path.
        return (uint32_t)client1_instance_->GetProcessId();
    }
    if (spectator_instance_ && spectator_instance_->IsRunning()) {
        return (uint32_t)spectator_instance_->GetProcessId();
    }
    return 0;
}

uint32_t FM2KLauncher::SpecRelayInboundPid() const {
    // Viewer-consumed ring. spectator_instance_ is where --spectate /
    // the lobby Spectate button / LaunchLocalSpectator put the game, so
    // it wins; the player slots stay as a fallback purely so a launcher
    // that somehow has only a player game still behaves as it did.
    if (spectator_instance_ && spectator_instance_->IsRunning()) {
        return (uint32_t)spectator_instance_->GetProcessId();
    }
    if (game_instance_ && game_instance_->IsRunning()) {
        return (uint32_t)game_instance_->GetProcessId();
    }
    if (client1_instance_ && client1_instance_->IsRunning()) {
        return (uint32_t)client1_instance_->GetProcessId();
    }
    return 0;
}

void FM2KLauncher::Update(float delta_time SDL_UNUSED) {
    if (!running_) {
        // If the main loop is not running, trigger a clean shutdown
        // This handles cases where on_exit is called
        SDL_Event quit_event;
        quit_event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit_event);
        return;
    }

    // DLL handles GekkoNet directly - no launcher-side session needed

    // Hub event drain + match-outcome reporting. MUST run here rather than in
    // the render path: Render() is skipped entirely while minimized/hidden, so
    // hosting a set with the launcher minimized used to silently report ZERO
    // matches and leave the in-game HUD showing "P1"/"P2". ImGui-free by
    // construction (see LauncherUI::TickAlways).
    if (ui_) ui_->TickAlways();

    // Process DLL events from the game instance
    if (game_instance_ && game_instance_->IsRunning()) {
        game_instance_->ProcessDLLEvents();
    }

    // TCP-STUN poll: when the spec hook completes its outbound STUN
    // probe (FM2KHook/src/netplay/spectator_tcp.cpp PerformTcpStun), it
    // bumps tcp_stun_seq in SharedMem with the discovered external
    // (ip, port). Forward to hub via WS so cross-NAT spectators can be
    // told the right TCP punch target. Track per-pid last-seen seq so
    // we only forward fresh values; resets when game_instance restarts.
    {
        DWORD target_pid = 0;
        if (game_instance_ && game_instance_->IsRunning()) {
            target_pid = game_instance_->GetProcessId();
        } else if (client1_instance_ && client1_instance_->IsRunning()) {
            target_pid = client1_instance_->GetProcessId();
        }
        static DWORD    s_last_pid = 0;
        static uint32_t s_last_seq = 0;
        static uint32_t s_last_sk_seq = 0;
        if (target_pid != 0 && target_pid != s_last_pid) {
            s_last_pid = target_pid;
            s_last_seq = 0;
            s_last_sk_seq = 0;
        }
        if (target_pid != 0) {
            const std::string mapping_name =
                "FM2K_SharedMem_" + std::to_string((unsigned)target_pid);
            HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE,
                                        mapping_name.c_str());
            if (h) {
                FM2KSharedMemData* shm = static_cast<FM2KSharedMemData*>(
                    MapViewOfFile(h, FILE_MAP_READ, 0, 0,
                                  sizeof(FM2KSharedMemData)));
                if (shm && shm->magic == FM2K_SHARED_MEM_MAGIC &&
                    shm->tcp_stun_seq != 0 &&
                    shm->tcp_stun_seq != s_last_seq) {
                    s_last_seq = shm->tcp_stun_seq;
                    if (ui_) {
                        ui_->SendHubTcpAddr(shm->tcp_stun_ext_ip_be,
                                            shm->tcp_stun_ext_port);
                    }
                }
                // session_kind poll: forwards menu/CSS/battle phase
                // transitions to the hub so spectators joining us know
                // whether to /F-boot-to-battle or walk title→CSS.
                if (shm && shm->magic == FM2K_SHARED_MEM_MAGIC &&
                    shm->session_kind_seq != 0 &&
                    shm->session_kind_seq != s_last_sk_seq) {
                    s_last_sk_seq = shm->session_kind_seq;
                    if (ui_) {
                        ui_->SendHubSessionKind(shm->session_kind);
                    }
                }
                if (shm) UnmapViewOfFile(shm);
                CloseHandle(h);
            }
        }
    }

    // Spec hub-relay drain (Phase 2c). When the hook is running in
    // FM2K_SPEC_TRANSPORT=relay mode it creates the outbound shared-mem
    // ring "FM2K_SpecRelayOut_<pid>" and produces one Slot per spec
    // frame it wants to ship. We open the ring lazily, drain pop-able
    // slots each tick, pack each Slot into the SpecDataBinary wire
    // shape (matches hub.py:handle_spec_relay_frame), and ship via
    // HubClient::SendSpecRelayFrame (-> WS binary frame).
    //
    // Hook in TCP mode never creates the mapping; OpenOutboundFor
    // returns nullptr and the drain is a no-op. No env probing or
    // mode detection needed on the launcher side -- existence of the
    // mapping IS the signal.
    {
        // Player slots first, spectator last -- SpecRelayOutboundPid()
        // documents why. The spectator slot used to be missing here AND
        // in the inbound callback, which is what made relay spectate
        // unreachable on a viewer launcher.
        const uint32_t target_pid = SpecRelayOutboundPid();

        // Outbound ring cached as class member (was lambda static)
        // for the same reason as inbound -- menu-bar status pill needs
        // live access.
        auto* relay_ring_ptr =
            static_cast<fm2k::spec_relay::Ring*>(spec_relay_out_ring_);

        // Game pid changed (or fresh process); drop the old mapping.
        if (target_pid != spec_relay_out_pid_) {
            if (relay_ring_ptr) {
                fm2k::spec_relay::Close(relay_ring_ptr);
                relay_ring_ptr = nullptr;
                spec_relay_out_ring_ = nullptr;
            }
            spec_relay_out_pid_ = target_pid;
        }
        // Retry open every tick when we have a pid but no ring. Hook
        // creates the mapping during SpectatorNode_Init which races our
        // first tick after game spawn; without retry the cache sticks
        // at nullptr even though the hook came up milliseconds later.
        // OpenFileMappingA returns fast on miss so tick-rate retry is
        // cheap.
        if (target_pid != 0 && !relay_ring_ptr) {
            relay_ring_ptr = fm2k::spec_relay::OpenOutboundFor(target_pid);
            spec_relay_out_ring_ = relay_ring_ptr;
            if (relay_ring_ptr) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpecRelay: opened outbound ring for game pid %lu",
                    (unsigned long)target_pid);
            }
        }

        // Drain. Bound work-per-tick so a snapshot transfer doesn't
        // monopolize the UI loop; 32 slots × 16 KB = ~512 KB / tick max,
        // and snapshot transfers are paced by GekkoNet's broadcast
        // cadence anyway so this rarely saturates.
        if (relay_ring_ptr && ui_) {
            constexpr int kMaxPerTick = 32;
            for (int i = 0; i < kMaxPerTick; ++i) {
                const fm2k::spec_relay::Slot* slot =
                    fm2k::spec_relay::PeekFront(relay_ring_ptr);
                if (!slot) break;

                // Pack Slot -> SpecDataBinary wire frame.
                //   u32 magic = 0x53504442 ("SPDB")
                //   u32 frame_count
                //   u16 type
                //   u16 flags
                //   u32 payload_len
                //   u8  target_kind
                //   u8  spec_user_id_len
                //   char spec_user_id[]
                //   u8  payload[]
                std::vector<uint8_t> frame;
                const uint32_t magic = 0x53504442u;
                const uint8_t spec_id_len =
                    slot->target_kind == fm2k::spec_relay::TARGET_DIRECT
                        ? (uint8_t)std::strlen(slot->spec_user_id)
                        : 0;
                frame.reserve(16 + 2 + spec_id_len + slot->payload_len);
                auto append_u32 = [&](uint32_t v) {
                    frame.push_back((uint8_t)(v));
                    frame.push_back((uint8_t)(v >> 8));
                    frame.push_back((uint8_t)(v >> 16));
                    frame.push_back((uint8_t)(v >> 24));
                };
                auto append_u16 = [&](uint16_t v) {
                    frame.push_back((uint8_t)(v));
                    frame.push_back((uint8_t)(v >> 8));
                };
                append_u32(magic);
                append_u32(slot->frame_count);
                append_u16((uint16_t)slot->spec_data_type);
                append_u16((uint16_t)slot->spec_data_flags);
                append_u32(slot->payload_len);
                frame.push_back((uint8_t)slot->target_kind);
                frame.push_back(spec_id_len);
                if (spec_id_len) {
                    frame.insert(frame.end(),
                                 slot->spec_user_id,
                                 slot->spec_user_id + spec_id_len);
                }
                if (slot->payload_len) {
                    frame.insert(frame.end(),
                                 slot->payload,
                                 slot->payload + slot->payload_len);
                }

                ui_->SendHubSpecRelayFrame(std::move(frame));
                fm2k::spec_relay::PopFront(relay_ring_ptr);
            }
        }

        // Inbound ring lifecycle. WireUICallbacks' on_spec_relay_bytes
        // opens it, but that only runs when a WS binary frame actually
        // arrives -- so a spec game that exits (or a launcher that flips
        // from viewer to player) would leave the dead pid's mapping
        // cached and its frozen counters on the status pill forever.
        // Reconcile here, where we tick regardless of hub traffic, using
        // the same resolver the callback uses. Mirrors what the outbound
        // block above already does for its own pid.
        const uint32_t in_pid = SpecRelayInboundPid();
        if (in_pid != spec_relay_in_pid_) {
            if (spec_relay_in_ring_) {
                fm2k::spec_relay::Close(
                    static_cast<fm2k::spec_relay::Ring*>(spec_relay_in_ring_));
                spec_relay_in_ring_ = nullptr;
            }
            spec_relay_in_pid_ = in_pid;
        }

        // Phase 4: surface the latest ring counters to the menu-bar
        // status pill. Both outbound (host produces -> launcher drains)
        // and inbound (launcher fills -> hook drains) read from class
        // members (promoted from lambda statics so this read works).
        LauncherUI::SpecRelayStatus st{};
        st.out_active = (relay_ring_ptr != nullptr);
        if (st.out_active) {
            st.out_enqueued = relay_ring_ptr->total_enqueued;
            st.out_dropped  = relay_ring_ptr->total_dropped;
            st.out_dequeued = relay_ring_ptr->total_dequeued;
        }
        auto* in_ring_status =
            static_cast<const fm2k::spec_relay::Ring*>(spec_relay_in_ring_);
        st.in_active = (in_ring_status != nullptr);
        if (st.in_active) {
            st.in_enqueued = in_ring_status->total_enqueued;
            st.in_dropped  = in_ring_status->total_dropped;
            st.in_dequeued = in_ring_status->total_dequeued;
        }
        if (ui_) {
            // Post-ring losses. A frame we popped and handed to the
            // HubClient can still die there (no hub connection, outbox at
            // its cap, discarded at reconnect) and the ring counters would
            // never show it -- from the ring's side the slot was dequeued
            // just fine. Without this the pill reads healthy while the
            // spec data never leaves the machine.
            st.out_hub_dropped = ui_->HubSpecRelayDropped();
            ui_->SetSpecRelayStatus(st);
        }
    }

    // Check for game termination. Players run under game_instance_; the
    // --spectate / --replay paths run under spectator_instance_ -- the original
    // check only watched game_instance_, so a spectator whose game exited (the
    // host-gone watchdog, a desync-terminate, or a crash) left the LAUNCHER
    // zombie-ing forever (the "launcher didnt kill" stuck window the harness
    // then had to timeout-kill at 300s). Watch BOTH.
    const bool game_gone = game_instance_ && !game_instance_->IsRunning();
    const bool spec_gone = spectator_instance_ && !spectator_instance_->IsRunning();
    if (game_gone || spec_gone) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hosted %s process has terminated.", game_gone ? "game" : "spectator");
        if (game_gone) {
            StopSession();              // player-session teardown -> GameSelection
        } else {
            spectator_instance_.reset();  // drop the dead spectator/replay handle
            SetState(LauncherState::GameSelection);
        }
        // CLI direct-mode (harness / --host / --connect / --spectate / --replay):
        // there is no lobby to return to, so the hosted process exiting means
        // we're done. Quit instead of zombie-ing the window.
        if (exit_on_game_end_) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Direct-mode: hosted game ended -- exiting launcher.");
            running_ = false;
        }
    }
}

void FM2KLauncher::Render() {
    // ImGui frame begins here, NOT in Update(). SDL_AppIterate skips this whole
    // method when the UI is idle/backgrounded; keeping NewFrame paired with
    // Render()+present means a skipped frame never leaves a half-open ImGui
    // frame (which would assert). Update() runs every tick for IPC; Render()
    // runs only when we actually paint.
    ui_->NewFrame();

    // Clear screen
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    
    // Render UI
    ui_->Render();

    // Finalize ImGui draw data
    ImGui::Render();
    
    // Render ImGui draw data using the SDL_Renderer backend
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    
    // Update and Render additional Platform Windows
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
    
    SDL_RenderPresent(renderer_);
}

void FM2KLauncher::SetSelectedGame(const FM2K::FM2KGameInfo& game) {
    selected_game_ = game;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Game selected via code: %s", game.exe_path.c_str());
}

void FM2KLauncher::SetGamesRootPaths(const std::vector<std::string>& paths) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Set games root paths: %d entry/entries", (int)paths.size());
    for (const auto& p : paths) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  - %s", p.c_str());
    }
    games_root_paths_ = paths;
    Utils::SaveGamesRootPaths(paths);
    if (ui_) ui_->SetGamesRootPaths(paths);  // Update UI with new paths

    // Kick off background discovery so the UI stays responsive
    StartAsyncDiscovery();
}

void FM2KLauncher::SetState(LauncherState state) {
    current_state_ = state;
    if (ui_) {
        ui_->SetLauncherState(state);
    }
    // Optional power-saving UX, default OFF (the launcher-while-game behavior
    // is otherwise handled by the BACKGROUND_GAME render-skip tier, which
    // already drops to ~zero paint cost while keeping IPC alive). When the
    // user opts in with FM2K_MINIMIZE_ON_LAUNCH=1, minimize the window the
    // moment a game starts so SDL_AppIterate routes into the MINIMIZED tier
    // (cheapest: IPC-only, 10 Hz, never paints). Spec relay still flows there
    // because the IPC slice runs regardless of visibility. Restoring the
    // window (taskbar click) snaps it back to a normal tier.
    if (state == LauncherState::InGame && window_) {
        static const bool minimize_on_launch = [] {
            const char* v = std::getenv("FM2K_MINIMIZE_ON_LAUNCH");
            return v && v[0] == '1';
        }();
        if (minimize_on_launch) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "FM2K_MINIMIZE_ON_LAUNCH=1 -> minimizing launcher behind game");
            SDL_MinimizeWindow(window_);
        }
    }
}

bool FM2KLauncher::UiWantsContinuousRedraw() const {
    return ui_ ? ui_->WantsContinuousRedraw() : false;
}
