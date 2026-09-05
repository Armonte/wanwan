// host_captures.cpp -- see host_captures.h.
#include "host_captures.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace hostgui {
namespace {

// The capture root the pipeline writes to. One place, shared with
// capture_pick.py's ROOT.
const fs::path kRoot = fs::path("C:\\games\\captures");

std::string ToWin(const fs::path& p) {
    std::string s = p.string();
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
}

// exe -> capture dir. paths.tsv (gid \t exe) is the pipeline's own map;
// index.tsv (exe \t thumb) is the fallback for a folder that predates it.
fs::path DirFor(const std::string& exe) {
    {
        std::ifstream f(kRoot / "paths.tsv");
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t t = line.find('\t');
            if (t == std::string::npos) continue;
            if (line.substr(t + 1) == exe) return kRoot / line.substr(0, t);
        }
    }
    std::ifstream f(kRoot / "index.tsv");
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t t = line.find('\t');
        if (t == std::string::npos) continue;
        if (line.substr(0, t) == exe) return fs::path(line.substr(t + 1)).parent_path();
    }
    return {};
}

struct Picks {
    std::string lead;
    std::vector<std::string> ok, veto;
};

Picks ReadPicks(const fs::path& dir) {
    Picks p;
    std::ifstream f(dir / "picks.txt");
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream in(line);
        std::string kw, name;
        in >> kw >> name;
        if (name.empty() || kw.empty() || kw[0] == '#') continue;
        if (kw == "lead") p.lead = name;
        else if (kw == "ok") p.ok.push_back(name);
        else if (kw == "veto") p.veto.push_back(name);
    }
    return p;
}

void WritePicks(const fs::path& dir, const Picks& p) {
    std::ofstream f(dir / "picks.txt", std::ios::trunc);
    f << "# reviewed in the launcher; capture_pick.py honours this file\n";
    if (!p.lead.empty()) f << "lead " << p.lead << "\n";
    for (const auto& n : p.ok) f << "ok " << n << "\n";
    for (const auto& n : p.veto) f << "veto " << n << "\n";
}

std::vector<std::string> Candidates(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".webp") == 0 &&
            n.find(".thumb.webp") == std::string::npos) {
            out.push_back(e.path().stem().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void Remove(std::vector<std::string>& v, const std::string& n) {
    v.erase(std::remove(v.begin(), v.end(), n), v.end());
}

// The game's line in index.tsv follows the lead. Rewritten in place so a
// review lands in the launcher's next scan without re-running the picker.
void UpdateIndex(const std::string& exe, const std::string& thumb_win) {
    const fs::path idx = kRoot / "index.tsv";
    std::vector<std::string> lines;
    {
        std::ifstream f(idx);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) lines.push_back(line);
        }
    }
    bool found = false;
    for (auto& l : lines) {
        const size_t t = l.find('\t');
        if (t != std::string::npos && l.substr(0, t) == exe) {
            l = exe + '\t' + thumb_win;
            found = true;
        }
    }
    if (!found && !thumb_win.empty()) lines.push_back(exe + '\t' + thumb_win);
    std::ofstream f(idx, std::ios::trunc);
    for (const auto& l : lines) f << l << "\n";
}

}  // namespace

std::string CaptureRows(const std::string& exe) {
    const fs::path dir = DirFor(exe);
    if (dir.empty()) return {};
    const Picks p = ReadPicks(dir);
    std::string out;
    for (const std::string& n : Candidates(dir)) {
        int state = 0;
        if (n == p.lead) state = 3;
        else if (std::find(p.ok.begin(), p.ok.end(), n) != p.ok.end()) state = 1;
        else if (std::find(p.veto.begin(), p.veto.end(), n) != p.veto.end()) state = 2;
        out += n + '\t' + ToWin(dir / (n + ".thumb.webp")) + '\t' + std::to_string(state) + '\n';
    }
    return out;
}

std::string CaptureThumbFor(const std::string& exe) {
    const fs::path dir = DirFor(exe);
    if (dir.empty()) return {};
    const Picks p = ReadPicks(dir);
    if (!p.lead.empty()) return ToWin(dir / (p.lead + ".thumb.webp"));
    if (!p.ok.empty()) return ToWin(dir / (p.ok.front() + ".thumb.webp"));
    return {};
}

std::string CaptureSlideshow(const std::string& exe) {
    const fs::path dir = DirFor(exe);
    if (dir.empty()) return {};
    const Picks p = ReadPicks(dir);
    std::string out;
    int n = 0;
    for (const std::string& name : p.ok) {
        if (name == p.lead) continue;
        const fs::path t = dir / (name + ".thumb.webp");
        std::error_code ec;
        if (!fs::exists(t, ec)) continue;
        if (!out.empty()) out += '|';
        out += ToWin(t);
        if (++n == 4) break;
    }
    return out;
}

bool CaptureAction(const std::string& key, const std::string& exe) {
    const size_t colon = key.find(':');
    if (key.rfind("cap.", 0) != 0 || colon == std::string::npos) return false;
    const std::string verb = key.substr(4, colon - 4);
    const std::string name = key.substr(colon + 1);
    const fs::path dir = DirFor(exe);
    if (dir.empty() || name.empty()) return false;
    Picks p = ReadPicks(dir);
    if (verb == "lead") {
        if (p.lead == name) { p.lead.clear(); }
        else { p.lead = name; Remove(p.veto, name); Remove(p.ok, name); }
    } else if (verb == "ok") {
        Remove(p.veto, name);
        if (p.lead == name) p.lead.clear();
        if (std::find(p.ok.begin(), p.ok.end(), name) == p.ok.end()) p.ok.push_back(name);
        else Remove(p.ok, name);
    } else if (verb == "veto") {
        Remove(p.ok, name);
        if (p.lead == name) p.lead.clear();
        if (std::find(p.veto.begin(), p.veto.end(), name) == p.veto.end()) p.veto.push_back(name);
        else Remove(p.veto, name);
    } else if (verb == "clear") {
        Remove(p.ok, name); Remove(p.veto, name);
        if (p.lead == name) p.lead.clear();
    } else {
        return false;
    }
    WritePicks(dir, p);
    // The tile follows: the lead, else the first approved, else leave the
    // heuristic's line alone (the picker only ever loses to a decision).
    const std::string thumb = CaptureThumbFor(exe);
    if (!thumb.empty()) UpdateIndex(exe, thumb);
    return true;
}

}  // namespace hostgui
