// Hook-only glue for the replay gameplay tag -- see gameplay_tag_hook.h.
#include "gameplay_tag_hook.h"
#include "gameplay_tag.h"
#include "globals.h"

#include <SDL3/SDL_log.h>
#include <windows.h>

namespace fm2k::gameplay_tag {

void AddLiveTeamRoster(Participants* parts) {
    if constexpr (!FM2K::kIsFM2K || FM2K::ADDR_P1_TEAM_ROSTER == 0) {
        (void)parts;
        return;
    } else {
        if (*(const uint32_t*)FM2K::ADDR_GAME_MODE_FLAG != 2) return;
        for (uintptr_t base : {FM2K::ADDR_P1_TEAM_ROSTER, FM2K::ADDR_P2_TEAM_ROSTER}) {
            for (int i = 0; i < 4; ++i) {
                const int32_t id = ((const int32_t*)base)[i];
                // Unused member slots hold -1 / stale values; only a valid
                // roster index can name a file. A stale-but-valid id only adds
                // a file to the tag (stricter, never looser).
                if (id >= 0 && id < 50) parts->chars.push_back((uint8_t)id);
            }
        }
    }
}

void PrewarmMatch(uint8_t p1_char, uint8_t p2_char, uint8_t stage_id) {
    if constexpr (!FM2K::kIsFM2K) {
        (void)p1_char; (void)p2_char; (void)stage_id;
        return;
    } else {
        Participants parts;
        parts.chars  = {p1_char, p2_char};
        parts.stages = {stage_id};
        AddLiveTeamRoster(&parts);
        const DWORD t0 = GetTickCount();
        const uint32_t tag = ComputeLocal(parts);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "GameplayTag: match chars %u/%u stage %u -> 0x%08X (%lu ms, cache "
            "%zu hit / %zu miss)%s",
            (unsigned)p1_char, (unsigned)p2_char, (unsigned)stage_id, tag,
            (unsigned long)(GetTickCount() - t0), CacheHits(), CacheMisses(),
            tag == 0 ? " -- 0 = exe/.kgt not resolvable, replays fall back to "
                       "the v1 install tag" : "");
    }
}

}  // namespace fm2k::gameplay_tag
