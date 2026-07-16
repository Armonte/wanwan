// sound_table.cpp -- left panel: the sound table.
// Columns per the MVP list: index, name, type, format, size, used-by
// count, modified marker, validity flag ('!' yellow = WARN, red = FATAL,
// tooltip carries the probe messages). Row click selects for the
// detail/usage panels.
#include "imgui.h"

#include "app_state.h"

namespace studio {

std::string TypeLabel(uint8_t t) {
    const char* base = "?";
    switch (t & 0x0F) {
        case 0: base = "stop"; break;
        case 1: base = "WAV"; break;
        case 2: base = "MIDI"; break;
        case 3: base = "CD"; break;
    }
    std::string s = base;
    if (t & 0x10) s += " loop";
    return s;
}

std::string SizeLabel(size_t n) {
    std::string s = std::to_string(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
    return s + " B";
}

void DrawSoundTable(AppState& st) {
    if (!ImGui::Begin("Sounds")) {
        ImGui::End();
        return;
    }
    const auto& sounds = st.model->Sounds();
    ImGui::TextDisabled("%d sounds  (* = modified, ! = validity)",
                        int(sounds.size()));
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("sound_table", 8, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.f);
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, 64.f);
        ImGui::TableSetupColumn("format", ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, 78.f);
        ImGui::TableSetupColumn("uses", ImGuiTableColumnFlags_WidthFixed, 38.f);
        ImGui::TableSetupColumn("*", ImGuiTableColumnFlags_WidthFixed, 18.f);
        ImGui::TableSetupColumn("!", ImGuiTableColumnFlags_WidthFixed, 18.f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(sounds.size()); ++i) {
            const SoundRow& s = sounds[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char idx[16];
            snprintf(idx, sizeof idx, "%d", i);
            if (ImGui::Selectable(idx, st.selected == i,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                if (st.selected != i) st.playing = false;
                st.selected = i;
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(TypeLabel(s.sound_type).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.fmt.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(SizeLabel(s.size).c_str());
            ImGui::TableNextColumn();
            if (s.use_count > 0)
                ImGui::Text("%d", s.use_count);
            else
                ImGui::TextDisabled("0");
            ImGui::TableNextColumn();
            if (s.modified) {
                ImGui::TextColored(ImVec4(.55f, .85f, 1.f, 1.f), "*");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip("modified (unsaved)");
            }
            ImGui::TableNextColumn();
            if (s.validity != Validity::Ok) {
                const ImVec4 col = s.validity == Validity::Fatal
                                       ? ImVec4(1.f, .35f, .3f, 1.f)    // red
                                       : ImVec4(1.f, .85f, .35f, 1.f);  // yellow
                ImGui::TextColored(col, "!");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) &&
                    !s.validity_msg.empty()) {
                    ImGui::SetNextWindowSize(ImVec2(420.f, 0.f), ImGuiCond_Always);
                    ImGui::BeginTooltip();
                    ImGui::TextWrapped("%s", s.validity_msg.c_str());
                    ImGui::EndTooltip();
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

}  // namespace studio
