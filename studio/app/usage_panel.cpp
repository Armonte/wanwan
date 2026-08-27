// usage_panel.cpp -- right-bottom panel: where the selected sound plays.
// One row per xref site: a sprite thumbnail plus "'action' @tick N"
// ('~tick' when a jump op made the tick an estimate), and a larger preview
// of whichever row is selected.
//
// The thumbnail is the point of this panel. An xref line tells a modder
// that a sound plays on action "5B" at tick 12, which is only useful if
// they already know that action by heart; the frame it plays on answers
// "what is on screen when this sound fires" directly.
//
// GPU TEXTURES ARE CACHED AND KEYED ON THE MODEL GENERATION, not on the
// sprite index alone. Sprite indices are dense and start at 0 in every
// file, so an index-only cache survives a File>Open and cheerfully draws
// the previous character's artwork against the new one's xrefs -- wrong in
// a way that looks plausible, which is the worst kind.
#include "imgui.h"
#include <SDL3/SDL.h>

#include <unordered_map>

#include "../core/sprite_decode.h"
#include "app_state.h"
#include "real_model.h"

namespace studio {
namespace {

constexpr float kRowThumbMax = 44.0f;      // inline, per xref row
constexpr float kPreviewMax  = 168.0f;     // the selected row's larger preview

struct TextureCache {
    std::unordered_map<int, SDL_Texture*> tex;
    uint64_t generation = 0;
    SDL_Renderer* renderer = nullptr;

    void Clear() {
        for (auto& kv : tex) if (kv.second) SDL_DestroyTexture(kv.second);
        tex.clear();
    }
};

TextureCache& Cache() { static TextureCache c; return c; }

// Upload once, reuse thereafter. Returns nullptr when the sprite is absent
// or would not decode, which the caller renders as a placeholder rather
// than as an error -- a missing thumbnail must not imply a broken xref.
SDL_Texture* TextureFor(AppState& st, int sprite) {
    TextureCache& c = Cache();
    if (!st.renderer || !st.model || sprite < 0) return nullptr;

    if (c.renderer != st.renderer || c.generation != st.model->Generation()) {
        c.Clear();
        c.renderer = st.renderer;
        c.generation = st.model->Generation();
    }
    auto it = c.tex.find(sprite);
    if (it != c.tex.end()) return it->second;

    const kgt::DecodedSprite* d = st.model->Sprite(sprite);
    SDL_Texture* t = nullptr;
    if (d && d->ok && d->width > 0 && d->height > 0) {
        t = SDL_CreateTexture(st.renderer, SDL_PIXELFORMAT_RGBA32,
                              SDL_TEXTUREACCESS_STATIC, d->width, d->height);
        if (t) {
            SDL_UpdateTexture(t, nullptr, d->rgba.data(), d->width * 4);
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
            // Nearest, always. These are hand-placed 8bpp pixels and any
            // filtering turns a readable 32x48 sprite into a smear.
            SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
        }
    }
    // Cached even when null: see RealModel::Sprite -- a sprite that will
    // never decode must not be retried once per frame forever.
    c.tex.emplace(sprite, t);
    return t;
}

// Fit within a square box without upscaling past the box or distorting.
ImVec2 FitBox(int w, int h, float box) {
    const float s = (w <= 0 || h <= 0) ? 1.0f
                  : (box / (float)(w > h ? w : h));
    return ImVec2((float)w * s, (float)h * s);
}

// Checkerboard behind the image so keyed pixels read as TRANSPARENT rather
// than as black artwork -- most FM2K sprites key index 0, and against a
// dark ImGui theme an unmarked hole is indistinguishable from a filled one.
void Checkerboard(ImVec2 p, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cell = 6.0f;
    const ImU32 a = IM_COL32(72, 72, 78, 255), b = IM_COL32(52, 52, 58, 255);
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), a);
    for (float y = 0; y < size.y; y += cell) {
        for (float x = ((int)(y / cell) % 2) ? cell : 0; x < size.x; x += cell * 2) {
            const float w = (x + cell > size.x) ? size.x - x : cell;
            const float h = (y + cell > size.y) ? size.y - y : cell;
            dl->AddRectFilled(ImVec2(p.x + x, p.y + y),
                              ImVec2(p.x + x + w, p.y + y + h), b);
        }
    }
}

void DrawSprite(AppState& st, int sprite, float box) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    SDL_Texture* t = TextureFor(st, sprite);
    const kgt::DecodedSprite* d = (sprite >= 0 && st.model) ? st.model->Sprite(sprite) : nullptr;
    if (!t || !d) {
        Checkerboard(p, ImVec2(box, box));
        ImGui::Dummy(ImVec2(box, box));
        return;
    }
    const ImVec2 size = FitBox(d->width, d->height, box);
    Checkerboard(p, size);
    ImGui::Image((ImTextureID)(intptr_t)t, size);
}

}  // namespace

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

    // Selection is per-sound: moving to another sound must not leave the
    // preview pointing at a row index that sound does not have.
    static int sel_use = 0;
    static int sel_sound = -1;
    if (sel_sound != st.selected) { sel_sound = st.selected; sel_use = 0; }
    if (sel_use >= int(uses.size())) sel_use = 0;

    const UseRow& cur = uses[(size_t)sel_use];
    if (cur.sprite >= 0) {
        DrawSprite(st, cur.sprite, kPreviewMax);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(cur.action.c_str());
        const kgt::DecodedSprite* d = st.model->Sprite(cur.sprite);
        if (d) ImGui::Text("sprite %d  %dx%d", cur.sprite, d->width, d->height);
        else   ImGui::Text("sprite %d  (no image)", cur.sprite);
        ImGui::TextDisabled("@%stick %d", cur.tick_estimated ? "~" : "", cur.tick);
        if (cur.tick_estimated)
            ImGui::TextDisabled("tick is an estimate (a jump op precedes it)");
        ImGui::EndGroup();
    } else {
        ImGui::TextDisabled("'%s' @%stick %d -- no sprite on this site",
                            cur.action.c_str(), cur.tick_estimated ? "~" : "", cur.tick);
    }

    ImGui::Separator();
    ImGui::TextDisabled("%d use(s)", int(uses.size()));

    for (int i = 0; i < int(uses.size()); ++i) {
        const UseRow& u = uses[(size_t)i];
        ImGui::PushID(i);
        const ImVec2 start = ImGui::GetCursorPos();
        if (ImGui::Selectable("##row", i == sel_use, 0, ImVec2(0, kRowThumbMax)))
            sel_use = i;
        ImGui::SetCursorPos(start);
        DrawSprite(st, u.sprite, kRowThumbMax);
        ImGui::SameLine();
        if (u.sprite >= 0)
            ImGui::Text("'%s' @%stick %d  sprite %d", u.action.c_str(),
                        u.tick_estimated ? "~" : "", u.tick, u.sprite);
        else
            ImGui::Text("'%s' @%stick %d", u.action.c_str(),
                        u.tick_estimated ? "~" : "", u.tick);
        ImGui::PopID();
    }
    ImGui::End();
}

}  // namespace studio
