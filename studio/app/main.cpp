// main.cpp -- 2dfm Studio shell: SDL3 window + ImGui docking + panels.
// Panels bind to StudioModel (app_state.h); the data behind it is
// RealModel (real_model.h) over kgtcore + EditSession (the write path).
// Layout per the design sketch: sounds table left, detail right-top,
// usage right-bottom.
//
// File-safety workflow wiring (docs/dev/2dfm_studio_design.md Phase 4):
// Save As Copy is the primary save (SDL_ShowSaveFileDialog, default
// `<stem>_edited.<ext>`); Save (overwrite) sits behind a confirmation
// modal naming the full path; Open/Quit with unsaved changes raise a
// Save Copy / Discard / Cancel confirm; browse-only files (round-trip
// gate failed) get a menu-bar banner and all editing disabled.
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

// Cancel matters here: an abandoned save dialog must also abandon a
// pending "save then open/quit" continuation.
void SaveFileCb(void* ud, const char* const* files, int /*filter*/) {
    auto* st = static_cast<studio::AppState*>(ud);
    std::lock_guard<std::mutex> lk(st->dlg_mutex);
    if (files && files[0])
        st->pending_save_as = files[0];
    else
        st->pending_save_cancel = true;
}

// Filter storage must outlive the async dialog (SDL keeps the pointer).
constexpr SDL_DialogFileFilter kKgtFilters[] = {
    {"2dfm files (.player/.stage/.demo/.kgt)", "player;stage;demo;kgt"},
    {"All files", "*"},
};

std::string LowerExt(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos ||
        (slash != std::string::npos && dot < slash))
        return std::string();
    std::string ext = path.substr(dot + 1);
    for (char& c : ext)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return ext;
}

bool IsKgtExt(const std::string& ext) {
    return ext == "player" || ext == "stage" || ext == "demo" || ext == "kgt";
}

bool IsAudioExt(const std::string& ext) {
    return ext == "wav" || ext == "mp3" || ext == "ogg" || ext == "flac" ||
           ext == "mid";
}

// `<dir>/<stem>_edited.<ext>` -- the contract's default copy name.
std::string DefaultCopyPath(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos ||
        (slash != std::string::npos && dot < slash))
        return path + "_edited";
    return path.substr(0, dot) + "_edited" + path.substr(dot);
}

void ShowOpenDialog(studio::AppState& st) {
    SDL_ShowOpenFileDialog(OpenFileCb, &st, st.window, kKgtFilters,
                           int(SDL_arraysize(kKgtFilters)), nullptr, false);
}

void ShowSaveCopyDialog(studio::AppState& st, studio::RealModel& model) {
    const std::string def = DefaultCopyPath(model.Path());
    SDL_ShowSaveFileDialog(SaveFileCb, &st, st.window, kKgtFilters,
                           int(SDL_arraysize(kKgtFilters)), def.c_str());
}

// Load `path` into the model, resetting selection/playback and every
// write-path state (a new file voids pending plans), raising the error
// modal on failure.
void LoadFile(studio::AppState& st, studio::RealModel& model,
              studio::AudioPlayer& player, const std::string& path) {
    player.Stop();
    st.playing = false;
    std::string err;
    if (!model.Load(path, &err)) {
        st.load_error = path + "\n\n" + err;
        return;
    }
    st.selected = model.Sounds().empty() ? -1 : 0;
    st.replace_slot = -1;
    st.replace_modal_open = false;
    st.replace_plan = studio::ReplacePlan{};
    st.replace_error.clear();
    st.confirm_overwrite = false;
}

// Run the action a dirty-confirm (or a clean state) has released.
void ExecuteAction(studio::AppState& st, studio::RealModel& model,
                   studio::AudioPlayer& player,
                   studio::AppState::AfterConfirm act,
                   const std::string& path) {
    using AfterConfirm = studio::AppState::AfterConfirm;
    switch (act) {
        case AfterConfirm::OpenDialog: ShowOpenDialog(st); break;
        case AfterConfirm::OpenPath: LoadFile(st, model, player, path); break;
        case AfterConfirm::Quit: st.quit = true; break;
        case AfterConfirm::None: break;
    }
}

// Dirty gate in front of Open/Quit: clean files act immediately, dirty
// ones raise the Save Copy / Discard / Cancel confirm.
void RequestGuarded(studio::AppState& st, studio::RealModel& model,
                    studio::AudioPlayer& player,
                    studio::AppState::AfterConfirm act,
                    std::string path = std::string()) {
    if (model.Loaded() && model.Dirty()) {
        st.after_confirm = act;
        st.after_confirm_path = std::move(path);
        st.confirm_unsaved = true;
    } else {
        ExecuteAction(st, model, player, act, path);
    }
}

void DoUndo(studio::AppState& st, studio::RealModel& model,
            studio::AudioPlayer& player) {
    const int slot = model.Undo();
    if (slot < 0) return;
    player.Stop();
    st.playing = false;
    st.selected = slot;  // show what changed
}

void DoRedo(studio::AppState& st, studio::RealModel& model,
            studio::AudioPlayer& player) {
    const int slot = model.Redo();
    if (slot < 0) return;
    player.Stop();
    st.playing = false;
    st.selected = slot;
}

void DrawMainMenu(studio::AppState& st, studio::RealModel& model,
                  studio::AudioPlayer& player) {
    using AfterConfirm = studio::AppState::AfterConfirm;
    if (!ImGui::BeginMainMenuBar()) return;
    const bool editable = model.Editable();
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open...", "Ctrl+O"))
            RequestGuarded(st, model, player, AfterConfirm::OpenDialog);
        ImGui::Separator();
        // Save As Copy is the PRIMARY save (workflow item 3).
        if (ImGui::MenuItem("Save As Copy...", "Ctrl+S", false,
                            model.Loaded() && editable))
            ShowSaveCopyDialog(st, model);
        if (ImGui::MenuItem("Save (overwrite)", nullptr, false,
                            model.Loaded() && editable && model.Dirty()))
            st.confirm_overwrite = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4"))
            RequestGuarded(st, model, player, AfterConfirm::Quit);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, model.CanUndo()))
            DoUndo(st, model, player);
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, model.CanRedo()))
            DoRedo(st, model, player);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        ImGui::MenuItem("Export all sounds...", nullptr, false, false);  // later
        ImGui::EndMenu();
    }
    if (model.Loaded() && !editable) {
        // Browse-only banner (workflow item 2): visible on every frame.
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, .85f, .35f, 1.f),
                           "BROWSE-ONLY: file does not round-trip byte-exact; "
                           "editing disabled");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip(
                "The parser could not reproduce this file byte-for-byte, so "
                "editing is disabled to protect it.\nPlease report the file "
                "so the parser can be fixed.");
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

// "Save failed" modal while st.save_error is set.
void DrawSaveErrorModal(studio::AppState& st) {
    if (st.save_error.empty()) return;
    if (!ImGui::IsPopupOpen("Save failed")) ImGui::OpenPopup("Save failed");
    if (ImGui::BeginPopupModal("Save failed", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(460.f);
        ImGui::TextWrapped("%s", st.save_error.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120.f, 0.f))) {
            st.save_error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// "Save (overwrite)" confirmation, naming the full path (workflow item 3).
void DrawOverwriteConfirmModal(studio::AppState& st, studio::RealModel& model) {
    if (!st.confirm_overwrite) return;
    if (!ImGui::IsPopupOpen("Overwrite original?"))
        ImGui::OpenPopup("Overwrite original?");
    if (!ImGui::BeginPopupModal("Overwrite original?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextWrapped("This will overwrite:");
    ImGui::TextColored(ImVec4(1.f, .85f, .35f, 1.f), "%s",
                       model.Path().c_str());
    ImGui::TextWrapped(
        "A .bak of the original is kept (created once, never rewritten). "
        "The write is atomic and self-verified before the original is "
        "replaced.");
    ImGui::Spacing();
    if (ImGui::Button("Overwrite", ImVec2(140.f, 0.f))) {
        st.confirm_overwrite = false;
        ImGui::CloseCurrentPopup();
        std::string err;
        if (!model.SaveOverwrite(&err)) st.save_error = err;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(140.f, 0.f))) {
        st.confirm_overwrite = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Unsaved-changes confirm on Open/Quit: Save Copy / Discard / Cancel
// (workflow item 6).
void DrawUnsavedConfirmModal(studio::AppState& st, studio::RealModel& model,
                             studio::AudioPlayer& player) {
    using AfterConfirm = studio::AppState::AfterConfirm;
    if (!st.confirm_unsaved) return;
    if (!ImGui::IsPopupOpen("Unsaved changes"))
        ImGui::OpenPopup("Unsaved changes");
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextWrapped("%s has unsaved changes.", model.Path().c_str());
    ImGui::Spacing();
    if (ImGui::Button("Save Copy...", ImVec2(130.f, 0.f))) {
        st.confirm_unsaved = false;
        st.save_as_continues = true;  // continue Open/Quit once saved
        ImGui::CloseCurrentPopup();
        ShowSaveCopyDialog(st, model);
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(130.f, 0.f))) {
        st.confirm_unsaved = false;
        const AfterConfirm act = st.after_confirm;
        const std::string path = st.after_confirm_path;
        st.after_confirm = AfterConfirm::None;
        st.after_confirm_path.clear();
        ImGui::CloseCurrentPopup();
        ExecuteAction(st, model, player, act, path);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(130.f, 0.f))) {
        st.confirm_unsaved = false;
        st.after_confirm = AfterConfirm::None;
        st.after_confirm_path.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Global shortcuts (skipped while a text field wants the keyboard).
void HandleShortcuts(studio::AppState& st, studio::RealModel& model,
                     studio::AudioPlayer& player) {
    using AfterConfirm = studio::AppState::AfterConfirm;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || !io.KeyCtrl) return;
    if (ImGui::IsKeyPressed(ImGuiKey_O, false))
        RequestGuarded(st, model, player, AfterConfirm::OpenDialog);
    if (ImGui::IsKeyPressed(ImGuiKey_S, false) &&
        model.Loaded() && model.Editable())
        ShowSaveCopyDialog(st, model);
    if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) DoUndo(st, model, player);
    if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) DoRedo(st, model, player);
}

// A file landed on the window: 2dfm files open (dirty-gated), audio files
// replace the selected slot through the normal plan/confirm pipeline.
void HandleDropFile(studio::AppState& st, studio::RealModel& model,
                    studio::AudioPlayer& player, const char* dropped) {
    using AfterConfirm = studio::AppState::AfterConfirm;
    if (!dropped || !dropped[0]) return;
    const std::string path = dropped;
    const std::string ext = LowerExt(path);
    if (IsKgtExt(ext)) {
        RequestGuarded(st, model, player, AfterConfirm::OpenPath, path);
        return;
    }
    if (IsAudioExt(ext)) {
        if (!model.Loaded() || !model.Editable() || st.selected < 0 ||
            st.selected >= int(model.Sounds().size())) {
            st.replace_error =
                "drop target: select a sound slot in an editable file first";
            return;
        }
        std::vector<uint8_t> bytes;
        std::string err;
        if (!studio::ReadFileBytes(path, &bytes, &err)) {
            st.replace_error = err;
            return;
        }
        st.replace_slot = st.selected;
        studio::StartReplace(st, st.selected, bytes.data(), bytes.size());
        return;
    }
    st.replace_error = "unsupported file type: " + path;
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
    st.real = &model;
    st.player = &player;
    st.window = window;
    st.renderer = renderer;  // usage-panel sprite thumbnails

    // Quick-test path: `2dfm-studio.exe Bewear.player` loads on startup.
    if (argc > 1 && argv[1] && argv[1][0])
        LoadFile(st, model, player, argv[1]);

    std::string last_title;
    while (!st.quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT)  // dirty-gated like the menu Quit
                RequestGuarded(st, model, player,
                               studio::AppState::AfterConfirm::Quit);
            else if (ev.type == SDL_EVENT_DROP_FILE)
                HandleDropFile(st, model, player, ev.drop.data);
        }

        // Consume async dialog results on the main thread (the work runs
        // outside the lock -- the lock only guards the pending slots).
        std::string open_req, replace_req, save_req;
        bool save_cancel = false;
        {
            std::lock_guard<std::mutex> lk(st.dlg_mutex);
            open_req.swap(st.pending_open);
            replace_req.swap(st.pending_replace);
            save_req.swap(st.pending_save_as);
            save_cancel = st.pending_save_cancel;
            st.pending_save_cancel = false;
        }
        if (!open_req.empty()) LoadFile(st, model, player, open_req);
        if (!replace_req.empty()) {
            std::vector<uint8_t> bytes;
            std::string err;
            if (!studio::ReadFileBytes(replace_req, &bytes, &err))
                st.replace_error = err;
            else
                studio::StartReplace(st, st.replace_slot, bytes.data(),
                                     bytes.size());
        }
        if (save_cancel) {  // abandoned save dialog abandons the follow-up
            st.save_as_continues = false;
            st.after_confirm = studio::AppState::AfterConfirm::None;
            st.after_confirm_path.clear();
        }
        if (!save_req.empty()) {
            std::string err;
            if (model.SaveCopy(save_req, &err)) {
                if (st.save_as_continues) {
                    const auto act = st.after_confirm;
                    const std::string path = st.after_confirm_path;
                    st.save_as_continues = false;
                    st.after_confirm = studio::AppState::AfterConfirm::None;
                    st.after_confirm_path.clear();
                    ExecuteAction(st, model, player, act, path);
                }
            } else {
                st.save_error = err;
                st.save_as_continues = false;
                st.after_confirm = studio::AppState::AfterConfirm::None;
                st.after_confirm_path.clear();
            }
        }

        // Panels only flip st.playing (e.g. the table on selection change);
        // keep the device in sync.
        if (!st.playing && player.Playing()) player.Stop();

        // Title bar: path, dirty '*', counts, browse-only tag (item 6).
        std::string title = "2dfm Studio";
        if (model.Loaded()) {
            title += " -- " + model.Path();
            if (model.Dirty()) title += " *";
            title += " (" + model.CountsLabel() + ")";
            if (!model.Editable()) title += "  [BROWSE-ONLY]";
        }
        if (title != last_title) {
            SDL_SetWindowTitle(window, title.c_str());
            last_title = title;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        HandleShortcuts(st, model, player);
        DrawMainMenu(st, model, player);
        DrawDockspace();
        studio::DrawSoundTable(st);
        studio::DrawSoundDetail(st);
        studio::DrawUsagePanel(st);
        studio::DrawReplaceModals(st);
        DrawUnsavedConfirmModal(st, model, player);
        DrawOverwriteConfirmModal(st, model);
        DrawLoadErrorModal(st);
        DrawSaveErrorModal(st);

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
