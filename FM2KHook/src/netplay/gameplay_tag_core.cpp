// Gameplay tag core -- see gameplay_tag_core.h for the region policy and the
// sprite-header evidence. Pure C++ (no Win32): compiled into the hook, the
// launcher and the host-native unit tests, so all three agree on the bytes.
#include "gameplay_tag_core.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#define XXH_INLINE_ALL
#include "../../../vendored/xxhash/xxhash.h"

namespace fm2k::gameplay_tag {

bool MemorySource::ReadAt(uint64_t off, void* dst, size_t n) {
    if (off > n_ || n > n_ - off) return false;
    if (n) std::memcpy(dst, p_ + off, n);
    return true;
}

namespace {

// bReadKgtCore's count limits (the engine refuses anything outside them).
constexpr int32_t kMaxScripts = 1024;
constexpr int32_t kMaxSteps   = 0x10000;
constexpr int32_t kMaxImages  = 0x2000;
constexpr int32_t kMaxSounds  = 256;
constexpr size_t  kScriptHdr  = 39;    // kgtSkillHeader
constexpr size_t  kStep       = 16;    // kgtSkill
constexpr size_t  kImageHdr   = 20;    // kgtImageHeader
constexpr size_t  kPalettes   = 0x2100;
constexpr size_t  kSoundHdr   = 42;    // kgtSound

// .kgt system block (after the dropped end-of-core DWORD), 0x1023C bytes.
constexpr size_t kSysBlock        = 0x1023C;
constexpr size_t kRosterSlots     = 50;
constexpr size_t kNameLen         = 256;
constexpr size_t kOffStageNames   = 20008;   // after chars, reactions, marker, recover
constexpr size_t kOffCommonImages = 58420;   // szCommonImageNames[200][32]: editor-only, excluded
constexpr size_t kOffSystemSkills = 64820;   // shSkillIdx* table: HASHED (spawns sim objects)

class Hasher {
public:
    explicit Hasher(FileKind k) {
        XXH3_64bits_reset(&st_);
        const uint8_t kb = (uint8_t)k;
        XXH3_64bits_update(&st_, &kb, 1);
    }
    // Length-prefixed so a byte moving across a region boundary still changes
    // the digest.
    void Region(const void* p, size_t n) {
        const uint64_t len = n;
        XXH3_64bits_update(&st_, &len, sizeof(len));
        if (n) XXH3_64bits_update(&st_, p, n);
    }
    uint64_t Digest() { return XXH3_64bits_digest(&st_); }
private:
    XXH3_state_t st_;
};

struct Walker {
    ByteSource& src;
    uint64_t off = 0;
    bool ok = true;

    bool Read(void* dst, size_t n) {
        if (!ok || !src.ReadAt(off, dst, n)) { ok = false; return false; }
        off += n;
        return true;
    }
    bool ReadVec(std::vector<uint8_t>& v, size_t n) {
        v.resize(n);
        return Read(v.data(), n);
    }
    bool I32(int32_t* v) { return Read(v, 4); }
    bool Skip(uint64_t n) {
        if (!ok || n > src.Size() || off > src.Size() - n) { ok = false; return false; }
        off += n;
        return true;
    }
};

// Walks the common KGT part (header .. end-of-core DWORD), hashing the
// gameplay regions into h. Leaves w.off at the start of the kind tail.
bool WalkCore(Walker& w, Hasher& h) {
    if (!w.Skip(16 + 256)) return false;   // signature + name: not hashed

    int32_t n = 0;
    std::vector<uint8_t> buf;
    if (!w.I32(&n) || n < 0 || n > kMaxScripts) return false;
    if (!w.ReadVec(buf, (size_t)n * kScriptHdr)) return false;
    h.Region(buf.data(), buf.size());

    if (!w.I32(&n) || n < 0 || n > kMaxSteps) return false;
    if (!w.ReadVec(buf, (size_t)n * kStep)) return false;
    h.Region(buf.data(), buf.size());

    // Images: header then data, data size exactly as bReadKgtCore computes
    // it -- iWidth*iHeight, +0x400 for an own palette, replaced by a non-zero
    // iSize. Nothing here is hashed.
    if (!w.I32(&n) || n < 0 || n > kMaxImages) return false;
    for (int32_t i = 0; i < n; ++i) {
        int32_t hdr[5];
        if (!w.Read(hdr, sizeof(hdr))) return false;
        uint64_t sz = (uint64_t)(uint32_t)hdr[1] * (uint32_t)hdr[2];
        if (hdr[3] & 1) sz += 0x400;
        if (hdr[4]) sz = (uint32_t)hdr[4];
        if (sz > 0xFFFFFFFFull) return false;
        if (!w.Skip(sz)) return false;
    }

    if (!w.Skip(kPalettes)) return false;

    if (!w.I32(&n) || n < 0 || n > kMaxSounds) return false;
    for (int32_t i = 0; i < n; ++i) {
        uint8_t hdr[kSoundHdr];
        if (!w.Read(hdr, sizeof(hdr))) return false;
        int32_t sz;
        std::memcpy(&sz, hdr + 36, 4);
        if (sz > 0 && !w.Skip((uint64_t)sz)) return false;
    }

    // End-of-core DWORD: read and dropped by the engine.
    return w.Skip(4);
}

bool CoreSignatureOk(ByteSource& src) {
    char sig[7];
    if (!src.ReadAt(0, sig, 7)) return false;
    return std::memcmp(sig, "2DKGT2K", 7) == 0 || std::memcmp(sig, "2DKGT2G", 7) == 0;
}

std::string SlotName(const uint8_t* p) {
    size_t n = 0;
    while (n < kNameLen && p[n]) ++n;
    return std::string((const char*)p, n);
}

// One pass for a .kgt: digest + roster.
bool KgtDigestAndRoster(ByteSource& src, uint64_t* digest, Roster* roster) {
    if (!CoreSignatureOk(src)) return false;
    Hasher h(FileKind::Kgt);
    Walker w{src};
    if (!WalkCore(w, h)) return false;
    std::vector<uint8_t> sys;
    if (!w.ReadVec(sys, kSysBlock)) return false;
    h.Region(sys.data(), kOffCommonImages);
    h.Region(sys.data() + kOffSystemSkills, kSysBlock - kOffSystemSkills);
    if (digest) *digest = h.Digest();
    if (roster) {
        roster->chars.clear();
        roster->stages.clear();
        for (size_t i = 0; i < kRosterSlots; ++i) {
            roster->chars.push_back(SlotName(sys.data() + i * kNameLen));
            roster->stages.push_back(
                SlotName(sys.data() + kOffStageNames + i * kNameLen));
        }
    }
    return true;
}

bool ExeDigest(ByteSource& src, uint64_t* out) {
    Hasher h(FileKind::Exe);
    const uint64_t total = src.Size();
    if (total == 0) return false;
    std::vector<uint8_t> chunk(1 << 16);
    // Whole file as ONE region (length prefix = file size), streamed.
    XXH3_state_t st;
    XXH3_64bits_reset(&st);
    for (uint64_t off = 0; off < total; ) {
        const size_t n = (size_t)std::min<uint64_t>(chunk.size(), total - off);
        if (!src.ReadAt(off, chunk.data(), n)) return false;
        XXH3_64bits_update(&st, chunk.data(), n);
        off += n;
    }
    const uint64_t body = XXH3_64bits_digest(&st);
    h.Region(&body, sizeof(body));
    *out = h.Digest();
    return true;
}

// ---- cache ---------------------------------------------------------------
struct CacheEntry {
    uint64_t size = 0, mtime = 0;
    FileKind kind = FileKind::Kgt;
    bool ok = false;
    uint64_t digest = 0;
    Roster roster;   // .kgt only
};
std::mutex g_mu;
std::map<std::string, CacheEntry> g_cache;
size_t g_hits = 0, g_misses = 0;

// Digest (and roster for a .kgt) of one file through the cache.
// Returns: 1 ok, 0 present but unparseable, -1 missing.
int CachedDigest(FileProvider& fp, const std::string& name, FileKind kind,
                 uint64_t* digest, Roster* roster) {
    std::string key;
    uint64_t size = 0, mtime = 0;
    if (!fp.Stat(name, &key, &size, &mtime)) return -1;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_cache.find(key);
        if (it != g_cache.end() && it->second.size == size &&
            it->second.mtime == mtime && it->second.kind == kind) {
            ++g_hits;
            if (!it->second.ok) return 0;
            *digest = it->second.digest;
            if (roster) *roster = it->second.roster;
            return 1;
        }
    }
    CacheEntry e;
    e.size = size; e.mtime = mtime; e.kind = kind;
    ByteSource* src = fp.Open(name);
    if (!src) return -1;
    if (kind == FileKind::Kgt) e.ok = KgtDigestAndRoster(*src, &e.digest, &e.roster);
    else                       e.ok = FileDigest(kind, *src, &e.digest);
    fp.Close(src);
    std::lock_guard<std::mutex> lk(g_mu);
    ++g_misses;
    if (e.ok) {
        *digest = e.digest;
        if (roster) *roster = e.roster;
    }
    const bool ok = e.ok;
    g_cache[key] = std::move(e);
    return ok ? 1 : 0;
}

void AppendLine(std::string& canon, const char* role, int id, int rc,
                uint64_t digest) {
    char line[64];
    if (rc == 1) {
        std::snprintf(line, sizeof(line), "%s|%d|%016llx\n", role, id,
                      (unsigned long long)digest);
    } else {
        std::snprintf(line, sizeof(line), "%s|%d|%s\n", role, id,
                      rc == 0 ? "unparseable" : "missing");
    }
    canon.append(line);
}

}  // namespace

bool FileDigest(FileKind kind, ByteSource& src, uint64_t* out) {
    if (kind == FileKind::Exe) return ExeDigest(src, out);
    if (kind == FileKind::Kgt) return KgtDigestAndRoster(src, out, nullptr);
    if (!CoreSignatureOk(src)) return false;
    Hasher h(kind);
    Walker w{src};
    if (!WalkCore(w, h)) return false;
    if (kind == FileKind::Player) {
        // Whole tail: commands, hit junctions, skill numbers + settings,
        // story section. The engine reads fixed blocks, so trailing bytes
        // beyond them are hashed too; editors do not write any.
        std::vector<uint8_t> tail;
        const uint64_t rest = src.Size() - w.off;
        if (rest > (64u << 20)) return false;
        if (!w.ReadVec(tail, (size_t)rest)) return false;
        h.Region(tail.data(), tail.size());
    }
    // Stage: tail is bgm_sound_id + zero padding -- nothing gameplay.
    *out = h.Digest();
    return true;
}

bool ParseKgtRoster(ByteSource& src, Roster* out) {
    return KgtDigestAndRoster(src, nullptr, out);
}

void Participants::Normalize() {
    std::sort(chars.begin(), chars.end());
    chars.erase(std::unique(chars.begin(), chars.end()), chars.end());
    std::sort(stages.begin(), stages.end());
    stages.erase(std::unique(stages.begin(), stages.end()), stages.end());
    // The SAME overflow rule on both sides: more ids than the header carries
    // means the tag covers the whole roster, and no ids are kept.
    if (whole_roster || chars.size() + stages.size() > kHdrMaxIds) {
        whole_roster = true;
        chars.clear();
        stages.clear();
    }
}

uint32_t ComputeTag(FileProvider& fp, const std::string& kgt_name,
                    const std::string& exe_name, Participants parts) {
    if (kgt_name.empty() || exe_name.empty()) return 0;
    parts.Normalize();

    uint64_t kgt_d = 0, exe_d = 0;
    Roster roster;
    if (CachedDigest(fp, kgt_name, FileKind::Kgt, &kgt_d, &roster) != 1) return 0;
    if (CachedDigest(fp, exe_name, FileKind::Exe, &exe_d, nullptr) != 1) return 0;

    std::vector<int> chars, stages;
    if (parts.whole_roster) {
        for (size_t i = 0; i < roster.chars.size(); ++i)
            if (!roster.chars[i].empty()) chars.push_back((int)i);
        for (size_t i = 0; i < roster.stages.size(); ++i)
            if (!roster.stages[i].empty()) stages.push_back((int)i);
    } else {
        for (uint8_t c : parts.chars)  chars.push_back(c);
        for (uint8_t s : parts.stages) stages.push_back(s);
    }

    std::string canon = "fm2k-gameplay-v2\n";
    AppendLine(canon, "kgt", 0, 1, kgt_d);
    AppendLine(canon, "exe", 0, 1, exe_d);
    canon.append(parts.whole_roster ? "roster|all\n" : "roster|ids\n");
    auto add = [&](const char* role, int id, const std::vector<std::string>& names,
                   const char* ext, FileKind kind) {
        uint64_t d = 0;
        int rc = -1;
        if (id >= 0 && (size_t)id < names.size() && !names[id].empty()) {
            rc = CachedDigest(fp, names[id] + ext, kind, &d, nullptr);
        }
        AppendLine(canon, role, id, rc, d);
    };
    for (int c : chars)  add("char",  c, roster.chars,  ".player", FileKind::Player);
    for (int s : stages) add("stage", s, roster.stages, ".stage",  FileKind::Stage);

    const uint64_t h = XXH3_64bits(canon.data(), canon.size());
    uint32_t tag = (uint32_t)(h ^ (h >> 32));
    if (tag == 0) tag = 1;   // 0 is the "unknown" sentinel
    return tag;
}

void ClearCache() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_cache.clear();
    g_hits = g_misses = 0;
}
size_t CacheHits()   { std::lock_guard<std::mutex> lk(g_mu); return g_hits; }
size_t CacheMisses() { std::lock_guard<std::mutex> lk(g_mu); return g_misses; }

void EncodeHeader(uint8_t* hdr, uint32_t tag, const Participants& in) {
    Participants p = in;
    p.Normalize();
    std::memset(hdr + kHdrOffTag, 0, 256 - kHdrOffTag);
    std::memcpy(hdr + kHdrOffTag, &tag, 4);
    hdr[kHdrOffFlags]   = p.whole_roster ? 1u : 0u;
    hdr[kHdrOffNChars]  = (uint8_t)p.chars.size();
    hdr[kHdrOffNStages] = (uint8_t)p.stages.size();
    size_t k = kHdrOffIds;
    for (uint8_t c : p.chars)  hdr[k++] = c;
    for (uint8_t s : p.stages) hdr[k++] = s;
}

bool DecodeHeader(const uint8_t* hdr, uint32_t* tag, Participants* p) {
    std::memcpy(tag, hdr + kHdrOffTag, 4);
    if (*tag == 0) return false;
    p->chars.clear();
    p->stages.clear();
    p->whole_roster = (hdr[kHdrOffFlags] & 1) != 0;
    const size_t nc = hdr[kHdrOffNChars], ns = hdr[kHdrOffNStages];
    if (nc + ns > kHdrMaxIds) {
        // Corrupt counts: fall back to the whole roster rather than reading
        // past the block (a wrong verdict here is still overridable).
        p->whole_roster = true;
        return true;
    }
    for (size_t i = 0; i < nc; ++i) p->chars.push_back(hdr[kHdrOffIds + i]);
    for (size_t i = 0; i < ns; ++i) p->stages.push_back(hdr[kHdrOffIds + nc + i]);
    return true;
}

}  // namespace fm2k::gameplay_tag
