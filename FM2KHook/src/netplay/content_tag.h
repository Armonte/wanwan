#pragma once

#include <cstdint>
#include <string>

// Game-install content TAG -- the provenance fingerprint stamped into replay
// file headers and checked before a replay is played back.
//
// WHY THIS IS NOT fm2k::game_hash::Compute()
// ------------------------------------------
// game_hash is the NETPLAY gate (#57, HELLO). It is switched OFF by default:
// Compute() returns 0 unless FM2K_HASH_CHECK is set (game_hash.cpp:120-133),
// because a canon-corruption bug produced spurious mismatches between peers
// with byte-identical installs. Stamping that into a replay header would write
// 0 for essentially every user, i.e. no provenance at all. Gating netplay and
// labelling a file are also different risk profiles: a wrong netplay verdict
// blocks a match, a wrong replay verdict blocks a replay the user can still
// force by other means.
//
// So this is a SEPARATE, always-on, deliberately cheaper fingerprint:
//   * DIRECTORY METADATA ONLY. Sorted, lowercase, UTF-8 "<name>|<size>" lines
//     over every *.player and *.kgt in the game folder plus the game's own
//     .exe. No file CONTENTS are read, so it costs one directory enumeration
//     and is safe to call on the battle-end write path and on a UI scan --
//     content-hashing a multi-MB .kgt on either would be a visible hitch.
//   * Consequently it catches the case that actually garbles a replay -- a
//     different roster, an added/removed character, a re-patched .kgt or .exe
//     whose size moved -- and does NOT catch a same-size in-place byte edit.
//     Call it a tag, not a hash, and say so in the UI.
//   * 0 means "could not enumerate / not recorded" and every consumer must
//     treat 0 as "unknown, allow", exactly like game_hash's 0 sentinel.
//
// Canonicalization matches game_hash.cpp's: wide -> UTF-8 conversion via Win32
// (never the ANSI narrowing that mangles JP filenames differently per locale)
// and lowercase ASCII folding, so two machines with the same files on disk
// produce the same tag regardless of locale.
//
// THE FILE SET IS SHARED BY CONSTRUCTION. ComputeForDir is the ONLY place the
// inclusion rule exists, and both sides call it: the hook stamps the tag into a
// replay header, the launcher recomputes it for that replay's game folder and
// blocks playback when the two disagree. If the two sides could disagree about
// WHICH files count, the gate would fire on installs that are actually
// identical -- so the rule is stated once, here, and admits no per-caller
// variation:
//     every *.player  +  every *.kgt  +  EXACTLY ONE .exe, the one the caller
//     names. Never "all the exes in the folder": installer leftovers, bundled
//     launchers, antimicrox and friends are precisely the noise that must not
//     change a game's identity, and which of them exist differs per machine.
// A caller that cannot name the game exe gets 0 (= unknown), which disables the
// gate, rather than a tag computed over a different file set.
namespace fm2k::content_tag {

// Tag for an arbitrary game directory. game_exe_name_w is the bare filename of
// the game's own executable ("wanwan.exe"). REQUIRED: null/empty returns 0.
uint32_t ComputeForDir(const wchar_t* dir_w, const wchar_t* game_exe_name_w);

// The launcher's half of "name the game exe": <kgt stem>.exe inside `dir`, and
// only if that file actually exists (otherwise empty -> the caller passes it on
// and gets tag 0 = unknown). This is the same file the launcher launches for a
// replay, which is why the hook's ComputeLocal -- which names its own running
// module -- resolves to the identical exe.
std::wstring ResolveGameExeName(const wchar_t* dir_w);

// Tag for the CURRENT process's own directory + own executable name. Cached
// after the first call. This is the hook-side entry point.
uint32_t ComputeLocal();

}  // namespace fm2k::content_tag
