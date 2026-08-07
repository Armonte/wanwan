// launcher_ui_display.cpp -- LauncherUI display/cnc-ddraw settings panel. Split from FM2K_LauncherUI.cpp (pure member-fn move).
#include "FM2K_Integration.h"
#include "launcher_ui_internal.h"  // shared persistence helpers (namespace lui)
#include "FM2K_HubClient.h"
#include "FM2K_PortMapper.h"  // UPnP port mapper member of LauncherUI (Phase 1)
#include "FM2K_DiscordAuth.h"
#include "FM2K_Locale.h"
#include "FM2K_Updater.h"
#include "version_local.h"
#include "auto_upload_secret.h"
#include "FM2K_UploadQueue.h"
#include "FM2KHook/src/ui/input_binder.h"
#include "FM2KHook/src/ui/shared_mem.h"
#include "FM2KHook/src/util/pii_scrub.h"
#include "FM2K_GameIni.h"
#include "FM2K_DDrawRedirect.h"
#include "FM2K_CncDDraw.h"
#include "FM2K_Utf8Path.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wininet.h>
#include <shellapi.h>  // Shell_NotifyIcon for challenge toast
#include <shobjidl.h>  // IFileOpenDialog (modern native folder picker)
#include <iostream>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>
#include <array>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>
#include "vendored/imgui/imgui.h"
#include "imgui_internal.h"
#include "vendored/GekkoNet/GekkoLib/include/gekkonet.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <memory>
#include <unordered_map>
#include <unordered_set>

using namespace lui;  // shared persistence helpers (launcher_ui_internal.h)

void LauncherUI::LoadDDrawCfgIfNeeded() {
    if (ddraw_cfg_loaded_) return;
    fm2k::cnc_ddraw::LoadIni(ddraw_cfg_);
    ddraw_cfg_loaded_ = true;
}

// Settings → Display. Mirrors every documented cnc-ddraw [ddraw] key
// from <install_dir>\ddraw.ini. Edits write per-key on change so each
// flick of a checkbox lands on disk immediately -- no Apply button.
// Changes take effect on the NEXT game launch (cnc-ddraw reads its ini
// at DLL_PROCESS_ATTACH); the header label calls that out.
//
// Sectioning mirrors cnc-ddraw config.exe's tabs for familiarity:
// Window mode → Renderer → Performance → Hotkeys → Compatibility →
// Undocumented (collapsing).
void LauncherUI::RenderDisplayBody() {
    LoadDDrawCfgIfNeeded();

    namespace cd = fm2k::cnc_ddraw;
    auto& c = ddraw_cfg_;

    ImGui::TextWrapped(
        "cnc-ddraw renderer settings. Changes apply on the NEXT game launch.");
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
        "Editing %s", cd::IniPath().c_str());
    ImGui::Spacing();
    if (ImGui::Button("Reset to launcher defaults")) {
        if (cd::ResetIniToDefault()) {
            ddraw_cfg_ = cd::IniConfig{};   // back to header defaults
            cd::LoadIni(ddraw_cfg_);        // pull in baked-ini values
        }
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.85f, 0.7f, 0.4f, 1.0f),
        "Wipes any per-game [<exe>] blocks");
    ImGui::Separator();

    // ── Window mode ──────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Window mode", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Render mode is the (windowed, fullscreen) tuple. cnc-ddraw
        // semantics: windowed=true & fullscreen=false → real windowed,
        // both true → borderless windowed-fullscreen, fullscreen=true
        // & windowed=false → fullscreen-upscaled. Surface as a 3-way
        // combo + an "advanced" raw checkbox pair.
        const char* mode_items[] = {
            "Windowed",
            "Borderless (windowed-fullscreen)",
            "Fullscreen upscaled"
        };
        int mode_idx = 0;
        if      ( c.windowed &&  c.fullscreen) mode_idx = 1;
        else if (!c.windowed &&  c.fullscreen) mode_idx = 2;
        else                                    mode_idx = 0;
        if (ImGui::Combo("Mode", &mode_idx, mode_items, IM_ARRAYSIZE(mode_items))) {
            switch (mode_idx) {
                case 0: c.windowed = true;  c.fullscreen = false; break;
                case 1: c.windowed = true;  c.fullscreen = true;  break;
                case 2: c.windowed = false; c.fullscreen = true;  break;
            }
            cd::SaveBool("windowed",   c.windowed);
            cd::SaveBool("fullscreen", c.fullscreen);
        }

        if (ImGui::DragInt("Width  (0 = use game's)",  &c.width,  1, 0, 7680)) {
            cd::SaveInt("width",  c.width);
        }
        if (ImGui::DragInt("Height (0 = use game's)",  &c.height, 1, 0, 4320)) {
            cd::SaveInt("height", c.height);
        }

        char ar_buf[32] = {};
        std::snprintf(ar_buf, sizeof(ar_buf), "%s", c.aspect_ratio.c_str());
        if (ImGui::InputText("Aspect ratio (e.g. 4:3, 16:9, blank=auto)",
                             ar_buf, sizeof(ar_buf))) {
            c.aspect_ratio = ar_buf;
            cd::SaveString("aspect_ratio", c.aspect_ratio);
        }

        if (ImGui::Checkbox("Maintain aspect ratio", &c.maintas)) {
            cd::SaveBool("maintas", c.maintas);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Integer scaling (boxing)", &c.boxing)) {
            cd::SaveBool("boxing", c.boxing);
        }

        if (ImGui::Checkbox("Window border (windowed mode)", &c.border)) {
            cd::SaveBool("border", c.border);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("User resizable", &c.resizable)) {
            cd::SaveBool("resizable", c.resizable);
        }

        const char* center_items[] = {
            "Never center", "Automatic", "Always center"
        };
        if (ImGui::Combo("Center on resolution change", &c.center_window,
                         center_items, IM_ARRAYSIZE(center_items))) {
            cd::SaveInt("center_window", c.center_window);
        }
        if (ImGui::DragInt("Window posX (-32000 = center)", &c.posX, 1, -32000, 32000)) {
            cd::SaveInt("posX", c.posX);
        }
        if (ImGui::DragInt("Window posY (-32000 = center)", &c.posY, 1, -32000, 32000)) {
            cd::SaveInt("posY", c.posY);
        }
        const char* save_items[] = {
            "Don't save", "Save to [ddraw]", "Save per-game [<exe>]"
        };
        if (ImGui::Combo("Save window position/size on exit", &c.savesettings,
                         save_items, IM_ARRAYSIZE(save_items))) {
            cd::SaveInt("savesettings", c.savesettings);
        }
    }

    // ── Renderer ─────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Renderer is locked to direct3d9: our in-game ImGui overlay
        // hooks `IDirect3DDevice9::EndScene` (FM2KHook/src/ui/imgui_overlay.cpp)
        // and would never attach if cnc-ddraw routed through OpenGL or
        // GDI. Force-write on every render of this tab so a stale
        // pre-existing ini gets corrected the moment the user opens it.
        if (c.renderer != "direct3d9") {
            c.renderer = "direct3d9";
            cd::SaveString("renderer", c.renderer);
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f),
            "Renderer: direct3d9  (locked)");
        ImGui::TextWrapped(
            "Locked so the launcher's in-game ImGui overlay can attach via "
            "IDirect3DDevice9::EndScene. Edit ddraw.ini directly if you "
            "really need a different backend (opengl / gdi / direct3d9on12).");

        const char* d3d9_items[] = {
            "Nearest neighbor", "Bilinear", "Bicubic (16/32-bit only)",
            "Lanczos (16/32-bit only)"
        };
        if (ImGui::Combo("Direct3D9 upscale filter", &c.d3d9_filter,
                         d3d9_items, IM_ARRAYSIZE(d3d9_items))) {
            cd::SaveInt("d3d9_filter", c.d3d9_filter);
        }

        char shader_buf[512] = {};
        std::snprintf(shader_buf, sizeof(shader_buf), "%s", c.shader.c_str());
        if (ImGui::InputText("Shader (path or name)", shader_buf, sizeof(shader_buf))) {
            c.shader = shader_buf;
            cd::SaveString("shader", c.shader);
        }

        if (ImGui::Checkbox("VSync", &c.vsync)) {
            cd::SaveBool("vsync", c.vsync);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Disable fullscreen-exclusive mode", &c.nonexclusive)) {
            cd::SaveBool("nonexclusive", c.nonexclusive);
        }

        char inj_buf[64] = {};
        std::snprintf(inj_buf, sizeof(inj_buf), "%s", c.inject_resolution.c_str());
        if (ImGui::InputText("Inject resolution (e.g. 960x540)",
                             inj_buf, sizeof(inj_buf))) {
            c.inject_resolution = inj_buf;
            cd::SaveString("inject_resolution", c.inject_resolution);
        }

        if (ImGui::Checkbox("vhack (high-res patches: C&C, RA1, Worms 2, KKND Xtreme)",
                            &c.vhack)) {
            cd::SaveBool("vhack", c.vhack);
        }
    }

    // ── Performance ──────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Performance / framerate")) {
        if (ImGui::DragInt("maxfps (-1=screen, 0=unlimited, n=cap)",
                           &c.maxfps, 1, -1, 1000)) {
            cd::SaveInt("maxfps", c.maxfps);
        }
        if (ImGui::DragInt("maxgameticks (-1=disabled, -2=refresh rate, 0=60Hz vblank)",
                           &c.maxgameticks, 1, -2, 1000)) {
            cd::SaveInt("maxgameticks", c.maxgameticks);
        }
        if (ImGui::DragInt("minfps (0=disabled, -1=use maxfps, -2=force redraw)",
                           &c.minfps, 1, -2, 1000)) {
            cd::SaveInt("minfps", c.minfps);
        }
        const char* limiter_items[] = {
            "Automatic", "TestCooperativeLevel", "BltFast", "Unlock", "PeekMessage"
        };
        if (ImGui::Combo("Limiter type", &c.limiter_type,
                         limiter_items, IM_ARRAYSIZE(limiter_items))) {
            cd::SaveInt("limiter_type", c.limiter_type);
        }
    }

    // ── Input ────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Input")) {
        if (ImGui::Checkbox("Auto mouse-sensitivity scaling (adjmouse)", &c.adjmouse)) {
            cd::SaveBool("adjmouse", c.adjmouse);
        }
        if (ImGui::Checkbox("Devmode (don't lock cursor)", &c.devmode)) {
            cd::SaveBool("devmode", c.devmode);
        }
        if (ImGui::Checkbox("hook_peekmessage (cursor-lock fix on upscaling)",
                            &c.hook_peekmessage)) {
            cd::SaveBool("hook_peekmessage", c.hook_peekmessage);
        }
    }

    // ── Hotkeys ──────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Hotkeys")) {
        // Press-to-bind UI mirrors the input-binder pattern: click
        // "Bind", wait for the user to release any keys still held from
        // the click, then capture the next VK that goes down. Esc =
        // cancel, Backspace = clear binding to 0 (disabled).
        //
        // We poll GetKeyState across VK 0x07..0xFE (skip mouse
        // buttons 0x01..0x06 so the click isn't read back). GetKeyState
        // is synchronized to this thread's message queue, so keys typed
        // while the launcher ISN'T focused can never bind (the desktop-
        // global GetAsyncKeyState could grab those). Each capture stores
        // the resolved VK back into the IniConfig field and writes
        // through fm2k::cnc_ddraw::SaveHex so the ini matches the
        // format the cnc-ddraw stock ini ships in (0xNN).
        static const char* s_capture_key   = nullptr;  // ini key being captured
        static int*        s_capture_field = nullptr;  // pointer into ddraw_cfg_
        static bool        s_capture_armed = false;    // released since click?

        auto vk_label = [](int vk) -> std::string {
            if (vk == 0) return "(disabled)";
            UINT scan = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
            char buf[64] = {};
            if (scan && GetKeyNameTextA((LONG)(scan << 16), buf, sizeof(buf)) > 0) {
                char out[96];
                std::snprintf(out, sizeof(out), "%s  (0x%02X)", buf, vk);
                return out;
            }
            char out[32];
            std::snprintf(out, sizeof(out), "VK 0x%02X", vk);
            return out;
        };

        auto any_key_held = []() {
            for (int vk = 0x01; vk <= 0xFE; ++vk) {
                if ((GetKeyState(vk) & 0x8000) != 0) return true;
            }
            return false;
        };

        // Drive the capture state machine -- runs once per frame regardless
        // of which row's Bind button started it. Fires before we render
        // the rows so a successful capture is reflected this frame.
        if (s_capture_key && s_capture_field) {
            if (!s_capture_armed) {
                if (!any_key_held()) s_capture_armed = true;
            } else {
                // Esc cancels without writing.
                if ((GetKeyState(VK_ESCAPE) & 0x8000) != 0) {
                    s_capture_key = nullptr;
                    s_capture_field = nullptr;
                    s_capture_armed = false;
                } else if ((GetKeyState(VK_BACK) & 0x8000) != 0) {
                    // Backspace clears the binding.
                    *s_capture_field = 0;
                    cd::SaveHex(s_capture_key, 0);
                    s_capture_key = nullptr;
                    s_capture_field = nullptr;
                    s_capture_armed = false;
                } else {
                    // Skip mouse buttons 0x01..0x06 (the click that
                    // started capture would re-trigger). All other VKs
                    // are fair game.
                    for (int vk = 0x07; vk <= 0xFE; ++vk) {
                        if ((GetKeyState(vk) & 0x8000) != 0) {
                            *s_capture_field = vk;
                            cd::SaveHex(s_capture_key, vk);
                            s_capture_key = nullptr;
                            s_capture_field = nullptr;
                            s_capture_armed = false;
                            break;
                        }
                    }
                }
            }
        }

        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
            "Click Bind, then press the key. Esc = cancel, Backspace = clear.");

        auto hk_row = [&](const char* label, const char* ini_key, int* field) {
            ImGui::PushID(ini_key);
            const bool waiting = (s_capture_key == ini_key);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", label);
            ImGui::SameLine(280.0f);
            if (waiting) {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                    "%s",
                    s_capture_armed ? "Press a key..."
                                    : "Release any held keys...");
            } else {
                ImGui::TextUnformatted(vk_label(*field).c_str());
            }
            ImGui::SameLine(460.0f);
            if (waiting) {
                if (ImGui::Button("Cancel", ImVec2(80, 0))) {
                    s_capture_key = nullptr;
                    s_capture_field = nullptr;
                    s_capture_armed = false;
                }
            } else {
                if (ImGui::Button("Bind", ImVec2(80, 0))) {
                    s_capture_key = ini_key;
                    s_capture_field = field;
                    s_capture_armed = false;  // wait for release
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear", ImVec2(60, 0))) {
                *field = 0;
                cd::SaveHex(ini_key, 0);
                if (waiting) {
                    s_capture_key = nullptr;
                    s_capture_field = nullptr;
                    s_capture_armed = false;
                }
            }
            ImGui::PopID();
        };

        hk_row("Toggle fullscreen (Alt+...)",  "keytogglefullscreen",  &c.keytogglefullscreen);
        hk_row("Toggle fullscreen 2 (single)", "keytogglefullscreen2", &c.keytogglefullscreen2);
        hk_row("Maximize window (Alt+...)",    "keytogglemaximize",    &c.keytogglemaximize);
        hk_row("Maximize window 2 (single)",   "keytogglemaximize2",   &c.keytogglemaximize2);
        hk_row("Unlock cursor 1 (Ctrl+...)",   "keyunlockcursor1",     &c.keyunlockcursor1);
        hk_row("Unlock cursor 2 (RAlt+...)",   "keyunlockcursor2",     &c.keyunlockcursor2);
        hk_row("Screenshot",                   "keyscreenshot",        &c.keyscreenshot);

        ImGui::Spacing();
        if (ImGui::Checkbox("Alt+Enter toggles windowed/borderless instead of fullscreen",
                            &c.toggle_borderless)) {
            cd::SaveBool("toggle_borderless", c.toggle_borderless);
        }
        if (ImGui::Checkbox("Alt+Enter toggles windowed/upscaled instead",
                            &c.toggle_upscaled)) {
            cd::SaveBool("toggle_upscaled", c.toggle_upscaled);
        }
    }

    // ── Compatibility ────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Compatibility")) {
        if (ImGui::Checkbox("Hide WM_ACTIVATEAPP/NCACTIVATE on alt-tab (noactivateapp)",
                            &c.noactivateapp)) {
            cd::SaveBool("noactivateapp", c.noactivateapp);
        }
        if (ImGui::Checkbox("Force CPU0 affinity (singlecpu)", &c.singlecpu)) {
            cd::SaveBool("singlecpu", c.singlecpu);
        }
        const char* res_items[] = {
            "Small list", "Very small list", "Full list"
        };
        if (ImGui::Combo("Available display resolutions", &c.resolutions,
                         res_items, IM_ARRAYSIZE(res_items))) {
            cd::SaveInt("resolutions", c.resolutions);
        }
        const char* fc_items[] = {
            "Disabled", "Display top-left", "Display top-left + repaint",
            "Hide", "Display top-left + hide"
        };
        if (ImGui::Combo("fixchilds (child window handling)", &c.fixchilds,
                         fc_items, IM_ARRAYSIZE(fc_items))) {
            cd::SaveInt("fixchilds", c.fixchilds);
        }
        if (ImGui::DragInt("anti_aliased_fonts_min_size",
                           &c.anti_aliased_fonts_min_size, 1, 0, 100)) {
            cd::SaveInt("anti_aliased_fonts_min_size", c.anti_aliased_fonts_min_size);
        }
        if (ImGui::DragInt("min_font_size",
                           &c.min_font_size, 1, 0, 100)) {
            cd::SaveInt("min_font_size", c.min_font_size);
        }

        char ssdir_buf[260] = {};
        std::snprintf(ssdir_buf, sizeof(ssdir_buf), "%s", c.screenshotdir.c_str());
        if (ImGui::InputText("Screenshot directory", ssdir_buf, sizeof(ssdir_buf))) {
            c.screenshotdir = ssdir_buf;
            cd::SaveString("screenshotdir", c.screenshotdir);
        }
    }

    // ── Undocumented / advanced ──────────────────────────────────────
    if (ImGui::CollapsingHeader("Advanced (undocumented -- only touch if needed)")) {
        ImGui::TextColored(ImVec4(0.85f, 0.6f, 0.4f, 1.0f),
            "Per cnc-ddraw: 'These will probably not solve your problem'.");
        if (ImGui::Checkbox("fix_alt_key_stuck", &c.fix_alt_key_stuck))
            cd::SaveBool("fix_alt_key_stuck", c.fix_alt_key_stuck);
        if (ImGui::Checkbox("game_handles_close", &c.game_handles_close))
            cd::SaveBool("game_handles_close", c.game_handles_close);
        if (ImGui::Checkbox("fix_not_responding", &c.fix_not_responding))
            cd::SaveBool("fix_not_responding", c.fix_not_responding);
        if (ImGui::Checkbox("no_compat_warning", &c.no_compat_warning))
            cd::SaveBool("no_compat_warning", c.no_compat_warning);
        if (ImGui::Checkbox("lock_surfaces", &c.lock_surfaces))
            cd::SaveBool("lock_surfaces", c.lock_surfaces);
        if (ImGui::Checkbox("flipclear", &c.flipclear))
            cd::SaveBool("flipclear", c.flipclear);
        if (ImGui::Checkbox("rgb555", &c.rgb555))
            cd::SaveBool("rgb555", c.rgb555);
        if (ImGui::Checkbox("no_dinput_hook", &c.no_dinput_hook))
            cd::SaveBool("no_dinput_hook", c.no_dinput_hook);
        if (ImGui::Checkbox("center_cursor_fix", &c.center_cursor_fix))
            cd::SaveBool("center_cursor_fix", c.center_cursor_fix);
        if (ImGui::Checkbox("lock_mouse_top_left", &c.lock_mouse_top_left))
            cd::SaveBool("lock_mouse_top_left", c.lock_mouse_top_left);
        if (ImGui::Checkbox("limit_gdi_handles", &c.limit_gdi_handles))
            cd::SaveBool("limit_gdi_handles", c.limit_gdi_handles);
        if (ImGui::Checkbox("remove_menu", &c.remove_menu))
            cd::SaveBool("remove_menu", c.remove_menu);

        if (ImGui::DragInt("guard_lines", &c.guard_lines, 1, 0, 1000))
            cd::SaveInt("guard_lines", c.guard_lines);
        if (ImGui::DragInt("max_resolutions", &c.max_resolutions, 1, 0, 100))
            cd::SaveInt("max_resolutions", c.max_resolutions);
        if (ImGui::DragInt("hook (mode 1-4; default 4)", &c.hook, 1, 0, 4))
            cd::SaveInt("hook", c.hook);
        if (ImGui::DragInt("refresh_rate (0 = monitor default)",
                           &c.refresh_rate, 1, 0, 360))
            cd::SaveInt("refresh_rate", c.refresh_rate);
    }
}
