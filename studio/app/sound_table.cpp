// sound_table.cpp -- left panel: the sound table.
// Columns per the MVP list: index, name, type, format, size, used-by
// count, modified marker, validity flag (red '!' = FATAL, the walk breaks
// in-game; dim yellow 'i' = informational format note -- plays on modern
// Windows, see ProbeWav; tooltip carries the probe messages). Row click
// selects for the detail/usage panels.
//
// Sorting is a VIEW permutation only: rows render through `order`, but
// st.selected / replace_slot always hold the real sound-table index, so
// the detail panel, xref, and the write path are sort-agnostic.
#include <algorithm>
#include <cctype>

#include "imgui.h"

#include "app_state.h"

namespace studio {

namespace {

enum SoundCol : ImGuiID {
    kColIndex,
    kColName,
    kColType,
    kColFmt,
    kColSize,
    kColUses,
    kColMod,
    kColValid,
};

// Case-insensitive (ASCII fold; names are UTF-8 from CP932 -- multibyte
// sequences compare bytewise, which keeps JP names grouped consistently).
int NameCmp(const std::string& a, const std::string& b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = (unsigned char)std::tolower((unsigned char)a[i]);
        unsigned char cb = (unsigned char)std::tolower((unsigned char)b[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

template <typename T>
int Cmp(T a, T b) {
    return a < b ? -1 : (b < a ? 1 : 0);
}

int CompareRows(const SoundRow& a, const SoundRow& b, ImGuiID col) {
    switch (col) {
        case kColName: return NameCmp(a.name, b.name);
        case kColType: return Cmp(a.sound_type, b.sound_type);
        case kColFmt: return Cmp(a.fmt.compare(b.fmt), 0);
        case kColSize: return Cmp(a.size, b.size);
        case kColUses: return Cmp(a.use_count, b.use_count);
        case kColMod: return Cmp(int(a.modified), int(b.modified));
        case kColValid: return Cmp(int(a.validity), int(b.validity));
        default: return 0;  // kColIndex: caller compares indices
    }
}

// Recomputed every frame (rows are a few hundred at most) so the order
// stays live when values change under a sorted column -- e.g. a replace
// bumps size/format/validity, save clears '*' -- without invalidation
// plumbing.
void SortOrder(const std::vector<SoundRow>& sounds, std::vector<int>& order) {
    order.resize(sounds.size());
    for (int i = 0; i < int(order.size()); ++i) order[i] = i;
    ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
    if (!specs || specs->SpecsCount == 0) return;  // tristate off = file order
    std::stable_sort(order.begin(), order.end(), [&](int ia, int ib) {
        for (int s = 0; s < specs->SpecsCount; ++s) {
            const ImGuiTableColumnSortSpecs& sp = specs->Specs[s];
            int c = sp.ColumnUserID == kColIndex
                        ? Cmp(ia, ib)
                        : CompareRows(sounds[size_t(ia)], sounds[size_t(ib)],
                                      sp.ColumnUserID);
            if (c) return sp.SortDirection == ImGuiSortDirection_Ascending ? c < 0
                                                                           : c > 0;
        }
        return ia < ib;  // stable tie-break: file order
    });
    specs->SpecsDirty = false;
}

}  // namespace

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
    ImGui::TextDisabled("%d sounds  (* = modified, ! = breaks in-game, i = format note)",
                        int(sounds.size()));
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
    if (ImGui::BeginTable("sound_table", 8, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.f,
                                kColIndex);
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 2.f,
                                kColName);
        ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, 64.f,
                                kColType);
        ImGui::TableSetupColumn("format", ImGuiTableColumnFlags_WidthStretch, 3.f,
                                kColFmt);
        ImGui::TableSetupColumn(
            "size",
            ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
            78.f, kColSize);
        ImGui::TableSetupColumn(
            "uses",
            ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
            38.f, kColUses);
        ImGui::TableSetupColumn(
            "*",
            ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
            18.f, kColMod);
        ImGui::TableSetupColumn(
            "!",
            ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
            18.f, kColValid);
        ImGui::TableHeadersRow();
        static std::vector<int> order;
        SortOrder(sounds, order);
        for (int i : order) {
            const SoundRow& s = sounds[size_t(i)];
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
                const bool fatal = s.validity == Validity::Fatal;
                const ImVec4 col = fatal
                                       ? ImVec4(1.f, .35f, .3f, 1.f)     // red
                                       : ImVec4(.85f, .75f, .4f, .9f);   // dim yellow
                ImGui::TextColored(col, fatal ? "!" : "i");
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
