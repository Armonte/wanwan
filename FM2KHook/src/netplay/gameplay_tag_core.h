#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// GAMEPLAY TAG (replay provenance v2) -- platform-neutral core.
//
// WHY A SECOND TAG
// ----------------
// content_tag (v1) hashes "<name>|<size>" of EVERY *.player and *.kgt in the
// folder. Any extra skin file, backup, or editor re-save that moves an RLE size
// anywhere in the folder flips it, so a palette mod blocked every replay of the
// game ("[different game files]", tester report, both installs "the same
// version"). Product rule: cosmetic mods -- palettes, recolored or re-encoded
// sprites, extra skin files, sounds -- must NEVER block a replay. Only
// gameplay-relevant differences may.
//
// v2 is therefore computed over the files the MATCH actually loads, and over
// only the gameplay-relevant bytes of each:
//   * the game's .kgt     (system file: roster, hitstop, system scripts)
//   * the game exe        (whole file -- the engine itself)
//   * each participating character's .player  (ids from the .kgt roster,
//     exactly like iOpenCharacterFile: "<szCharacterNames[id]>.player")
//   * each stage used     ("<szStageNames[id]>.stage", LoadStageFile @0x4041E0)
// An unrelated .player in the folder is never opened, so it cannot matter.
//
// REGION POLICY (layout: bReadKgtCore in the 2dfm community decomp, same exe;
// tools/kgt/{kgt,fm2nd}.py are the byte-exact oracle; fpk_reader.cpp names the
// same regions header / scripts / script_items / sprite headers / sprite
// contents / palettes / sounds / tail):
//
//   16B signature            EXCLUDED  the loaders read it and never check it
//                                      (kgt_core.cSignature: "not checked")
//   256B name                EXCLUDED  display name (netplay label, title)
//   scripts (39B headers)    HASHED    skill table: start index + flags
//   script_items (16B steps) HASHED    the move scripts: frame data, hitboxes,
//                                      movement, cancels -- the gameplay core
//   sprite headers (20B)     EXCLUDED  see below
//   sprite contents          EXCLUDED  pixels (+ the optional 1KB private
//                                      palette stored in front of them)
//   palettes (8 x 1056B)     EXCLUDED  colours
//   sounds (42B hdr + data)  EXCLUDED  audio never feeds the sim
//   end-of-core DWORD        EXCLUDED  read and dropped by bReadKgtCore
//   tail                     per kind:
//     .player  HASHED in full: commands (input motions), hit junctions, the
//              per-character common images (their x/y offsets drive the
//              thrown player, battle.c:3049-3054), CPU commands, the
//              standard skill numbers + character settings, story section.
//   Full region table with evidence: docs/dev/async_css/
//   sim_vs_presentation.txt (private docs submodule).
//     .kgt     HASHED (the 0x1023C-byte system block: roster + stage list,
//              hit reactions, hitstop, demo ids, game-mode settings, CSS
//              layout + per-slot eligibility -- replay playback re-runs CSS
//              from recorded inputs, so CSS geometry IS replay-relevant)
//              EXCEPT common_images[200] = kgtSystem.szCommonImageNames
//              (editor-only asset names: IDA insn scan finds zero references
//              to their in-memory range 0x4438AC..0x4451AC, tools/kgt/
//              README.md; the decomp has no reader either). NOTE the 264
//              bytes after it, which tools/kgt calls
//              "score_digit_sprites_array", are kgtSystem.shSkillIdx* --
//              the SYSTEM SKILL table (round start/end animation, KO, combo
//              digits, timer, gauges, CSS cursors). The engine spawns engine
//              objects from it (kgtSpawnNewEngineObjectReturnSkillField0x1,
//              engine.c:1721/1736/1957/2030), i.e. it shapes round flow and
//              object-pool occupancy, so it is HASHED.
//     .stage   EXCLUDED: bgm_sound_id + 1029 bytes of zero padding.
//   exe      HASHED whole.
//
// SPRITE HEADERS ARE NOT GAMEPLAY-RELEVANT (the decision, with evidence):
//   * IDA, WonderfulWorld_ver_0946.exe: the per-file image-header table
//     pointer (kgt_core +0x118) is dereferenced at runtime ONLY by
//     sprite_rendering_engine @0x40CC30 (0x40CC9E, 0x40CDFD), whose sole
//     caller is render_game @0x404E11. Every other +0x118 access is a loader
//     (player_data_file_loader @0x4039F0, character_data_loader @0x403600),
//     resource_cleanup_manager @0x403520, or an unrelated constant (the 280px
//     camera margin in camera_manager @0x40B1F1).
//   * Decomp: every kgtImageHeader iWidth/iHeight read lives in
//     vDrawCurrentEngineObject / vDrawKgtImage16 (engine.c), called from the
//     draw-layer walk AFTER vProcessEngineObjects has run the frame; battle.c
//     (vHitboxHandling, vAdjustHitboxes, vHitboxCalc) never touches an image
//     header -- hitboxes come from the script steps. The only object field the
//     draw path writes is iColor*, which it restores (after-image tint).
//   * So width/height/flags/size only decide WHERE pixels go, never a sim
//     value. The RLE size field must be excluded anyway or an editor re-save
//     would flip the tag; width/height go with it, and so does the image COUNT
//     (adding frames is cosmetic; a step pointing at a missing image only
//     draws nothing).
//
// Wire: the digest folds to 32 bits; 0 = "not recorded / unknown" and every
// consumer treats 0 as "allow", exactly like content_tag.
namespace fm2k::gameplay_tag {

enum class FileKind : uint8_t { Kgt = 1, Player = 2, Stage = 3, Exe = 4 };

// Random-access byte source. Files are parsed by seeking past sprite contents
// and sounds instead of reading them, so a cold replay-browser scan reads tens
// of KB per character instead of several MB.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual uint64_t Size() const = 0;
    // Exactly n bytes at off, or false.
    virtual bool ReadAt(uint64_t off, void* dst, size_t n) = 0;
};

class MemorySource final : public ByteSource {
public:
    MemorySource(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    uint64_t Size() const override { return n_; }
    bool ReadAt(uint64_t off, void* dst, size_t n) override;
private:
    const uint8_t* p_;
    size_t n_;
};

// Digest of the gameplay-relevant regions of one file (policy above).
// false = not a parseable FM2K file of that kind.
bool FileDigest(FileKind kind, ByteSource& src, uint64_t* out);

// The .kgt roster: raw (CP932, NUL-terminated) character and stage file stems,
// 50 slots each, in table order. Empty string = empty slot.
struct Roster {
    std::vector<std::string> chars;
    std::vector<std::string> stages;
};
bool ParseKgtRoster(ByteSource& src, Roster* out);

// Which roster entries a match (or a whole set) used.
struct Participants {
    std::vector<uint8_t> chars;    // indices into Roster::chars
    std::vector<uint8_t> stages;   // indices into Roster::stages
    bool whole_roster = false;     // id list overflowed the header: tag covers
                                   // every non-empty roster slot instead
    void Normalize();              // sort + dedupe (order-independent tag)
};

// Opens game files by their bare filename (CP932 bytes, as the engine builds
// them). The core never touches a filesystem itself.
class FileProvider {
public:
    virtual ~FileProvider() = default;
    // Cheap identity check for the digest cache: a stable key (e.g. absolute
    // path) plus size + mtime. false = file missing.
    virtual bool Stat(const std::string& name, std::string* key,
                      uint64_t* size, uint64_t* mtime) = 0;
    // Open for parsing; nullptr = missing / unreadable.
    virtual ByteSource* Open(const std::string& name) = 0;
    virtual void Close(ByteSource* src) = 0;
};

// Full v2 tag. kgt_name / exe_name are bare filenames. Returns 0 (unknown)
// when the .kgt or exe cannot be read or the .kgt does not parse. A
// participating .player/.stage that is missing is folded in as "missing", so
// an install lacking a character used in the match reads as different.
uint32_t ComputeTag(FileProvider& fp, const std::string& kgt_name,
                    const std::string& exe_name, Participants parts);

// Per-file digest cache keyed by (key, size, mtime). Process-wide, mutexed.
void ClearCache();
size_t CacheHits();     // diagnostics / tests
size_t CacheMisses();

// ---- Replay header block (FM2KSessionFileHeader bytes 212..255) ----------
// Layout, pinned by tests/test_gameplay_tag.cpp and
// test_spectator_protocol_sessionfile.cpp:
//   212 u32  gameplay_tag      0 = not recorded (pre-v2 producer or FM95)
//   216 u8   flags             bit0 whole_roster
//   217 u8   n_chars
//   218 u8   n_stages
//   219 u8   reserved
//   220 u8   ids[28]           n_chars char ids, then n_stages stage ids
//   248 u8   reserved[8]       still zero
// Old readers never look past 211, and the loader validates only magic +
// version, so SESSION_FILE_VERSION stays 2 (same argument as the provenance
// block above it).
constexpr size_t kHdrOffTag     = 212;
constexpr size_t kHdrOffFlags   = 216;
constexpr size_t kHdrOffNChars  = 217;
constexpr size_t kHdrOffNStages = 218;
constexpr size_t kHdrOffIds     = 220;
constexpr size_t kHdrMaxIds     = 28;

// Write tag + participants into a 256-byte header image. Overflow sets
// whole_roster and writes no ids.
void EncodeHeader(uint8_t* hdr256, uint32_t tag, const Participants& parts);
// false = no v2 tag recorded (tag field 0).
bool DecodeHeader(const uint8_t* hdr256, uint32_t* tag, Participants* parts);

}  // namespace fm2k::gameplay_tag
