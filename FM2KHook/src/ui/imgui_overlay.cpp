// ImGui overlay for FM2K - debug display
#include "imgui_overlay.h"
#include "imgui_overlay_internal.h"
#include "fc_hud.h"
#include "globals.h"  // FM2K::kIsFM95 — engine-aware window class match
#include "netplay.h"
#include "savestate.h"
#include "../hooks/per_game_patches.h"  // training / OPTION mode badges + F2 cycle
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx9.h>
#include <MinHook.h>
#include <SDL3/SDL_log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// D3D9 function typedefs
typedef HRESULT(APIENTRY* EndScene_t)(LPDIRECT3DDEVICE9 pDevice);
typedef HRESULT(APIENTRY* Reset_t)(LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters);

// Original function pointers
static EndScene_t EndScene_orig = nullptr;
static Reset_t Reset_orig = nullptr;
static WNDPROC oWndProc = nullptr;

// State
bool g_overlay_visible = false;
// In-game HUD master visibility. Defaults OFF for the v0.2.5 release —
// the HUD scaffolding (top bar / chat / system message) is shipped but
// hidden until F9 toggles it on. Once user-tested for stability and we
// wire delay/score/match-state surfacing properly, flip the default to
// true. F9's ToggleOverlay() flips this alongside g_overlay_visible
// so one keystroke shows both the dev overlay and the in-game HUD.
static bool g_hud_master_visible = false;
static bool g_imgui_initialized = false;
static bool g_hooks_installed = false;
static HWND g_game_window = nullptr;
static bool g_f9_key_pressed = false;
static bool g_f10_key_pressed = false;
// Tracks the IDirect3DDevice9 pointer ImGui's DX9 backend was last
// initialized against. cnc-ddraw's fullscreen toggle path either
// `Reset`s the device (render_d3d9.c:219) or fully `Release`s and
// recreates it (render_d3d9.c:73,160). Reset is caught by Hook_Reset
// below; the release-and-recreate path doesn't fire Reset at all, so
// we detect by comparing the SWAP CHAIN's private-data tag below
// (pointer comparison is unreliable — Windows heap reuse routinely
// returns the same address for a freshly recreated device, leaving
// the imgui backend pointing at freed GPU resources and producing
// glitched glyphs every other fullscreen toggle).
static LPDIRECT3DDEVICE9 g_imgui_device = nullptr;

// Stamped on every back-buffer surface we've initialized the imgui
// DX9 backend against, via IDirect3DSurface9::SetPrivateData. Survives
// only as long as the surface itself does — any Reset/Release
// recreates the back buffer, the new one comes back without our tag,
// and we know unambiguously that we need to rebuild. Resilient to
// heap address reuse for both the device and back-buffer pointers.
// (IDirect3DSwapChain9 doesn't inherit IDirect3DResource9 so it has
// no Set/GetPrivateData; the back buffer surface does.)
//   {7C7AC1C5-2DFA-4B0F-9F4E-DE0F6A6F7AC1}
static const GUID kImguiSwapChainTagGuid = {
    0x7c7ac1c5, 0x2dfa, 0x4b0f,
    { 0x9f, 0x4e, 0xde, 0x0f, 0x6a, 0x6f, 0x7a, 0xc1 }
};
static const DWORD kImguiSwapChainTagValue = 0xCAFEF00D;

// Pixel-rect on the d3d9 backbuffer where cnc-ddraw is drawing the
// game-quad. cnc-ddraw's `SetViewport` call at render_d3d9.c:531-541
// is commented out — the quad is positioned via D3DFVF_XYZRHW vertex
// coords directly. Backbuffer's d3d9 viewport stays at full size, so
// `pDevice->GetViewport(...)` gives us the wrong answer. We recompute
// the rect ourselves using cnc-ddraw's exact formula (dd.c:924-979)
// against the live backbuffer dimensions.
//
// Used to clamp our debug ImGui window so it floats *over* the live
// game pixels and never over the black letterbox / pillarbox bars.
D3DVIEWPORT9 g_game_viewport = { 0, 0, 0, 0, 0.0f, 1.0f };

// cnc-ddraw layout flags read from <cnc_ddraw_dir>\ddraw.ini at first
// frame — these don't change without a launcher restart, so reading
// every frame is wasted work. `maintas` keeps 4:3, `boxing` does
// integer-scaled letterbox (overrides maintas), `aspect_ratio` is an
// optional override like "16:9" / "4:3".
static bool  g_layout_loaded     = false;
static bool  g_layout_maintas    = false;
static bool  g_layout_boxing     = false;
static char  g_layout_aspect_ratio[16] = {};

// Debug-only: render a green outline + tinted fill of the detected
// game viewport so the user can visually confirm rect detection is
// correct at different window sizes. Toggled from the overlay's UI.
bool g_show_game_rect_debug = false;

// Test results
bool g_last_test_ran = false;
bool g_last_test_passed = false;
char g_last_test_diff[1024] = "";
StateSnapshot g_last_snapshot_before;
StateSnapshot g_last_snapshot_after;

// Forward declarations
HRESULT APIENTRY Hook_EndScene(LPDIRECT3DDEVICE9 pDevice);
HRESULT APIENTRY Hook_Reset(LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters);
LRESULT WINAPI Hook_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// for cnc-ddraw to pick up new ini values), so caching the trio at
// first call avoids the per-frame profile-API hit.
static void LoadCncDdrawLayoutOnce() {
    if (g_layout_loaded) return;
    char ini[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("CNC_DDRAW_CONFIG_FILE", ini, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        // Fallback: <2DFMD.dll dir>\ddraw.ini
        HMODULE dd = GetModuleHandleA("2DFMD.dll");
        if (dd) {
            char dll_path[MAX_PATH] = {};
            if (GetModuleFileNameA(dd, dll_path, MAX_PATH) > 0) {
                char* slash = strrchr(dll_path, '\\');
                if (slash) {
                    *slash = '\0';
                    std::snprintf(ini, sizeof(ini), "%s\\ddraw.ini", dll_path);
                }
            }
        }
    }
    if (ini[0]) {
        char buf[16] = {};
        GetPrivateProfileStringA("ddraw", "maintas", "false",
                                 buf, sizeof(buf), ini);
        g_layout_maintas = (_stricmp(buf, "true") == 0);
        GetPrivateProfileStringA("ddraw", "boxing", "false",
                                 buf, sizeof(buf), ini);
        g_layout_boxing = (_stricmp(buf, "true") == 0);
        GetPrivateProfileStringA("ddraw", "aspect_ratio", "",
                                 g_layout_aspect_ratio,
                                 sizeof(g_layout_aspect_ratio), ini);
    }
    g_layout_loaded = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Overlay: cnc-ddraw layout maintas=%d boxing=%d aspect='%s'",
        (int)g_layout_maintas, (int)g_layout_boxing,
        g_layout_aspect_ratio[0] ? g_layout_aspect_ratio : "(default)");
}

// Replicate cnc-ddraw's `dd.c:924-979` to figure out where on the
// backbuffer the game quad will land. We use the d3d9 backbuffer's
// own dimensions (via GetBackBuffer/GetDesc) instead of GetClientRect
// — fullscreen-exclusive presents through a different surface size
// than the window client, and the backbuffer is the source of truth
// for the surface cnc-ddraw is rendering into.
static void ComputeCncDdrawGameRect(LPDIRECT3DDEVICE9 pDevice,
                                    D3DVIEWPORT9& out)
{
    LoadCncDdrawLayoutOnce();

    // Game logical resolution — fixed per engine.
    const int game_w = FM2K::kIsFM95 ? 320 : 640;
    const int game_h = FM2K::kIsFM95 ? 240 : 480;

    // Backbuffer (= cnc-ddraw's render-target) dimensions.
    int bb_w = 0, bb_h = 0;
    {
        IDirect3DSurface9* bb = nullptr;
        if (pDevice && SUCCEEDED(pDevice->GetBackBuffer(
                0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            D3DSURFACE_DESC desc = {};
            bb->GetDesc(&desc);
            bb_w = (int)desc.Width;
            bb_h = (int)desc.Height;
            bb->Release();
        }
    }
    if (bb_w <= 0 || bb_h <= 0) {
        out = {};
        return;
    }

    // Default: full backbuffer. boxing/maintas override below.
    int vp_x = 0, vp_y = 0, vp_w = bb_w, vp_h = bb_h;

    if (g_layout_boxing) {
        // Integer scaling, centered. Walks scale factor down from 20
        // to 2 looking for the largest that fits inside the backbuffer.
        for (int i = 20; i > 1; --i) {
            if (game_w * i <= bb_w && game_h * i <= bb_h) {
                vp_w = i * game_w;
                vp_h = i * game_h;
                vp_x = (bb_w - vp_w) / 2;
                vp_y = (bb_h - vp_h) / 2;
                break;
            }
        }
    } else if (g_layout_maintas) {
        // Aspect-preserving fit with optional aspect_ratio override.
        // src_ar/dst_ar match cnc-ddraw's variable names exactly.
        double src_ar = (double)bb_h / (double)bb_w;
        double dst_ar = (double)game_h / (double)game_w;
        if (g_layout_aspect_ratio[0]) {
            char* e = g_layout_aspect_ratio;
            unsigned long cx = strtoul(e, &e, 0);
            unsigned long cy = strtoul(e + 1, &e, 0);
            if (cx && cy) dst_ar = (double)cy / (double)cx;
        }
        int new_w = bb_w;
        int new_h = (int)(dst_ar * (double)new_w + 0.5);
        if (src_ar < dst_ar) {
            new_w = (int)(((double)new_w / (double)new_h) * (double)bb_h + 0.5);
            new_h = bb_h;
        }
        if (new_w > bb_w) new_w = bb_w;
        if (new_h > bb_h) new_h = bb_h;
        vp_w = new_w;
        vp_h = new_h;
        vp_x = (bb_w - new_w) / 2;
        vp_y = (bb_h - new_h) / 2;
    }

    out.X      = (DWORD)vp_x;
    out.Y      = (DWORD)vp_y;
    out.Width  = (DWORD)vp_w;
    out.Height = (DWORD)vp_h;
    out.MinZ   = 0.0f;
    out.MaxZ   = 1.0f;
}

// Render debug overlay

// Find the game window owned by THIS process. Mirrors
// wndproc_subclass.cpp's FindOwnWindowProc — the previous code used
// `FindWindowA(nullptr, "WonderfulWorld")` (title hardcoded, only
// matched one specific game) with a `GetForegroundWindow()` fallback
// that, in dual-client local testing, happily returned the OTHER
// instance's HWND when that client happened to be foreground at the
// moment we polled. SetWindowLongPtr cross-process is rejected, prev
// comes back NULL, Hook_WndProc never enters any subclass chain, and
// ImGui never receives key events for the wrongly-targeted instance.
struct FindOwnGameWndCtx {
    DWORD pid;
    HWND  result;
};
static BOOL CALLBACK FindOwnGameWndProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<FindOwnGameWndCtx*>(lparam);
    DWORD owner_pid = 0;
    GetWindowThreadProcessId(hwnd, &owner_pid);
    if (owner_pid != ctx->pid) return TRUE;
    char cls[32] = {0};
    if (GetClassNameA(hwnd, cls, sizeof(cls)) == 0) return TRUE;
    const char* expect_cls = FM2K::kIsFM95 ? "KGT95GAME" : "KGT2KGAME";
    if (lstrcmpA(cls, expect_cls) != 0) return TRUE;
    ctx->result = hwnd;
    return FALSE;  // stop enumeration
}
static HWND FindOwnGameWindow() {
    FindOwnGameWndCtx ctx{ GetCurrentProcessId(), nullptr };
    EnumWindows(FindOwnGameWndProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

// Returns true if `pDevice`'s back buffer carries our private-data
// tag — i.e. the same surface we already initialized the imgui DX9
// backend against. False means it's fresh (Reset rebuilt it, or
// release+CreateDevice produced a new one, possibly at the same heap
// address) and we have to rebuild ImGui's GPU resources before
// drawing or we'll sample freed memory.
static bool ImguiSwapChainStillOurs(LPDIRECT3DDEVICE9 pDevice) {
    IDirect3DSurface9* bb = nullptr;
    if (!pDevice || FAILED(pDevice->GetBackBuffer(
            0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    DWORD tag = 0;
    DWORD size = sizeof(tag);
    HRESULT hr = bb->GetPrivateData(kImguiSwapChainTagGuid, &tag, &size);
    bb->Release();
    return SUCCEEDED(hr) && tag == kImguiSwapChainTagValue;
}

// Stamp the active back-buffer surface so a future EndScene can
// recognize it. Called right after we successfully (re)init the
// backend.
static void TagImguiSwapChain(LPDIRECT3DDEVICE9 pDevice) {
    IDirect3DSurface9* bb = nullptr;
    if (!pDevice || FAILED(pDevice->GetBackBuffer(
            0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    DWORD value = kImguiSwapChainTagValue;
    bb->SetPrivateData(kImguiSwapChainTagGuid, &value, sizeof(value), 0);
    bb->Release();
}

// Whether the always-on HUD should render. Default ON; users / tests
// can suppress with FM2K_HUD_OFF=1. We don't gate on netplay-active
// so the HUD's still useful offline (shows fps, confirms the rect
// detector is working).
static bool ShouldRenderHud() {
    static int s_cached = -1;
    if (s_cached < 0) {
        const char* off = std::getenv("FM2K_HUD_OFF");
        s_cached = (off && off[0] == '1') ? 0 : 1;
    }
    return s_cached != 0;
}

// Hook implementations
HRESULT APIENTRY Hook_EndScene(LPDIRECT3DDEVICE9 pDevice) {
    // Swap-chain identity check. If the swap chain we currently hand
    // the backend isn't tagged as ours, cnc-ddraw recreated the
    // device or Reset rebuilt the chain — either way the cached GPU
    // texture/buffer pointers are now garbage. Tear the backend down
    // (full Shutdown if the device pointer also changed; just
    // Invalidate+Create otherwise) and stamp the new swap chain so
    // we don't keep firing this branch.
    if (g_imgui_initialized && !ImguiSwapChainStillOurs(pDevice)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "ImGui: swap chain changed (device %p -> %p) — rebuilding DX9 backend",
            (void*)g_imgui_device, (void*)pDevice);
        if (pDevice != g_imgui_device) {
            ImGui_ImplDX9_Shutdown();
            ImGui_ImplDX9_Init(pDevice);
            g_imgui_device = pDevice;
        } else {
            ImGui_ImplDX9_InvalidateDeviceObjects();
            ImGui_ImplDX9_CreateDeviceObjects();
        }
        TagImguiSwapChain(pDevice);
    }

    if (!g_imgui_initialized) {
        g_imgui_initialized = true;
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        // Replace ImGui's stock ProggyClean (a bitmap pixel font) with
        // Segoe UI from %WINDIR%\Fonts. Loaded at 16px as a "base" —
        // fc_hud renders most text via ImDrawList::AddText with an
        // explicit font_size that scales with the bar geometry, so the
        // base size only matters as a rasterization quality target. If
        // the font isn't available (e.g. modified Windows install), we
        // silently fall back to the default font.
        {
            io.Fonts->Clear();
            const char* segoe = "C:\\Windows\\Fonts\\segoeui.ttf";
            ImFont* f = io.Fonts->AddFontFromFileTTF(segoe, 16.0f);
            if (!f) io.Fonts->AddFontDefault();
        }

        if (!g_game_window) {
            // Process-scoped window lookup — must NOT cross processes
            // when running multiple clients on the same machine
            // (matches wndproc_subclass.cpp's class-name discovery).
            g_game_window = FindOwnGameWindow();
        }
        if (!g_game_window) {
            // No game window yet (very early init). Skip ImGui init
            // this frame; we'll retry on the next frame.
            g_imgui_initialized = false;
            ImGui::DestroyContext();
            return EndScene_orig(pDevice);
        }

        // Hook WndProc
        if (g_game_window && !oWndProc) {
            // SetWindowLongPtrW keeps the window flagged Unicode (the
            // locale-spoof wrapper promoted it at create time). Switching
            // to A here would flip the flag back and re-introduce the
            // W→A bridge that mangles JP titles via CP_ACP.
            oWndProc = (WNDPROC)SetWindowLongPtrW(g_game_window,
                GWLP_WNDPROC, (LONG_PTR)Hook_WndProc);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "imgui_overlay: Hook_WndProc=%p installed on hwnd=%p, "
                "captured prev=%p (oWndProc)",
                (void*)Hook_WndProc, (void*)g_game_window,
                (void*)oWndProc);
        }

        ImGui_ImplWin32_Init(g_game_window);
        ImGui_ImplDX9_Init(pDevice);
        g_imgui_device = pDevice;
        TagImguiSwapChain(pDevice);
        fc_hud::Init();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "ImGui initialized (device %p)", (void*)pDevice);
    }

    // Compute the cnc-ddraw game-quad rect for this frame. This
    // mirrors `dd.c:924-979` exactly so the result tracks cnc-ddraw's
    // actual placement across windowed/borderless/upscaled and
    // maintas/boxing toggles. Bypasses GetViewport (always returns
    // full backbuffer because cnc-ddraw's SetViewport is commented
    // out at render_d3d9.c:531-541).
    ComputeCncDdrawGameRect(pDevice, g_game_viewport);

    ClipCursor(nullptr);

    // Hard overlay bypass for A/B perf testing (#63). FM2K_OVERLAY_OFF=1
    // skips the ENTIRE ImGui pipeline (NewFrame + Render + RenderDrawData)
    // for this present. The in-game overlay landed in the v0.2.5x catchup
    // (~5/18) and runs unconditionally every frame; it's the prime suspect
    // for the field-wide 95fps regression. This flag lets a user confirm
    // that by toggling it -- if fps snaps back to 100 with it set, the
    // overlay is the cause. (The empty-draw-list skip below is the actual
    // always-on fix; this is the measurement tool / escape hatch.)
    static const bool s_overlay_off = []{
        const char* v = std::getenv("FM2K_OVERLAY_OFF");
        return v && v[0] == '1';
    }();
    if (s_overlay_off) return EndScene_orig(pDevice);

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Per-game patches per-frame tick — F2 hotkey for training-mode P2
    // behavior cycle, game_mode edge tracker for OPTION-mode submode
    // application, etc. No-op when no relevant mode is active.
    PerGamePatches_OnFrameTick();

    // Always-on HUD (Fightcade-style top bar inside the game rect).
    // Pushes its own data via setters from the netplay layer; here we
    // just hand it the rect and let it draw via ImDrawList. Skipped
    // when FM2K_HUD_OFF=1 or the rect isn't valid yet.
    if (ShouldRenderHud() && g_game_viewport.Width > 0 &&
        g_game_viewport.Height > 0) {
        // Sample current frame stats and push to the HUD. fps comes
        // from hooks.cpp, ping/delay from the netplay layer when a
        // session is live.
        extern int g_current_fps;
        const bool conn = Netplay_IsConnected();
        const uint32_t ping = conn ? Netplay_GetPingMs() : 0;
        const int delay = conn ? Netplay_GetLocalDelay() : 0;
        fc_hud::SetStats(g_current_fps, ping, delay);
        fc_hud::SetConnected(conn);
        // v0.2.5: HUD shipped-but-hidden by default, F9 reveals.
        // Skip the entire Render call when off so we don't pay the
        // foreground draw-list cost on every frame for a hidden HUD.
        if (g_hud_master_visible) {
            fc_hud::Render((int)g_game_viewport.X, (int)g_game_viewport.Y,
                           (int)g_game_viewport.Width,
                           (int)g_game_viewport.Height);
        }
    }

    // Optional visual: green frame around the detected rect.
    // Renders to the foreground draw list so it sits above any
    // user windows. Toggle from the overlay's UI.
    if (g_show_game_rect_debug && g_game_viewport.Width > 0 &&
        g_game_viewport.Height > 0) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 a((float)g_game_viewport.X,
                 (float)g_game_viewport.Y);
        ImVec2 b((float)(g_game_viewport.X + g_game_viewport.Width),
                 (float)(g_game_viewport.Y + g_game_viewport.Height));
        dl->AddRectFilled(a, b, IM_COL32(0, 255, 0, 28));
        dl->AddRect(a, b, IM_COL32(0, 255, 0, 220), 0.0f, 0, 2.0f);
    }

    // Per-game patch badges — small corner labels showing which
    // experimental mode is currently engaged. Always visible (no F-key
    // gate) so the user can see at a glance which mode is driving the
    // input. Renders to the foreground draw list inside the game
    // viewport. The "ALT CSS MODE = ..." status is always shown so the
    // user sees "default" when no override is engaged, and the active
    // submode otherwise.
    if (g_game_viewport.Width > 0 && g_game_viewport.Height > 0) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float pad = 6.0f;
        const float vx  = (float)g_game_viewport.X;
        const float vy  = (float)g_game_viewport.Y;
        const float vw  = (float)g_game_viewport.Width;
        float       cy  = vy + pad;  // running top-left Y for stacked badges

        auto draw_badge = [&](const char* text, ImU32 bg, ImU32 fg) {
            ImVec2 sz = ImGui::CalcTextSize(text);
            ImVec2 a(vx + vw - sz.x - pad * 3.0f, cy);
            ImVec2 b(vx + vw - pad,                cy + sz.y + pad);
            dl->AddRectFilled(a, b, bg, 4.0f);
            dl->AddText(ImVec2(a.x + pad, a.y + pad * 0.5f), fg, text);
            cy += (sz.y + pad * 2.5f);
        };

        // ALT CSS MODE status — rendered ONLY when a non-default mode is
        // engaged or queued. Default state stays visually quiet.
        //
        // Sources, in priority order:
        //   1. An active mode flag (vs_cpu / training / cpu_vs_cpu).
        //   2. On title with option_mode_selector on AND submode != 0 —
        //      the QUEUED submode (what title→CSS will engage).
        //   3. Otherwise — badge hidden.
        //
        // On title→non-title transition (i.e., the user confirms a mode
        // and CSS init begins), the queued-label badge SLIDES OUT to the
        // right over ~0.4s instead of snap-disappearing. The frozen label
        // captured at transition stays drawn for the duration; the
        // post-CSS active-mode badge appears normally once the slide
        // completes (a tiny gap where no badge shows is fine — keeps the
        // exit animation clean and avoids overlapping motion).
        {
            const uint32_t game_mode = *(const uint32_t*)0x00470054;
            const bool on_title = (game_mode == 1000u);
            const char* mode_label = nullptr;
            if      (PerGamePatches_IsVsCpuModeActive())     mode_label = "VS CPU";
            else if (PerGamePatches_IsCpuVsCpuModeActive())  mode_label = "CPU vs CPU";
            else if (PerGamePatches_IsTrainingModeActive())  mode_label = "Training";
            else if (PerGamePatches_IsOptionModeSelectorActive() && on_title) {
                const int sub = PerGamePatches_GetVsSubmode();
                if (sub != 0) {  // 0 = Default, hide badge
                    const int menu_ctx = PerGamePatches_GetVsMenuContext();
                    mode_label = PerGamePatches_VsSubmodeLabel(sub, menu_ctx);
                }
            }

            // Title-exit slide-out tracker.
            static uint32_t s_prev_game_mode_anim = 0;
            static float    s_exit_t = 1.0f;       // 1.0 = at rest (no anim)
            static char     s_exit_label[80]      = {};
            constexpr float kExitAnimDur          = 0.4f;

            // Detect 1000 → (not 1000) edge; start animation with the
            // last-known on-title label.
            if (s_prev_game_mode_anim == 1000u && game_mode != 1000u) {
                if (mode_label || s_exit_label[0]) {
                    // Prefer current frame's label; fall back to whatever
                    // was last shown on title if the current is null
                    // (e.g., transition raced the label clear).
                    const char* src = mode_label ? mode_label : s_exit_label;
                    std::snprintf(s_exit_label, sizeof(s_exit_label),
                                  "ALT CSS MODE = %s", src);
                    s_exit_t = 0.0f;
                }
            }
            s_prev_game_mode_anim = game_mode;

            // Snapshot the latest on-title label so it's available at
            // the transition edge (the current-frame mode_label might
            // already be the post-transition value).
            if (on_title && mode_label) {
                std::snprintf(s_exit_label, sizeof(s_exit_label),
                              "ALT CSS MODE = %s", mode_label);
            }

            // Advance animation.
            if (s_exit_t < 1.0f) {
                s_exit_t += ImGui::GetIO().DeltaTime / kExitAnimDur;
                if (s_exit_t > 1.0f) s_exit_t = 1.0f;
            }

            // Render the exit slide if active. Uses ease-out cubic so
            // motion accelerates away quickly then settles, matching a
            // "swipe off" feel.
            if (s_exit_t < 1.0f && s_exit_label[0]) {
                const float t      = s_exit_t;
                const float inv    = 1.0f - t;
                const float eased  = 1.0f - inv * inv * inv;  // ease-out cubic
                ImVec2  sz = ImGui::CalcTextSize(s_exit_label);
                const float bw   = sz.x + pad * 3.0f;
                const float bh   = sz.y + pad * 2.0f;
                const float dx   = eased * (bw + pad * 2.0f);
                const float left = vx + vw - sz.x - pad * 3.0f + dx;
                const float top  = cy;
                ImVec2 a(left, top);
                ImVec2 b(left + bw, top + bh);
                dl->AddRectFilled(a, b, IM_COL32(30, 100, 60, 220), 4.0f);
                dl->AddText(ImVec2(a.x + pad, a.y + pad * 0.5f),
                            IM_COL32(220, 255, 220, 255), s_exit_label);
                cy += (sz.y + pad * 2.5f);
            } else if (mode_label) {
                // Steady-state badge — render once exit-anim is done OR
                // when we're on title and have a queued label.
                char buf[80];
                std::snprintf(buf, sizeof(buf), "ALT CSS MODE = %s", mode_label);
                draw_badge(buf, IM_COL32(30, 100, 60, 220),
                           IM_COL32(220, 255, 220, 255));
            }
        }

        // Training mode P2 behavior — shown below the ALT CSS MODE status
        // when training is active so the user sees what dummy is doing.
        if (PerGamePatches_IsTrainingModeActive()) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Training [F2]: %s",
                PerGamePatches_TrainingP2BehaviorLabel(
                    PerGamePatches_GetTrainingP2Behavior()));
            draw_badge(buf, IM_COL32(40, 100, 200, 200), IM_COL32(255, 255, 255, 255));
        }
    }

    // Debug overlay window — F9-gated, drag-around, tabs etc. The
    // HUD above is always-on; this is the developer surface.
    if (g_overlay_visible) {
        RenderDebugOverlay();
    }

    ImGui::EndFrame();
    ImGui::Render();
    // Skip the D3D9 draw submission when the frame produced no geometry.
    // ImGui_ImplDX9_RenderDrawData saves and restores the entire device
    // state (state block + render/sampler/transform state) on EVERY call --
    // ~0.3-0.5ms on many drivers -- which is pure waste when the HUD,
    // overlay, badges and chat are all idle (the default for a normal
    // match). That always-on cost, added with the in-game overlay in the
    // v0.2.5x catchup, is what dropped slower machines from 100 to ~95fps
    // (#63). When anything IS visible, TotalVtxCount > 0 and it renders
    // normally, so this is safe with no per-element visibility predicate.
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data && draw_data->TotalVtxCount > 0) {
        ImGui_ImplDX9_RenderDrawData(draw_data);
    }

    return EndScene_orig(pDevice);
}

HRESULT APIENTRY Hook_Reset(LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters) {
    // Reset path: cnc-ddraw keeps the device alive but invalidates all
    // D3DPOOL_DEFAULT resources. ImGui's DX9 backend allocates its
    // vertex/index buffers + font atlas in DEFAULT pool, so they have
    // to be released before Reset and recreated after. Skipping this
    // = c0000005 on the next RenderDrawData (the original symptom).
    if (g_imgui_initialized && pDevice == g_imgui_device) {
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }
    HRESULT hr = Reset_orig(pDevice, pPresentationParameters);
    if (g_imgui_initialized && pDevice == g_imgui_device && SUCCEEDED(hr)) {
        ImGui_ImplDX9_CreateDeviceObjects();
        // Reset rebuilds the swap chain. Re-tag the new one so the
        // EndScene swap-chain check above doesn't fire and double-
        // rebuild on the next frame.
        TagImguiSwapChain(pDevice);
    }
    return hr;
}

LRESULT WINAPI Hook_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Tab is OUR chat-toggle key — swallow it at this layer so:
    //   - The game's WndProc / DefWindowProc don't see it (Tab would
    //     otherwise trigger focus-traversal in default DefWindowProc,
    //     which is harmless for FM2K but pointless).
    //   - ImGui doesn't see it either. Default Tab in a focused
    //     InputText is "focus next/prev item"; with our single-item
    //     chat window that immediately defocuses InputText, which is
    //     why earlier builds could open chat but couldn't type into
    //     it. Eating Tab before ImGui's input layer keeps the
    //     InputText's focus stable.
    //
    // fc_hud's GetKeyState polling handles the open/close toggle on
    // its own render-tick cadence, so we don't need ImGui or the game
    // to see Tab at all. Covers WM_KEYDOWN, WM_KEYUP, and WM_CHAR
    // (Tab generates 0x09 as a typed character too).
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR) &&
        wParam == VK_TAB) {
        return 0;
    }

    // Always forward everything else so ImGui's key state stays
    // coherent — the previous "gate forwarding on chat-active" mode
    // dropped KEYUP events when chat closed mid-press, leaving keys
    // stuck "down" in ImGui state and breaking subsequent reopens.
    // ImGui_ImplWin32_WndProcHandler returns 1 only when a focused
    // widget actually consumes the input (WantCaptureKeyboard etc.),
    // so passing it to the game's WndProc happens iff ImGui doesn't
    // care.
    if (g_imgui_initialized) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
            return true;
        }
    }
    return CallWindowProcW(oWndProc, hWnd, msg, wParam, lParam);
}

DWORD WINAPI DirectXInit(LPVOID lpParameter) {
    // Wait for d3d9.dll
    int attempts = 0;
    while (!GetModuleHandleA("d3d9.dll") && attempts < 50) {
        Sleep(100);
        attempts++;
    }

    if (!GetModuleHandleA("d3d9.dll")) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "d3d9.dll not found");
        return 0;
    }

    // Create temp window
    HWND tmpWnd = CreateWindowA("BUTTON", "TempD3D", WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 300, 300,
                                nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!tmpWnd) return 0;

    LPDIRECT3D9 d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        DestroyWindow(tmpWnd);
        return 0;
    }

    D3DPRESENT_PARAMETERS d3dpp = {};
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.hDeviceWindow = tmpWnd;
    d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;

    LPDIRECT3DDEVICE9 d3ddev = nullptr;
    if (d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, tmpWnd,
                          D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &d3ddev) != D3D_OK) {
        d3d->Release();
        DestroyWindow(tmpWnd);
        return 0;
    }

    DWORD* vtable = *(DWORD**)d3ddev;

    // Hook EndScene and Reset
    MH_CreateHook((void*)vtable[42], (void*)Hook_EndScene, (void**)&EndScene_orig);
    MH_EnableHook((void*)vtable[42]);
    MH_CreateHook((void*)vtable[16], (void*)Hook_Reset, (void**)&Reset_orig);
    MH_EnableHook((void*)vtable[16]);

    d3ddev->Release();
    d3d->Release();
    DestroyWindow(tmpWnd);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "D3D9 hooks installed - Press F9 to toggle overlay + in-game HUD");
    return 1;
}

bool InitializeImGuiOverlay() {
    // FM2K_NO_OVERLAY=1 skips our D3D9 EndScene/Reset vtable hook entirely
    // (diagnostic / opt-out). Overlay was ruled out as the SJIS F4 cause.
    if (const char* off = std::getenv("FM2K_NO_OVERLAY"); off && off[0] == '1') {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "imgui_overlay: DISABLED via FM2K_NO_OVERLAY=1 (D3D9 hook not installed)");
        return true;
    }
    HANDLE thread = CreateThread(nullptr, 0, DirectXInit, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
        return true;
    }
    return false;
}

void ShutdownImGuiOverlay() {
    if (g_imgui_initialized) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imgui_initialized = false;
    }
    if (g_game_window && oWndProc) {
        SetWindowLongPtrW(g_game_window, GWLP_WNDPROC, (LONG_PTR)oWndProc);
    }
}

bool IsOverlayVisible() { return g_overlay_visible; }

void ToggleOverlay() {
    g_overlay_visible    = !g_overlay_visible;
    // Mirror the dev overlay state onto the in-game HUD master flag
    // so a single F9 press reveals both (and a second press hides
    // both). Default is "everything off" so a fresh launch never
    // shows the in-game HUD until the user explicitly opts in.
    g_hud_master_visible = g_overlay_visible;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Overlay %s (in-game HUD %s)",
                g_overlay_visible    ? "shown" : "hidden",
                g_hud_master_visible ? "shown" : "hidden");
}

void CheckOverlayHotkey() {
    // Lazy-install d3d9 hooks on first call. Used to wait for F9
    // press, but the always-on HUD needs the hooks up at the first
    // frame after the game starts rendering — we have no way to
    // draw otherwise. InitializeImGuiOverlay spawns a worker thread
    // that polls for d3d9.dll, so it's safe to call before the
    // game's d3d9 device exists.
    if (!g_hooks_installed) {
        if (InitializeImGuiOverlay()) {
            g_hooks_installed = true;
        }
    }

    // Hotkeys read via GetKeyState — synchronized to this (window)
    // thread's message queue, so an F9/F10 typed into ANY other window
    // never registers here. GetAsyncKeyState is banned: it reads the
    // physical keyboard desktop-wide, and used to toggle the overlay /
    // fire a savestate roundtrip in the background game.

    // F9 - Toggle the developer debug overlay window. The HUD
    // itself stays always-on regardless of this toggle.
    bool f9_current = (GetKeyState(VK_F9) & 0x8000) != 0;
    if (f9_current && !g_f9_key_pressed) {
        ToggleOverlay();
    }
    g_f9_key_pressed = f9_current;

    // F10 - Run savestate roundtrip test
    bool f10_current = (GetKeyState(VK_F10) & 0x8000) != 0;
    if (f10_current && !g_f10_key_pressed) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "F10 pressed - running roundtrip test");
        g_last_snapshot_before = SaveState_CaptureSnapshot();
        g_last_test_passed = SaveState_TestRoundtrip();
        g_last_snapshot_after = SaveState_CaptureSnapshot();
        SaveState_CompareSnapshots(g_last_snapshot_before, g_last_snapshot_after,
            g_last_test_diff, sizeof(g_last_test_diff));
        g_last_test_ran = true;
    }
    g_f10_key_pressed = f10_current;
}
