// Transforms between the games TSV and what the UI shows. See the header.
#include "host_library.h"
#include <functional>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace hostgui {

void RailName(const std::string& in, std::string* l1, std::string* l2) {
        // Split across the chip's two lines by DISPLAY COLUMNS, not bytes.
        // A byte budget makes Japanese titles three characters long, since
        // each is three bytes -- and it makes them too WIDE if you raise
        // the budget, because a full-width glyph occupies two columns of a
        // proportional line that Latin text fits four characters into.
        // Breaking on a space when one is available keeps English titles
        // from splitting mid-word; Japanese has no spaces to find, and
        // breaking anywhere is normal there.
        constexpr int kCols = 11;
        auto walk = [](const std::string& t, size_t from, int budget,
                       size_t* end, int* used) {
            size_t i = from;
            int cols = 0;
            while (i < t.size()) {
                const unsigned char c = (unsigned char)t[i];
                const size_t len = c < 0x80 ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
                const int w = c < 0x80 ? 1 : 2;
                if (cols + w > budget) break;
                cols += w;
                i += len;
            }
            *end = i;
            *used = cols;
        };

        l1->clear();
        l2->clear();
        size_t end = 0;
        int used = 0;
        walk(in, 0, kCols, &end, &used);
        if (end >= in.size()) { *l1 = in; return; }

        size_t cut = end;
        size_t rest_from;
        const size_t sp = in.find_last_of(' ', end);
        if (sp != std::string::npos && sp > 0 && sp * 2 > end) {
            // A space to break on: clean two-word split.
            cut = sp;
            *l1 = in.substr(0, cut);
            rest_from = cut;
        } else {
            // The FIRST WORD is wider than the line. Cutting at the column
            // limit and continuing it on line two orphans a letter --
            // "Hypothetica / l HRT Fig" -- so the word is elided instead
            // and line two starts at the word AFTER it, which is the part
            // that still carries information.
            size_t w_end = 0;
            int w_used = 0;
            walk(in, 0, kCols - 1, &w_end, &w_used);
            *l1 = in.substr(0, w_end) + "...";
            const size_t next = in.find(' ', end);
            if (next == std::string::npos) { l2->clear(); return; }
            rest_from = next;
        }
        while (rest_from < in.size() && in[rest_from] == ' ') ++rest_from;

        walk(in, rest_from, kCols, &end, &used);
        if (end >= in.size()) {
            *l2 = in.substr(rest_from);
        } else {
            walk(in, rest_from, kCols - 1, &end, &used);
            *l2 = in.substr(rest_from, end - rest_from) + "...";
        }
    };

    // The rail shows a bounded number of chips; the rest stay reachable
    // through Browse. 12 rather than 6 now that the list scrolls.

std::vector<std::string> RowTitles(const std::string& tsv) {
    std::vector<std::string> out;
    std::istringstream in(tsv);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        out.push_back(line.substr(0, line.find('\t')));
    }
    return out;
}

static std::vector<std::string> RowField(const std::string& tsv, size_t idx) {
    std::vector<std::string> out;
    std::istringstream in(tsv);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> f;
        size_t s = 0;
        for (;;) {
            const size_t t = line.find('\t', s);
            f.push_back(line.substr(s, t == std::string::npos ? t : t - s));
            if (t == std::string::npos) break;
            s = t + 1;
        }
        out.push_back(f.size() > idx ? f[idx] : std::string());
    }
    return out;
}

std::vector<std::string> RowPaths(const std::string& tsv)   { return RowField(tsv, 8); }
std::vector<std::string> RowEngines(const std::string& tsv) { return RowField(tsv, 3); }

std::vector<std::string> RowStems(const std::string& tsv) {
    std::vector<std::string> out;
    std::istringstream in(tsv);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> f;
        size_t s = 0;
        for (;;) {
            const size_t t = line.find('\t', s);
            f.push_back(line.substr(s, t == std::string::npos ? t : t - s));
            if (t == std::string::npos) break;
            s = t + 1;
        }
        out.push_back(f.size() > 7 ? f[7] : std::string());
    }
    return out;
}

std::string FilterByTitle(const std::string& tsv, const std::string& needle_in) {
    // Case-insensitive, because nobody types "Bishi Bashi" with the
    // capitals right and half these titles are CP932 anyway.
    std::string needle = needle_in;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    std::string out;
    std::istringstream in(tsv);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (needle.empty()) { out += line; out += '\n'; continue; }
        std::string hay = line.substr(0, line.find('\t'));
        std::transform(hay.begin(), hay.end(), hay.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (hay.find(needle) != std::string::npos) { out += line; out += '\n'; }
    }
    return out;
}

std::string RailRows(const std::vector<std::string>& titles,
                     const std::vector<std::string>& mru,
                     int max_chips,
                     const std::function<int(size_t)>& unread) {
    std::string out;
    int emitted = 0;
    std::vector<bool> used(titles.size(), false);
    auto emit = [&](size_t i) {
        std::string l1, l2;
        RailName(titles[i], &l1, &l2);
        out += std::to_string(i); out += '\t';
        out += l1;                out += '\t';
        out += l2;                out += '\t';
        out += std::to_string(unread ? unread(i) : 0);
        out += '\n';
        used[i] = true;
        ++emitted;
    };
    for (const std::string& t : mru) {
        if (emitted >= max_chips) break;
        for (size_t i = 0; i < titles.size(); ++i) {
            if (!used[i] && titles[i] == t) { emit(i); break; }
        }
    }
    // NO FILLER. The rail is the games you play -- most recent first --
    // and nothing else: padding it out of the library read as a list of
    // rooms the user never chose.
    return out;
}

namespace {

// Field 5 of a published row is the live online count.
int PeersOf(const std::string& line) {
    size_t s = 0;
    for (int f = 0; f < 5; ++f) {
        const size_t t = line.find('\t', s);
        if (t == std::string::npos) return 0;
        s = t + 1;
    }
    return std::atoi(line.c_str() + s);
}

void EmitHead(std::string* out, const std::string& head, const std::string& extra) {
    *out += head; *out += '\t';
    *out += extra; *out += "\t0\t-1\t-1\t-1\t-1\t-1\n";
}

int g_browse_cols = 3;

// A row always carries five slots (the UI's column repeater is fixed);
// only the first `g_browse_cols` are filled, the rest are -1 and render
// as nothing. The tile keeps its 320x240 and the ROW COUNT adapts.
void EmitTiles(std::string* out, const std::vector<int>& idx, bool dim) {
    const size_t cols = (size_t)std::max(1, std::min(5, g_browse_cols));
    for (size_t i = 0; i < idx.size(); i += cols) {
        *out += "\t\t";
        *out += dim ? "1" : "0";
        for (size_t k = 0; k < 5; ++k) {
            *out += '\t';
            *out += std::to_string(k < cols && i + k < idx.size() ? idx[i + k] : -1);
        }
        *out += '\n';
    }
}

}  // namespace

void SetBrowseColumns(int cols) { g_browse_cols = cols; }
int  BrowseColumns() { return g_browse_cols; }

std::string BrowseRows(const std::string& tsv,
                       const std::vector<std::string>& mru,
                       bool searching) {
    std::vector<std::string> titles;
    std::vector<int> peers;
    {
        std::istringstream in(tsv);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            titles.push_back(line.substr(0, line.find('\t')));
            peers.push_back(PeersOf(line));
        }
    }
    const int n = (int)titles.size();
    std::string out;
    if (n == 0) return out;

    if (searching) {
        // A filtered list is already the answer to a question; splitting it
        // into POPULAR and HIDDEN would be answering a different one.
        std::vector<int> all(n);
        for (int i = 0; i < n; ++i) all[i] = i;
        EmitHead(&out, "RESULTS", std::to_string(n) + " matching");
        EmitTiles(&out, all, false);
        return out;
    }

    // ── POPULAR: who is actually online, right now, from the hub ──
    std::vector<int> pop;
    for (int i = 0; i < n; ++i) if (peers[i] > 0) pop.push_back(i);
    std::sort(pop.begin(), pop.end(),
              [&](int a, int b) { return peers[a] > peers[b]; });
    if (!pop.empty()) {
        if (pop.size() > 10) pop.resize(10);
        EmitHead(&out, "POPULAR NOW",
                 "live -- " + std::to_string(pop.size()) + " of " + std::to_string(n));
        EmitTiles(&out, pop, false);
    }

    // ── YOUR ROOMS: the rail, in rail order ──
    std::vector<int> recent;
    for (const std::string& t : mru) {
        if (recent.size() >= 5) break;
        for (int i = 0; i < n; ++i) {
            if (titles[i] == t) { recent.push_back(i); break; }
        }
    }
    if (!recent.empty()) {
        EmitHead(&out, "YOUR ROOMS", "joined -- most recently played first");
        EmitTiles(&out, recent, false);
    }

    // ── HIDDEN GEMS: nobody is playing these ──
    //
    // Deterministic rather than random: a shuffle that changes on every
    // repaint is not a recommendation, it is a slot machine, and you can
    // never go back to the one you just saw. Seeded off the library size so
    // it moves when the library does.
    std::vector<int> quiet;
    for (int i = 0; i < n; ++i) if (peers[i] == 0) quiet.push_back(i);
    if (quiet.size() > 5) {
        unsigned seed = 0x9E3779B9u ^ (unsigned)n;
        std::vector<int> gems;
        for (int k = 0; k < 10 && !quiet.empty(); ++k) {
            seed = seed * 1664525u + 1013904223u;
            const size_t pick = seed % quiet.size();
            gems.push_back(quiet[pick]);
            quiet.erase(quiet.begin() + (long)pick);
        }
        char sd[32];
        std::snprintf(sd, sizeof sd, "seed 0x%04X", (unsigned)(0x9E3779B9u ^ (unsigned)n) & 0xFFFF);
        EmitHead(&out, "HIDDEN GEMS", sd);
        EmitTiles(&out, gems, false);
    }

    // ── everything, so nothing is only reachable through a mood ──
    std::vector<int> all(n);
    for (int i = 0; i < n; ++i) all[i] = i;
    EmitHead(&out, "ALL INSTALLED", std::to_string(n) + " games");
    EmitTiles(&out, all, false);
    return out;
}

}  // namespace hostgui
