// input_binder_profiles.cpp -- INI persistence + per-game profile routing.
// Path helpers + Save/Load + SetGameProfile/Fork/Delete. DefaultConfigPath
// is external (core Init calls it); the rest are TU-local.
#include "input_binder.h"
#include "input_binder_internal.h"
#include <SDL3/SDL_log.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#  include <windows.h>
#endif

namespace FM2KInputBinder {
std::string g_active_game = "";  // external -- ui reads it (override checkbox)

namespace {
std::string DefaultProfileBaseDir() {
    if (const char* env = std::getenv("FM2K_INPUT_CONFIG_PATH")) {
        if (env[0]) {
            // env override — derive the base dir by stripping the filename.
            std::string s = env;
            size_t slash = s.find_last_of("/\\");
            return (slash == std::string::npos) ? "." : s.substr(0, slash);
        }
    }
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) {
        if (appdata[0]) {
            std::string dir = std::string(appdata) + "\\FM2K_Rollback";
            CreateDirectoryA(dir.c_str(), nullptr);
            return dir;
        }
    }
#endif
    return ".";
}

std::string SanitizeProfileName(const char* name) {
    // Filenames go on disk — strip ONLY the Windows-forbidden chars
    // and ASCII control bytes. Letting non-ASCII through preserves
    // UTF-8 sequences for games shipped by Japanese authors (e.g.
    // ＣＰＷ.exe → "ＣＰＷ" profile name). Original sanitizer treated
    // every byte 0x80+ as "not isalnum" and replaced with '_', which
    // turned full-width filenames into rows of underscores in the
    // launcher UI ("Use override for ＣＰＷ" → "Use override for ___").
    //
    // Win32 NTFS forbids: < > : " / \ | ? * and 0x00-0x1F. Everything
    // else (including the full Unicode BMP and beyond as UTF-8 bytes)
    // is legal in a filename via the W-API.
    std::string out;
    if (!name) return out;
    for (const char* p = name; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20) {
            out.push_back('_');                  // control char
            continue;
        }
        switch (c) {
            case '<': case '>': case ':': case '"':
            case '/': case '\\': case '|': case '?': case '*':
                out.push_back('_');
                continue;
        }
        out.push_back((char)c);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    return out;
}

std::string DefaultProfilePath() {
    // Anchor in %APPDATA%\FM2K_Rollback so the launcher EXE and the
    // injected hook DLL — which live in different working directories
    // — resolve to THE SAME path. CWD-relative was bugged: launcher
    // saved at e.g. C:\games\fm2k_inputs.ini, hook stat'd
    // C:\games\2dfm\wanwan\fm2k_inputs.ini. File never found in-game.
    return DefaultProfileBaseDir() +
#ifdef _WIN32
           std::string("\\")
#else
           std::string("/")
#endif
           + "fm2k_inputs.ini";
}

std::string GameProfilePath() {
    if (g_active_game.empty()) return DefaultProfilePath();
    return DefaultProfileBaseDir() +
#ifdef _WIN32
           std::string("\\")
#else
           std::string("/")
#endif
           + "fm2k_inputs_" + g_active_game + ".ini";
}

bool FileExists(const std::string& p) {
    if (p.empty()) return false;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}
void Trim(std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    size_t b = s.size();
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) --b;
    s = s.substr(a, b - a);
}

void WriteBinding(FILE* f, const char* key, const Binding& b) {
    switch (b.source) {
        case Binding::Source::NONE:
            std::fprintf(f, "%s=none\n", key);
            break;
        case Binding::Source::KEYBOARD:
            std::fprintf(f, "%s=kbd:%d\n", key, b.code);
            break;
        case Binding::Source::GAMEPAD_BUTTON:
            std::fprintf(f, "%s=padbtn:%d:%d\n", key, b.gamepad_index, b.code);
            break;
        case Binding::Source::GAMEPAD_AXIS:
            std::fprintf(f, "%s=padaxis:%d:%d:%d\n", key,
                         b.gamepad_index, b.code, b.axis_dir);
            break;
    }
}

bool ParseBinding(const std::string& v, Binding& out) {
    out = Binding{};
    if (v == "none" || v.empty()) return true;
    // Split on ':' lazily.
    auto next = [&](size_t& pos) -> std::string {
        size_t e = v.find(':', pos);
        std::string s = v.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = (e == std::string::npos) ? v.size() : e + 1;
        return s;
    };
    size_t p = 0;
    std::string tag = next(p);
    if (tag == "kbd") {
        out.source = Binding::Source::KEYBOARD;
        out.code = std::atoi(next(p).c_str());
        return true;
    } else if (tag == "padbtn") {
        out.source = Binding::Source::GAMEPAD_BUTTON;
        out.gamepad_index = std::atoi(next(p).c_str());
        out.code = std::atoi(next(p).c_str());
        return true;
    } else if (tag == "padaxis") {
        out.source = Binding::Source::GAMEPAD_AXIS;
        out.gamepad_index = std::atoi(next(p).c_str());
        out.code = std::atoi(next(p).c_str());
        out.axis_dir = std::atoi(next(p).c_str());
        return true;
    }
    return false;
}
}  // anonymous namespace

std::string DefaultConfigPath() {
    if (!g_active_game.empty()) {
        const std::string p = GameProfilePath();
        if (FileExists(p)) return p;
    }
    return DefaultProfilePath();
}

// Resolve the active config path against per-game profile + disk state.
static void RefreshActivePath() {
    g_config_path = DefaultConfigPath();
}

bool Save() {
    // Per-game profile routing rule: write to <game>.ini ONLY if that
    // file already exists on disk (user explicitly forked it via the
    // "Use override for X" checkbox). Otherwise write to the default
    // profile. Without this gate, every Save() while a game is
    // selected silently re-creates the per-game file even after the
    // user unchecked override — the checkbox would auto-re-check
    // itself on the next render after any binding tweak.
    const bool route_to_game =
        !g_active_game.empty() && FileExists(GameProfilePath());
    const std::string path = route_to_game
        ? GameProfilePath()
        : DefaultProfilePath();
    g_config_path = path;
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "; FM2K input bindings\n");
    for (int p = 0; p < kPlayers; ++p) {
        std::fprintf(f, "[Player%d]\n", p);
        // Stable device identity ("<GUID>[#serial]") — written before the
        // bit rows. Omitted entirely for identity-less (legacy) players so
        // their configs keep index-based routing untouched.
        if (!g_players[p].device_id.empty()) {
            std::fprintf(f, "device_id=%s\n",   g_players[p].device_id.c_str());
            std::fprintf(f, "device_name=%s\n", g_players[p].device_name.c_str());
        }
        for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
            WriteBinding(f, kBitNames[i], g_players[p].bits[i]);
            // Alt slot — emit only when set so legacy single-source
            // configs round-trip without picking up a noisy ".alt = NONE"
            // for every bit.
            if (g_players[p].bits_alt[i].source != Binding::Source::NONE) {
                char alt_key[64];
                std::snprintf(alt_key, sizeof(alt_key), "%s.alt", kBitNames[i]);
                WriteBinding(f, alt_key, g_players[p].bits_alt[i]);
            }
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    return true;
}

bool Load() {
    RefreshActivePath();  // resolves per-game vs default based on disk state
    FILE* f = std::fopen(g_config_path.c_str(), "r");
    if (!f) return false;

    // The config file exists → clear alt slots before parsing so missing
    // ".alt" keys mean "empty alt", not "XInput defaults from Init". Without
    // this, every pre-v0.2.16 config (no .alt keys ever written) silently
    // gets its alt slots populated with the auto-defaults — which OR with
    // the user's custom primary bindings and fire wrong bits when they
    // press a face button. Fresh installs (no config file) skip this branch
    // and keep the Init-time defaults; that's the intended path for new
    // users wanting "pad just works out of the box".
    //
    // Device identity is cleared for the same reason: a missing
    // "device_id" key must mean "legacy index routing", not whatever
    // identity a previously-loaded profile carried.
    for (int p = 0; p < kPlayers; ++p) {
        for (auto& b : g_players[p].bits_alt) b = Binding{};
        g_players[p].device_id.clear();
        g_players[p].device_name.clear();
    }

    int section = -1;  // -1 = none, 0/1 = player slot
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        Trim(s);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s.front() == '[' && s.back() == ']') {
            std::string sec = s.substr(1, s.size() - 2);
            if (sec == "Player0") section = 0;
            else if (sec == "Player1") section = 1;
            else section = -1;
            continue;
        }
        if (section < 0) continue;

        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        Trim(key);
        Trim(val);

        if (key == "device_id")   { g_players[section].device_id   = val; continue; }
        if (key == "device_name") { g_players[section].device_name = val; continue; }

        for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
            if (key == kBitNames[i]) {
                ParseBinding(val, g_players[section].bits[i]);
                break;
            }
            // ".alt" suffix → secondary slot for the same bit.
            // Old configs without this key leave bits_alt at NONE
            // (set by ApplyDefaults' loop pre-load), which is fine.
            char alt_key[64];
            std::snprintf(alt_key, sizeof(alt_key), "%s.alt", kBitNames[i]);
            if (key == alt_key) {
                ParseBinding(val, g_players[section].bits_alt[i]);
                break;
            }
        }
    }
    std::fclose(f);

    // Identity may have changed with the file contents — re-resolve each
    // player's pad against the currently-connected set.
    ResolvePlayerPads();

    // Breadcrumb — which file actually drives in-game input, and how many
    // bits each player has bound. Load() runs on a 1 s cadence, so only
    // log when the resolved path or a player's bound-count changes. This
    // is the line that turns a "P2 does nothing" report into a diagnosis
    // (wrong profile resolved / P2 rows empty) from the log alone.
    {
        int bound[kPlayers] = {};
        for (int p = 0; p < kPlayers; ++p) {
            for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
                if (g_players[p].bits[i].source     != Binding::Source::NONE ||
                    g_players[p].bits_alt[i].source != Binding::Source::NONE) {
                    ++bound[p];
                }
            }
        }
        static std::string s_last_path;
        static int s_last_bound[kPlayers] = {-1, -1};
        if (s_last_path != g_config_path ||
            s_last_bound[0] != bound[0] || s_last_bound[1] != bound[1]) {
            s_last_path = g_config_path;
            s_last_bound[0] = bound[0];
            s_last_bound[1] = bound[1];
            const bool is_override =
                !g_active_game.empty() && g_config_path == GameProfilePath();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "InputBinder: loaded '%s' (%s) — P1 %d/%d bits bound, "
                "P2 %d/%d bits bound%s",
                g_config_path.c_str(),
                is_override ? "per-game override" : "default profile",
                bound[0], (int)Bit::COUNT, bound[1], (int)Bit::COUNT,
                bound[1] == 0 ? " (P2 unbound → engine's own keys)" : "");
        }

        // PAUSE is the one bit whose absence is silently FATAL rather than
        // merely inconvenient. The engine toggles g_game_paused (0x4701BC)
        // only on a RISING EDGE of bit 0x400 in g_combined_input_changes
        // (vs_round_function @0x4086A0), and that word is built from what our
        // binder returns — so an unbound PAUSE row means the pause button is
        // simply dead, with no other symptom to hint at why. We deliberately
        // do NOT repair the profile here: the user's config is the user's
        // config. But a dead pause must not be silent. (Residual half of the
        // "P2 controls & pause dead in offline" report — the P2 half was a
        // per-player takeover bug, this half is just an unbound row.)
        //
        // Warn whenever PAUSE is unbound, regardless of how many other bits
        // the slot binds. There is NO "native pause" to fall back on: the
        // engine's own pipeline (process_game_inputs @0x4146D0 snapshots the
        // keyboard into KeyState[0x424D20], then calls get_player_input
        // @0x414340) runs THROUGH the function we detour, so every bit the
        // engine sees is whatever our hook returns. An all-unbound slot only
        // "works" because the hook hands back the vanilla read, which sources
        // the GAME's own key config -- inputs with no row in our binder at
        // all. That is the same phantom-input behavior we rejected for the
        // per-bit case, so it must not be treated here as a working fallback.
        static int s_last_pause_state[kPlayers] = {-1, -1};
        for (int p = 0; p < kPlayers; ++p) {
            const size_t si = (size_t)Bit::START;
            const bool has_start =
                g_players[p].bits[si].source     != Binding::Source::NONE ||
                g_players[p].bits_alt[si].source != Binding::Source::NONE;
            const int cur = has_start ? 1 : 0;
            if (s_last_pause_state[p] != cur) {
                s_last_pause_state[p] = cur;
                if (cur == 0) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "InputBinder: P%d has NO PAUSE BINDING — the pause "
                        "button will not work for that player. Bind PAUSE in "
                        "the input settings to restore it.", p + 1);
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Per-game profile management
// ---------------------------------------------------------------------------

void SetGameProfile(const char* exe_basename) {
    const std::string sanitized = SanitizeProfileName(exe_basename);
    if (g_active_game == sanitized) return;
    g_active_game = sanitized;
    RefreshActivePath();
}

bool HasGameProfile() {
    if (g_active_game.empty()) return false;
    return FileExists(GameProfilePath());
}

bool ForkDefaultToGameProfile() {
    if (g_active_game.empty()) return false;
    const std::string dst = GameProfilePath();
    if (FileExists(dst)) {
        // Already exists — caller can choose to overwrite via Save().
        // Treat fork-of-already-existing as a no-op success so the UI
        // checkbox toggle works idempotently.
        RefreshActivePath();
        return true;
    }
    const std::string src = DefaultProfilePath();
    FILE* in = std::fopen(src.c_str(), "rb");
    FILE* out = std::fopen(dst.c_str(), "wb");
    if (!out) {
        if (in) std::fclose(in);
        return false;
    }
    if (in) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
            std::fwrite(buf, 1, n, out);
        }
        std::fclose(in);
    } else {
        // No default file yet — write the current in-memory bindings as the
        // seed of the per-game profile. Otherwise the per-game file would
        // be empty and Load() would silently leave defaults in place.
        std::fclose(out);
        const std::string saved_active = g_active_game;
        g_config_path = dst;  // route Save() to the per-game file
        const bool ok = Save();
        g_active_game = saved_active;
        RefreshActivePath();
        return ok;
    }
    std::fclose(out);
    RefreshActivePath();
    return true;
}

bool DeleteGameProfile() {
    if (g_active_game.empty()) return false;
    const std::string p = GameProfilePath();
    if (!FileExists(p)) {
        RefreshActivePath();
        return false;
    }
    std::remove(p.c_str());
    RefreshActivePath();
    Load();  // pull bindings back from default
    return true;
}

const char* CurrentConfigPath() {
    if (g_config_path.empty()) g_config_path = DefaultConfigPath();
    return g_config_path.c_str();
}

}  // namespace FM2KInputBinder
