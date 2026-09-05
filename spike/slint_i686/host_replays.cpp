#include "host_replays.h"

#include "host_http.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

// FM2KSessionFileHeader v2, 256 bytes (docs/replay_hierarchy_status.md:101).
// Read by OFFSET rather than by re-declaring the struct: the writer owns
// that definition, and a second copy here would be a thing to keep in sync
// that nothing would notice had drifted.
constexpr size_t kHdr = 256;
constexpr uint32_t kMagicSet = 0x53534D46;  // 'FMSS' -- .fm2kset
constexpr uint32_t kMagicRep = 0x52504D46;  // 'FMPR' -- .fm2krep
constexpr size_t kOffFlags       = 6;    // u16, bit0: battle slice
constexpr size_t kOffFinished    = 16;   // u64 unix
constexpr size_t kOffGameId      = 32;   // char[32]
constexpr size_t kOffP1Nick      = 64;   // char[32]
constexpr size_t kOffP2Nick      = 96;   // char[32]
constexpr size_t kOffP1Char      = 128;  // u8, index into the .kgt roster
constexpr size_t kOffP2Char      = 129;  // u8
constexpr size_t kOffRoundsWonP1 = 132;  // u8
constexpr size_t kOffRoundsWonP2 = 133;  // u8
constexpr size_t kOffMatchCount  = 134;  // u8
constexpr size_t kOffMatchIndex  = 135;  // u8 -- 1-based in a .fm2krep, 0 in a set
constexpr size_t kOffSessionId   = 136;  // u64 -- shared across one set

template <typename T>
T Read(const uint8_t* h, size_t off) {
    T v{};
    std::memcpy(&v, h + off, sizeof(T));
    return v;
}

std::string Fixed(const uint8_t* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len]) ++len;
    std::string s(reinterpret_cast<const char*>(p), len);
    // These come out of the game as raw bytes. A tab would split a field
    // and a newline would split a row, so neither is trusted downstream.
    for (char& c : s) {
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

std::string Stamp(uint64_t t, const char* fmt) {
    if (t == 0) return "--";
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm lt{};
    if (localtime_s(&lt, &tt) != 0) return "--";
    char buf[32];
    std::strftime(buf, sizeof(buf), fmt, &lt);
    return buf;
}

std::string HumanSize(uintmax_t b) {
    char buf[32];
    if (b >= 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", (double)b / 1048576.0);
    else                  std::snprintf(buf, sizeof(buf), "%llu KB", (unsigned long long)(b / 1024));
    return buf;
}

// game folder + roster slot -> face image, from the sweep's faces.tsv.
// Loaded once; a missing index just means no faces, which the row renders
// as absence rather than as a broken image.
std::map<std::string, std::string>& FaceIndex() {
    static std::map<std::string, std::string> idx;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        // Try the WSL path first, then the Windows one -- the same
        // two-shot the thumbnail index uses. This binary is built for
        // Windows but the tooling that writes the index runs under WSL,
        // and a single hardcoded /mnt path silently opens nothing here,
        // which reads as "no faces were extracted" rather than "the file
        // could not be found".
        std::ifstream f("/mnt/c/games/captures/faces/faces.tsv");
        if (!f) f.open("C:\\games\\captures\\faces\\faces.tsv");
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            // game \t slot \t name \t path
            const size_t a = line.find('\t');
            if (a == std::string::npos) continue;
            const size_t b = line.find('\t', a + 1);
            if (b == std::string::npos) continue;
            const size_t c = line.find('\t', b + 1);
            if (c == std::string::npos) continue;
            idx[line.substr(0, b)] = line.substr(c + 1);
        }
    }
    return idx;
}

std::string FaceFor(const std::string& game_folder, int slot) {
    if (slot < 0) return {};
    auto& idx = FaceIndex();
    auto it = idx.find(game_folder + "\t" + std::to_string(slot));
    return it == idx.end() ? std::string() : it->second;
}

struct File {
    bool        parsed = false;
    bool        battle = false;   // .fm2krep slice vs .fm2kset whole set
    uint64_t    finished = 0;
    uint64_t    session_id = 0;
    unsigned    match_index = 0;
    unsigned    match_count = 0;
    unsigned    won1 = 0, won2 = 0;
    int         char1 = -1, char2 = -1;
    std::string game, folder, p1, p2, path, size;
    // Which peer recorded it. Both sides of a match keep their own file
    // under one session, so without this the set lists every match twice
    // with nothing to tell the two rows apart.
    std::string side;
};

struct Group {
    bool                 real_session = false;
    std::vector<size_t>  idx;
    uint64_t             latest = 0;
    std::string          game, p1, p2;
};

}  // namespace

std::string ScanReplays(const std::vector<std::string>& roots) {
    std::vector<File> files;
    std::error_code ec;

    for (const std::string& root : roots) {
        if (!fs::is_directory(root, ec)) continue;
        for (const auto& game : fs::directory_iterator(root, ec)) {
            if (!game.is_directory(ec)) continue;
            const std::string folder = game.path().filename().string();
            for (const char* sub : {"replays", "sessions"}) {
                const fs::path dir = game.path() / sub;
                if (!fs::is_directory(dir, ec)) continue;
                for (const auto& e : fs::directory_iterator(dir, ec)) {
                    if (!e.is_regular_file(ec)) continue;
                    const std::string ext = e.path().extension().string();
                    if (ext != ".fm2krep" && ext != ".fm2kset") continue;

                    File f;
                    f.path = e.path().string();
                    f.size = HumanSize(e.file_size(ec));
                    f.battle = (ext == ".fm2krep");

                    uint8_t h[kHdr] = {};
                    std::ifstream in(e.path(), std::ios::binary);
                    if (in && in.read(reinterpret_cast<char*>(h), kHdr)) {
                        const uint32_t magic = Read<uint32_t>(h, 0);
                        if (magic == kMagicSet || magic == kMagicRep) {
                            f.parsed      = true;
                            f.battle      = (Read<uint16_t>(h, kOffFlags) & 1) != 0;
                            f.finished    = Read<uint64_t>(h, kOffFinished);
                            f.session_id  = Read<uint64_t>(h, kOffSessionId);
                            f.match_index = Read<uint8_t>(h, kOffMatchIndex);
                            f.match_count = Read<uint8_t>(h, kOffMatchCount);
                            f.won1        = Read<uint8_t>(h, kOffRoundsWonP1);
                            f.won2        = Read<uint8_t>(h, kOffRoundsWonP2);
                            f.char1       = Read<uint8_t>(h, kOffP1Char);
                            f.char2       = Read<uint8_t>(h, kOffP2Char);
                            f.game        = Fixed(h + kOffGameId, 32);
                            f.p1          = Fixed(h + kOffP1Nick, 32);
                            f.p2          = Fixed(h + kOffP2Nick, 32);
                        }
                    }
                    f.folder = folder;
                    {
                        const std::string stem = e.path().stem().string();
                        if (stem.find("_p0") != std::string::npos) f.side = "P0";
                        else if (stem.find("_p1") != std::string::npos) f.side = "P1";
                        if (stem.find("harness") != std::string::npos)
                            f.side += f.side.empty() ? "harness" : " harness";
                    }
                    if (f.game.empty()) f.game = folder;
                    if (f.finished == 0) {
                        const auto ft = fs::last_write_time(e.path(), ec);
                        if (!ec) {
                            const auto sys = std::chrono::time_point_cast<
                                std::chrono::system_clock::duration>(
                                ft - fs::file_time_type::clock::now() +
                                std::chrono::system_clock::now());
                            f.finished = (uint64_t)std::chrono::system_clock::to_time_t(sys);
                        }
                    }
                    files.push_back(std::move(f));
                }
            }
        }
    }

    // GROUPING, following the shipping browser: a session_id groups the
    // slices of one connection with their set file. Files without one are
    // NOT one undifferentiated bucket -- they group by what they do carry,
    // game + both nicks + the calendar day, so a pile of joined matches
    // reads as opponents-and-days instead of a single enormous node.
    std::vector<Group> groups;
    std::unordered_map<uint64_t, size_t> by_sid;
    std::unordered_map<std::string, size_t> by_key;
    for (size_t i = 0; i < files.size(); ++i) {
        const File& f = files[i];
        const bool real = f.parsed && f.session_id != 0;
        size_t gi;
        if (real) {
            auto [it, ins] = by_sid.try_emplace(f.session_id, groups.size());
            if (ins) groups.push_back({});
            gi = it->second;
        } else {
            // The INSTALL FOLDER is part of the key, not just the header's
            // game_id. Harness and older recordings share a game_id across
            // different games (every one of ours says "_hrun"), which
            // merged unrelated titles under a single heading -- two games
            // are never one session.
            std::string key = f.folder + '\x1f' + f.game + '\x1f' + f.p1
                            + '\x1f' + f.p2 + '\x1f'
                            + Stamp(f.finished, "%Y-%m-%d");
            auto [it, ins] = by_key.try_emplace(key, groups.size());
            if (ins) groups.push_back({});
            gi = it->second;
        }
        Group& g = groups[gi];
        if (g.idx.empty()) {
            g.real_session = real;
            // Show the folder: it is the name the user installed and
            // recognises, whereas game_id is an internal tag they have
            // never seen and which is sometimes wrong.
            g.game = f.folder.empty() ? f.game : f.folder;
            g.p1 = f.p1; g.p2 = f.p2;
        }
        g.idx.push_back(i);
        g.latest = std::max(g.latest, f.finished);
    }

    std::sort(groups.begin(), groups.end(),
              [](const Group& a, const Group& b) { return a.latest > b.latest; });
    for (Group& g : groups) {
        std::sort(g.idx.begin(), g.idx.end(), [&](size_t a, size_t b) {
            const File& x = files[a];
            const File& y = files[b];
            if (g.real_session) {
                // A real session IS a set: the whole-set file first, then
                // its matches in PLAY order. Study flows depend on 1..N.
                if (x.battle != y.battle) return !x.battle;
                if (x.match_index != y.match_index) return x.match_index < y.match_index;
                return x.finished < y.finished;
            }
            // A synthetic group is not a set -- there is no play order to
            // preserve, so newest first, which is what "I just played
            // that, where is it" needs.
            return x.finished > y.finished;
        });
    }

    std::string out;
    for (const Group& g : groups) {
        // The set file, if this session has one. "Watch set from here"
        // needs its path, and a session recorded by a guest may not have
        // one at all.
        std::string set_path, set_size;
        for (size_t i : g.idx) {
            if (!files[i].battle) { set_path = files[i].path; set_size = files[i].size; break; }
        }

        // Counting rule differs by group kind, because match_index means
        // different things in each. Inside a REAL session it is a position
        // in a set and both peers stamp the same one, so counting files
        // reported a three-match set as six -- count distinct indices.
        // A SYNTHETIC group is a bag of unrelated recordings that merely
        // happened on the same day, and every one of them is stamped
        // "match 1", so distinct indices would report a whole afternoon
        // as a single match. There, count the files.
        // COUNT WHAT WE LIST. This used to count distinct match_index
        // values for a real session and raw files otherwise, while the
        // child loop below emitted one row per battle file unconditionally
        // -- so a set could say "3 matches" above two rows and there was
        // no way for a reader to tell which number was wrong.
        unsigned matches = 0;
        for (size_t i : g.idx) if (files[i].battle) ++matches;

        // Group header row. It is a SET only when a set file exists to
        // play; otherwise it is a heading over loose matches and must not
        // offer a Watch button that has nothing to open.
        {
            const File& first = files[g.idx.front()];
            out += set_path.empty() ? "GROUP" : "SET";           out += '\t';
            out += Stamp(g.latest, "%Y-%m-%d %H:%M");            out += '\t';
            out += g.game;                                       out += '\t';
            out += first.p1;                                     out += '\t';
            out += first.p2;                                     out += '\t';
            out += std::to_string(matches) + (matches == 1 ? " match" : " matches");
            out += '\t';
            out += set_size;                                     out += '\t';
            out += set_path;                                     out += '\t';
            out += set_path;                                     out += '\t';
            out += "0";                                          out += "\t\t\t0\n";
        }

        for (size_t i : g.idx) {
            const File& f = files[i];
            if (!f.battle) continue;  // the set file is the header row
            out += "MATCH";                                      out += '\t';
            out += Stamp(f.finished, "%H:%M");                   out += '\t';
            // The match column is "#N" in the UI now, so the side (host /
            // guest) rides along here rather than being dropped.
            out += f.side;
            out += '\t';
            out += f.p1;                                         out += '\t';
            out += f.p2;                                         out += '\t';
            // Rounds won, when the recorder captured an outcome. Zero-zero
            // means it did not, and printing "0-0" would assert a draw.
            out += (f.won1 || f.won2)
                 ? (std::to_string(f.won1) + "-" + std::to_string(f.won2)) : "--";
            out += '\t';
            out += f.size;                                       out += '\t';
            out += f.path;                                       out += '\t';
            out += set_path;                                     out += '\t';
            out += std::to_string(f.match_index);                out += '\t';
            out += FaceFor(f.folder, f.char1);                   out += '\t';
            out += FaceFor(f.folder, f.char2);                   out += '\t';
            // Who won, so the loser's face can be dimmed. 0 when the
            // recorder captured no outcome -- dimming a face on a guess
            // would tell the reader something false about the match.
            out += std::to_string(f.won1 > f.won2 ? 1 : (f.won2 > f.won1 ? 2 : 0));
            out += '\n';
        }
    }
    return out;
}

namespace {

// Pull one field out of a flat JSON object. The hub's replay list is a
// flat array of flat objects with no nesting and no escaped quotes in
// practice, so a full parser would be more code than the whole feature.
// Deliberately conservative: anything it cannot find comes back empty and
// the row simply shows less.
std::string JField(const std::string& obj, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    const size_t k = obj.find(pat);
    if (k == std::string::npos) return {};
    size_t i = obj.find(':', k + pat.size());
    if (i == std::string::npos) return {};
    ++i;
    while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t')) ++i;
    if (i >= obj.size()) return {};
    if (obj[i] == '"') {
        const size_t e = obj.find('"', i + 1);
        if (e == std::string::npos) return {};
        return obj.substr(i + 1, e - i - 1);
    }
    const size_t e = obj.find_first_of(",}", i);
    return obj.substr(i, (e == std::string::npos ? obj.size() : e) - i);
}

}  // namespace

std::string FetchCommunityReplays(const std::string& hub_host,
                                  const std::string& game_id,
                                  bool* reached) {
    if (reached) *reached = false;
    if (hub_host.empty() || game_id.empty()) return {};
    std::string body;
    const std::string url = "https://" + hub_host + "/replay/list?limit=200&game=" + game_id;
    if (!hostgui::HttpGet(url, &body)) return {};
    if (reached) *reached = true;

    std::string out;
    size_t pos = 0;
    for (;;) {
        const size_t a = body.find('{', pos);
        if (a == std::string::npos) break;
        const size_t b = body.find('}', a);
        if (b == std::string::npos) break;
        const std::string obj = body.substr(a, b - a + 1);
        pos = b + 1;

        const std::string id = JField(obj, "id");
        if (id.empty()) continue;
        // First entry of the "files" array -- the set file, which is what
        // WATCH should open. JField hands back the raw array text; the
        // first quoted string in it is the name.
        const std::string files = JField(obj, "files");
        std::string first_file;
        {
            const size_t q = files.find('"');
            if (q != std::string::npos) {
                const size_t e = files.find('"', q + 1);
                if (e != std::string::npos) first_file = files.substr(q + 1, e - q - 1);
            }
        }

        uint64_t ts = 0;
        try { ts = std::stoull(JField(obj, "ts")); } catch (...) {}
        const std::string p1 = JField(obj, "p1");
        const std::string p2 = JField(obj, "p2");
        const std::string s1 = JField(obj, "p1_score");
        const std::string s2 = JField(obj, "p2_score");

        int w1 = 0, w2 = 0;
        try { w1 = std::stoi(s1); } catch (...) {}
        try { w2 = std::stoi(s2); } catch (...) {}

        // A community row is one SET. It has no local path, so WATCH has
        // nothing to open until downloading is wired -- the row carries
        // the hub id in place of a path so that step has what it needs.
        out += "SET";                                        out += '\t';
        out += Stamp(ts, "%Y-%m-%d %H:%M");                  out += '\t';
        out += JField(obj, "game_id");                       out += '\t';
        out += p1;                                           out += '\t';
        out += p2;                                           out += '\t';
        out += (w1 || w2) ? (s1 + "-" + s2) : JField(obj, "format");
        out += '\t';
        out += "";                                           out += '\t';
        // A HUB REFERENCE, not a path. The host recognises the prefix and
        // downloads before playing; putting a fake local path here would
        // fail later and further from the cause.
        out += first_file.empty() ? std::string()
                                  : ("hub:" + id + "|" + first_file);
        out += '\t';
        out += "";                                           out += '\t';
        out += "0";                                          out += '\t';
        out += "";                                           out += '\t';
        out += "";                                           out += '\t';
        out += std::to_string(w1 > w2 ? 1 : (w2 > w1 ? 2 : 0));
        out += '\n';
    }
    return out;
}

std::string FetchCommunityFile(const std::string& hub_host,
                               const std::string& ref) {
    // "hub:<id>|<file>"
    if (hub_host.empty() || ref.rfind("hub:", 0) != 0) return {};
    const size_t bar = ref.find('|');
    if (bar == std::string::npos) return {};
    const std::string id   = ref.substr(4, bar - 4);
    const std::string name = ref.substr(bar + 1);
    if (id.empty() || name.empty()) return {};

    // Cached next to the captures, so watching the same replay twice
    // costs one download. The hub is content-addressed by id, so a file
    // that is already here is the same file.
    const fs::path dir = fs::path("C:\\games\\captures\\community") / id;
    const fs::path dst = dir / name;
    std::error_code ec;
    if (fs::exists(dst, ec)) return dst.string();

    std::string body;
    if (!hostgui::HttpGet("https://" + hub_host + "/replay/get/" + id + "/" + name,
                          &body) || body.empty()) {
        return {};
    }
    fs::create_directories(dir, ec);
    // Written whole then renamed, so a half-downloaded file is never
    // handed to the player.
    const fs::path tmp = dst.string() + ".part";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return {};
        f.write(body.data(), (std::streamsize)body.size());
    }
    fs::rename(tmp, dst, ec);
    if (ec) return {};
    return dst.string();
}
