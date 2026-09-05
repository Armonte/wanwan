#include "screenshot.h"

#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

// Single-file public-domain PNG writer. MinGW's GDI+ headers were
// the alternative and they break the build with PROPID-not-declared
// errors that nobody has bothered fixing upstream. stb_image_write
// has zero deps and produces fine PNGs at this scale (320x240 - 1280x960).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../../vendored/stb/stb_image_write.h"

namespace FM2KCapture {

namespace {

std::string  g_capture_dir;
bool         g_active = false;

}  // namespace


void Init() {
    if (g_active) return;
    const char* dir = std::getenv("FM2K_CAPTURE_DIR");
    const char* on  = std::getenv("FM2K_AUTO_CAPTURE");
    const bool enabled = on && on[0] != '\0' && on[0] != '0';
    if (!enabled || !dir) return;
    g_capture_dir = dir;
    std::error_code ec;
    std::filesystem::create_directories(g_capture_dir, ec);
    if (ec) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "FM2KCapture: create_directories('%s') failed: %s",
            dir, ec.message().c_str());
    }
    g_active = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "FM2KCapture: active (capture_dir='%s')", dir);
}


void Shutdown() {
    g_active = false;
}


bool IsActive() { return g_active; }

bool g_sweep_active = false;
bool g_sweep_nudge = false;
bool g_sweep_nudge_down = false;


// Read the game's OWN framebuffer instead of the window.
//
// WHY NOT GDI. BitBlt captures the window, and the game renders through
// DirectDraw, so until the surface has been presented BitBlt returns a
// game-independent fill. That is not a theory: captured at title, wanwan and
// vanpri produced BYTE-IDENTICAL files (same SHA1), while their CSS captures
// -- taken after presents had started -- differed properly. No blank
// heuristic can separate "not presented yet" from "presented and dark",
// because the bytes genuinely do not come from the game.
//
// The framebuffer is the same one BlitVerify in hooks_render.cpp already
// reads and writes: a pointer at 0x4246CC to 640x480 16-bit pixels, stride
// 1280. Reading it gets the frame the game actually composed, whether or not
// anything has been presented to a window, which also means capture works
// with the window minimized or offscreen -- the point of a headless sweep.
static std::vector<uint8_t> CaptureFramebufferRGBA(int& w, int& h) {
    std::vector<uint8_t> out;
    const uint8_t* fb = *(const uint8_t* const*)0x4246CC;
    if (!fb) return out;
    w = 640; h = 480;
    out.resize((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint16_t* row = (const uint16_t*)(fb + (size_t)1280 * y);
        uint8_t* dst = out.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            const uint16_t p = row[x];
            // RGB565. Replicating the high bits into the low ones scales
            // 5/6-bit channels to 8-bit without a divide and keeps white at
            // 255 rather than 248 -- a plain shift leaves everything subtly
            // dark, which is the sort of thing that looks fine until it is
            // next to a correct image.
            const uint8_t r = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
            const uint8_t g = (uint8_t)(((p >> 5)  & 0x3F) * 255 / 63);
            const uint8_t b = (uint8_t)(( p        & 0x1F) * 255 / 31);
            dst[x * 4 + 0] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = 255;
        }
    }
    return out;
}

namespace {
std::vector<uint8_t> g_held;
int g_held_w = 0, g_held_h = 0;
int g_held_score = -1;

// Stillness tracking. A 16x16 luma thumbnail of the previous frame, and how
// many consecutive frames have looked like it. Sixteen squared is enough to
// see a fade or a wipe (they move every cell at once) and coarse enough that
// an animating portrait moves only a few.
constexpr int kThumbDim = 16;
uint8_t g_prev_thumb[kThumbDim * kThumbDim] = {};
bool    g_prev_thumb_valid = false;
int     g_settled_frames = 0;
// Mean absolute luma difference, 0-255, below which the picture counts as
// still. Four tolerates a cursor blink and a little animation; a crossfade
// is an order of magnitude above it.
int     g_settle_tol = -1;   // resolved from env on first use

void ThumbOf(const std::vector<uint8_t>& rgba, int w, int h, uint8_t* out) {
    for (int ty = 0; ty < kThumbDim; ++ty) {
        for (int tx = 0; tx < kThumbDim; ++tx) {
            // One sample per cell, taken at its centre. Averaging the whole
            // cell would be more faithful and costs 1.2 MB of reads per
            // frame; this runs on every captured frame, so it samples.
            const int sx = (int)(((int64_t)tx * 2 + 1) * w / (kThumbDim * 2));
            const int sy = (int)(((int64_t)ty * 2 + 1) * h / (kThumbDim * 2));
            const size_t i = ((size_t)sy * (size_t)w + (size_t)sx) * 4;
            if (i + 2 >= rgba.size()) { out[ty * kThumbDim + tx] = 0; continue; }
            // Rec.601 luma, integer.
            out[ty * kThumbDim + tx] = (uint8_t)(
                ((int)rgba[i] * 77 + (int)rgba[i + 1] * 150 + (int)rgba[i + 2] * 29) >> 8);
        }
    }
}

void NoteStillness(const std::vector<uint8_t>& rgba, int w, int h) {
    if (g_settle_tol < 0) {
        g_settle_tol = 4;
        if (const char* e = std::getenv("FM2K_CAPTURE_SETTLE_TOL")) {
            const int v = std::atoi(e);
            if (v >= 0 && v <= 255) g_settle_tol = v;
        }
    }
    uint8_t thumb[kThumbDim * kThumbDim];
    ThumbOf(rgba, w, h, thumb);
    if (!g_prev_thumb_valid) {
        std::memcpy(g_prev_thumb, thumb, sizeof thumb);
        g_prev_thumb_valid = true;
        g_settled_frames = 0;
        return;
    }
    int sum = 0;
    for (size_t i = 0; i < sizeof thumb; ++i) {
        sum += std::abs((int)thumb[i] - (int)g_prev_thumb[i]);
    }
    const int mean = sum / (int)sizeof thumb;
    std::memcpy(g_prev_thumb, thumb, sizeof thumb);
    if (mean <= g_settle_tol) ++g_settled_frames;
    else                      g_settled_frames = 0;
}

// How much is actually ON this frame. Distinct sampled colours, minus a
// penalty for one colour dominating -- a flat fill and a fade-out both
// score near zero, a drawn screen scores high.
int FrameScore(const std::vector<uint8_t>& rgba, int w, int h) {
    const size_t px = (size_t)w * (size_t)h;
    if (px == 0) return 0;
    const size_t step = px > 4096 ? px / 4096 : 1;
    uint32_t seen[128] = {0};
    bool used[128] = {false};
    size_t counts[128] = {0};
    size_t distinct = 0, sampled = 0, best = 0;
    for (size_t i = 0; i < px; i += step) {
        const uint8_t* p8 = rgba.data() + i * 4;
        const uint32_t v = (uint32_t)p8[0] | ((uint32_t)p8[1] << 8) |
                           ((uint32_t)p8[2] << 16);
        ++sampled;
        const size_t slot = (v * 2654435761u) % 128;
        for (size_t probe = 0; probe < 128; ++probe) {
            const size_t k = (slot + probe) % 128;
            if (!used[k]) { used[k] = true; seen[k] = v; counts[k] = 1; ++distinct; break; }
            if (seen[k] == v) { ++counts[k]; break; }
        }
    }
    for (size_t k = 0; k < 128; ++k) if (used[k] && counts[k] > best) best = counts[k];
    const int dom = sampled ? (int)(best * 100 / sampled) : 100;
    return (int)distinct * (100 - dom);
}
}  // namespace

// KEEP THE RICHEST FRAME OF THE STATE, not the last one.
//
// "Last frame" fails when a state ends mid-transition: vanpri's title lasts
// 53 frames and its final frame is a fade-out -- captured, that came out
// 99.4% black with an empty UI box, while wanwan's title over a comparable
// 57 frames was a full screen. Scoring each frame and keeping the best is
// immune to where in the state the game happens to be when it ends, and it
// cannot do worse than last-frame on a state that was drawn throughout.
void RememberFrame() {
    if (!g_active) return;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba = CaptureFramebufferRGBA(w, h);
    if (rgba.empty()) return;
    NoteStillness(rgba, w, h);
    const int score = FrameScore(rgba, w, h);
    if (score <= g_held_score) return;
    g_held = std::move(rgba);
    g_held_w = w;
    g_held_h = h;
    g_held_score = score;
}

void ResetHeldScore() { g_held_score = -1; }

int FramesSettled() { return g_settled_frames; }

void ResetSettle() {
    g_settled_frames = 0;
    g_prev_thumb_valid = false;
}

bool WriteRememberedFrame(const std::string& filename) {
    if (!g_active || g_held.empty()) return false;
    std::filesystem::path out_path =
        std::filesystem::path(g_capture_dir) / filename;
    const int ok = stbi_write_png(out_path.string().c_str(), g_held_w, g_held_h,
                                  4, g_held.data(), g_held_w * 4);
    if (ok) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "FM2KCapture: wrote %dx%d -> %s", g_held_w, g_held_h,
            out_path.string().c_str());
    }
    g_held.clear();
    g_held_score = -1;   // next state starts scoring from scratch
    return ok != 0;
}

}  // namespace FM2KCapture
