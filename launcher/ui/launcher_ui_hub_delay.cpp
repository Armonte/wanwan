// Hub panel "Delay" combo (#24), split out of launcher_ui_hub_panel.cpp.
//
// Peers exchange a delay candidate over the control channel. Two AUTO peers
// adopt min(local, remote) -- from a current build only, see netplay_battle.cpp
// -- so they land on the same delay. A MANUAL pin runs verbatim and purely
// local: it is advertised (an auto opponent raises itself to at least it) but
// is never itself raised, so two manual peers may run different delays.
// Asymmetric is fine -- inputs are frame-tagged.
//
//   index 0 "auto"                  -> FM2K_DELAY_MODE=0, rollback cap 8
//   index 1 "auto (less rollback)"  -> FM2K_DELAY_MODE=1, rollback cap 4
//   index 2..28 manual 0..26        -> FM2K_LOCAL_DELAY=N
//
// Auto splits the p95 one-way trip between input delay and rollback
// (FM2KHook/src/netplay/delay_math.h SplitDelay). FM2K runs at 100 Hz, so one
// frame is 10 ms. Manual 0 is for LAN / loopback / hotseat only: any jitter
// rolls back every frame.
#include "launcher_ui_internal.h"
#include "FM2K_Locale.h"
#include "vendored/imgui/imgui.h"
#include <windows.h>
#include <cstdio>

namespace lui {

void RenderDelayCombo() {
    static int s_delay_override = 0;

    const char* items[2 + 27];
    items[0] = T("delay_auto");
    items[1] = T("delay_auto_smooth");
    static const char* kManual[27] = {
        "0",  "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",  "9",
        "10", "11", "12", "13", "14", "15", "16", "17", "18", "19",
        "20", "21", "22", "23", "24", "25", "26",
    };
    for (int i = 0; i < 27; ++i) items[2 + i] = kManual[i];

    ImGui::PushItemWidth(-120);
    ImGui::Combo(T("delay_label"), &s_delay_override, items, IM_ARRAYSIZE(items));
    ImGui::PopItemWidth();
    ImGui::SetItemTooltip("%s", T("delay_tooltip"));

    if (s_delay_override >= 2) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", s_delay_override - 2);
        ::SetEnvironmentVariableA("FM2K_LOCAL_DELAY", buf);
    } else {
        ::SetEnvironmentVariableA("FM2K_LOCAL_DELAY", nullptr);
    }
    // Harmless when a manual value is pinned (the hook ignores the mode once
    // FM2K_LOCAL_DELAY is set).
    ::SetEnvironmentVariableA("FM2K_DELAY_MODE", s_delay_override == 1 ? "1" : "0");
}

}  // namespace lui
