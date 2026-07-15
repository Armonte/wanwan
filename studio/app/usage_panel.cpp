// usage_panel.cpp -- right-bottom panel: where the selected sound plays.
// One row per xref site: "'action' @tick N  sprite S" ('~tick' when a
// jump op made the tick an estimate). Click = no-op for now; later it
// opens the action's timeline strip + sprite thumbnail.
#include "imgui.h"

#include "app_state.h"

namespace studio {

void DrawUsagePanel(AppState& st) {
    if (!ImGui::Begin("Used by")) {
        ImGui::End();
        return;
    }
    const auto& sounds = st.model->Sounds();
    if (st.selected < 0 || st.selected >= int(sounds.size())) {
        ImGui::TextDisabled("no sound selected");
        ImGui::End();
        return;
    }
    const auto& uses = st.model->Uses(st.selected);
    if (uses.empty()) {
        ImGui::TextDisabled("unused -- safe to replace or repurpose");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("%d use(s)  (click -> action timeline, later)", int(uses.size()));
    ImGui::Separator();
    for (int i = 0; i < int(uses.size()); ++i) {
        const UseRow& u = uses[i];
        char label[192];
        if (u.sprite >= 0)
            snprintf(label, sizeof label, "'%s' @%stick %d  sprite %d##u%d",
                     u.action.c_str(), u.tick_estimated ? "~" : "", u.tick, u.sprite, i);
        else
            snprintf(label, sizeof label, "'%s' @%stick %d##u%d",
                     u.action.c_str(), u.tick_estimated ? "~" : "", u.tick, i);
        if (ImGui::Selectable(label)) {
            // no-op: timeline strip / sprite thumbnail comes with kgtcore
        }
    }
    ImGui::End();
}

}  // namespace studio
