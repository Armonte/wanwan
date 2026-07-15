// main.cpp -- 2dfm Studio shell: SDL3 window + ImGui docking + panels.
// Panels bind to StudioModel (app_state.h); the data behind it is
// RealModel (real_model.h) over kgtcore. Layout per the design sketch:
// sounds table left, detail right-top, usage right-bottom.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_internal.h"  // DockBuilder

#include "app_state.h"
#include "audio_player.h"
#include "real_model.h"

namespace {

// May fire off-thread (SDL_dialog.h) -- stash under the lock, main loop consumes.
void OpenFileCb(void* ud, const char* const* files, int /*filter*/) {
    auto* st = static_cast<studio::AppState*>(ud);
    if (files && files[0]) {
        std::lock_guard<std::mutex> lk(st->dlg_mutex);
        st->pending_open = files[0];
    }
}

// Filter storage must outlive the async dialog (SDL keeps the pointer).
constexpr SDL_DialogFileFilter kKgtFilters[] = {
    {"2dfm files (.player/.stage/.demo/.kgt)", "player;stage;demo;kgt"},
    {"All files", "*"},
};

void DrawMainMenu(studio::AppState& st) {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open...", "Ctrl+O")) {
            SDL_ShowOpenFileDialog(OpenFileCb, &st, st.window, kKgtFilters,
                                   int(SDL_arraysize(kKgtFilters)), nullptr, false);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4")) st.quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        ImGui::MenuItem("Undo", "Ctrl+Z", false, false);  // write milestone
        ImGui::MenuItem("Redo", "Ctrl+Y", false, false);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        ImGui::MenuItem("Export all sounds...", nullptr, false, false);  // later
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

// Dockspace over the whole viewport; build the default split layout once
// (skipped when imgui.ini already restored one).
void DrawDockspace() {
    ImGuiID dock = ImGui::DockSpaceOverViewport(ImGui::GetID("StudioDock"),
                                                ImGui::GetMainViewport());
    static bool once = false;
    if (once) return;
    once = true;
    ImGuiDockNode* node = ImGui::DockBuilderGetNode(dock);
    if (node && node->IsSplitNode()) return;  // saved layout restored
    ImGui::DockBuilderRemoveNodeChildNodes(dock);
    ImGuiID left = 0, right = 0, rtop = 0, rbot = 0;
    ImGui::DockBuilderSplitNode(dock, ImGuiDir_Left, 0.34f, &left, &right);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.42f, &rbot, &rtop);
    ImGui::DockBuilderDockWindow("Sounds", left);
    ImGui::DockBuilderDockWindow("Sound", rtop);
    ImGui::DockBuilderDockWindow("Used by", rbot);
    ImGui::DockBuilderFinish(dock);
}

// "Open failed" modal while st.load_error is set.
void DrawLoadErrorModal(studio::AppState& st) {
    if (st.load_error.empty()) return;
    if (!ImGui::IsPopupOpen("Open failed")) ImGui::OpenPopup("Open failed");
    if (ImGui::BeginPopupModal("Open failed", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", st.load_error.c_str());
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120.f, 0.f))) {
            st.load_error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// Load `path` into the model, resetting selection/playback and the title
// bar (path + counts) on success, raising the error modal on failure.
void LoadFile(studio::AppState& st, studio::RealModel& model,
              studio::AudioPlayer& player, const std::string& path) {
    player.Stop();
    st.playing = false;
    std::string err;
    if (!model.Load(path, &err)) {
        st.load_error = path + "\n\n" + err;
        return;
    }
    st.opened_path = path;
    st.selected = model.Sounds().empty() ? -1 : 0;
    st.replace_path.clear();
    std::string title =
        "2dfm Studio -- " + path + " (" + model.CountsLabel() + ")";
    SDL_SetWindowTitle(st.window, title.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    // Audio is best-effort: without a device the app still browses files
    // (AudioPlayer::Play just fails and the play button does nothing).
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "audio unavailable: %s", SDL_GetError());
    SDL_Window* window = SDL_CreateWindow("2dfm Studio", 1280, 760,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL_CreateRenderer failed: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    studio::RealModel model;
    studio::AudioPlayer player;
    studio::AppState st;
    st.model = &model;
    st.player = &player;
    st.window = window;

    // Quick-test path: `2dfm-studio.exe Bewear.player` loads on startup.
    if (argc > 1 && argv[1] && argv[1][0])
        LoadFile(st, model, player, argv[1]);

    while (!st.quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT) st.quit = true;
        }

        // Consume async dialog results on the main thread (the load itself
        // runs outside the lock -- the lock only guards the pending slots).
        std::string open_req;
        {
            std::lock_guard<std::mutex> lk(st.dlg_mutex);
            open_req.swap(st.pending_open);
            if (!st.pending_replace.empty()) {
                st.replace_path = std::move(st.pending_replace);
                st.pending_replace.clear();
            }
        }
        if (!open_req.empty()) LoadFile(st, model, player, open_req);

        // Panels only flip st.playing (e.g. the table on selection change);
        // keep the device in sync.
        if (!st.playing && player.Playing()) player.Stop();

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        DrawMainMenu(st);
        DrawDockspace();
        studio::DrawSoundTable(st);
        studio::DrawSoundDetail(st);
        studio::DrawUsagePanel(st);
        DrawLoadErrorModal(st);

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 18, 18, 22, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    player.Stop();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
