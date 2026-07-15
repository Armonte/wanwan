# kgt — 1:1 byte-for-byte round-trip parser for FM2K `.kgt` files

Python tool that parses FM2K project files, dumps them to JSON for
inspection / diffing / regression detection, and packs JSON back to the
original byte layout — **every byte accounted for, byte-exact**.

## Format coverage

Supports the FM2K family:
- `2DKGT2K\0` (modern: Wonderful World, ReSHUFFLE, vanpri, most)
- `2DKGT2G\0` (G-variant: AOB et al)

FM95 family (`KGTGAME\0`, `2DKGT95\0`) round-trips via the opaque-blob
fallback (`fm2nd.Fm95Opaque`) — 16B sig + 256B name + verbatim body.
Full FM95 byte-decoding is deferred (no public spec — see
`FM2K_KgtParser.cpp` for the slim name-extraction layout).

**Verified across 146 files (809 MiB) from `C:\games\2dfm`, `C:\dev\fm95`,
`C:\Users\teo\Downloads\_nicotine`, `D:\games\fm2k`:**
- 114 / 114 FM2K files: round-trip byte-exact
- 32 / 32 FM95 files: gracefully skipped
- 0 hard failures

## Quick start

```bash
# Summary
python3 kgt.py info "path/to/game.kgt"

# Verify round-trip
python3 kgt.py verify "path/to/game.kgt"

# Full JSON
python3 kgt.py parse "path/to/game.kgt" -o out.json

# Full JSON with semantic script-item decode (Xem's 28-Block catalog)
python3 kgt.py parse --decode-blocks "path/to/game.kgt" -o out.json

# Rebuild from JSON
python3 kgt.py pack out.json rebuilt.kgt

# Byte-level coverage map (every section labeled)
python3 kgt.py coverage "path/to/game.kgt"

# Extract assets — sprite frames as 8-bit BMP, sounds as WAV/MIDI,
# palettes as 16×16 swatch BMPs. Decompresses RLE-packed frames
# automatically. Frames without a private palette use shared palette 0
# (override with --palette 0..7).
python3 kgt.py extract "path/to/game.kgt" out_dir/
```

The `extract` command produces three sub-directories:
- `frames/0042_640x480.bmp` — one BMP per non-empty sprite-frame slot, named with `<index>_<width>x<height>.bmp`. 8-bit indexed Windows-3.x format with the embedded palette baked in (private palette if the frame has one, otherwise the shared palette you select with `--palette`).
- `palettes/0.bmp` — eight 16×16 swatch BMPs, one per shared palette.
- `sounds/0007_ファイト.wav` — WAV/MIDI/raw payload per sound entry, named with `<index>_<sound_name>.<ext>`. CP932/SJIS/GBK names preserved.

The RLE decompressor is ported from `Unity2dfmRuntime/Assets/Scripts/2dfmFile/DecompressUtil.cs` (4-opcode format: zero-fill / verbatim / byte-fill / back-reference, with 6-bit lengths and two-stage extension).

## Coverage on WonderfulWorld_ver_0946.kgt (4.48 MiB)

```
OFFSET         END       BYTES  SECTION
0x00000000  0x00000010      16  file_signature (sig+version)
0x00000010  0x00000110     256  project_name
0x00000110  0x00000114       4  scripts.count
0x00000114  0x00001509    5109  scripts[131]
…  
0x0046f7d4  0x0046f7d8       4  freeze_time_marker (always 2)
0x0046f7d8  0x0046f7d9       1  recover_time_gap
0x0046f7d9  0x0046f7da       1  block_hitstop_frames
0x0046f7da  0x0046f7db       1  hit_hitstop_frames
0x0046f7db  0x0046f7dc       1  collision_effect_type
0x0046f7dc  0x004729dc   12800  stage_infos[50] (g_stage_file_buffer)
…
0x0047a6e8  0x0047a7f0     264  score_digit_sprites_array (132 × u16)
0x0047a7f0  0x0047a80c      28  char_sel_config (14 × 2B)
0x0047a80c  0x0047a83e      50  player_selectable_infos[50]
0x0047a83e  0x0047abf0     946  reserved_tail_946 (all-zero pad)

file total:  4697072 bytes  (4.48 MiB)
accounted:   4697072  (100.0000%)
```

## Field naming policy

Where the 010 Editor template, the C++ `2dfmFileReader.cpp` axmol reader,
and the IDA disassembly of `WonderfulWorld_ver_0946.exe` disagreed about
the semantics of a byte, **IDA names win** — that's what the actual game
runtime reads. Specifically:

| 010 / C++ name | Our name | Why |
|----------------|----------|-----|
| `atkFreezeTime` (RecoverTimeConfig.attackRecoverTime) | `block_hitstop_frames` | IDA: `g_block_hitstop_frames`, used by `hit_detection_system` |
| `dfsFreezeTime` (RecoverTimeConfig.defenceRecoverTime) | `hit_hitstop_frames` | IDA: `g_hit_hitstop_frames` |
| `cclFreezeTime` (RecoverTimeConfig.clashRecoverTime) | `collision_effect_type` | IDA: `g_collision_effect_type`, used by `ProcessHitboxCollisions` |
| `unknownGapBytes[264]` ("位置数据") | `score_digit_sprites_array` (132 × u16) | IDA: `g_score_digit_sprites_array`, used by `DisplayScoreNumbers` |
| `unknown996Bytes[996]` | split: `player_selectable_infos[50]` + `reserved_tail_946` | C++ reader names first 50; remaining 946 always-zero pad |
| `ThrowReactionItem[200]` (010) | `common_images[200]` | Xem-confirmed: IDA insn scan finds zero references to in-memory range 0x4438AC..0x4451AC — editor-only image asset names, never read by runtime. First entry is `--Sin opciones--` (Spanish placeholder), inconsistent with hit-reaction naming. |
| `player_selectable_infos[50]` raw byte | `player_selectable_infos_bits` (2-bit decode) | Per Xem: bit 0 = enabled_for_story_mode, bit 1 = enabled_for_vs_mode. Verified corpus-wide: across 6050 slots in 121 files, ONLY values 0/1/2/3 appear; bits 2+ never set. Old C++ "3=selectable, 1=hidden/CPU" was a guess. Reality: 3=both, 2=vs-only, 1=story-only, 0=disabled. |
| `generalSettings` (int32) | `general_settings` + `general_settings_bits` | Added bitfield decode (ProjectBaseConfig union) |

## What's still labeled "unknown"

After our naming pass, only **3 bytes per file** carry "unknown" labels:
- `unknown_demo_id_1` and `unknown_demo_id_2` (2 bytes) — last 2 bytes of
  `GameDemoConfig`. C++ also calls them `unknownTag1/2`. No IDA xrefs.
- `recover_time_gap` (1 byte) — first byte of `RecoverTimeConfig`. No IDA
  xrefs in the WW runtime; LoadStageFile/WinMain/settings_dialog_proc do
  reference the *block* at this address but via the stageFileBuffer name,
  not this byte.

These 3 bytes are kept verbatim for round-trip. **Everything else is
named with a real semantic purpose**.

## JSON output schema

```jsonc
{
  "file_signature": "32444b4754324b000000000001000000",  // hex (sig + 8B version flags)
  "project_name": "...",                                  // hex with trailing NUL+garbage
  "project_name_str": "WonderfulWorld_ver_0946",          // decoded (cp932/gbk/latin-1)

  "scripts": [{ "name": "<hex>", "name_str": "...", "script_index": 0, ... }],
  "script_items": [...],
  "sprite_frames": [{ "width": 84, "height": 41, ..., "frame_content_b64": "..." }],
  "palettes": [...],   // always 8
  "sounds": [...],

  "player_name_gap": 0,
  "player_infos": [...],  // 50 × {raw: hex, name_str: ...}
  "reactions": [...],     // 200

  "freeze_time_marker": 2,           // always 2 in our corpus
  "recover_time_gap": 0,             // RecoverTimeConfig.gap (no IDA xrefs)
  "block_hitstop_frames": 4,         // g_block_hitstop_frames
  "hit_hitstop_frames": 4,           // g_hit_hitstop_frames
  "collision_effect_type": 20,       // g_collision_effect_type

  "stage_infos": [...],  // 50
  "demo_infos": [...],   // 100

  "title_demo_id": 1,
  "char_sel_for_1p_demo_id": 2,
  ...
  "unknown_demo_id_1": 3,            // GameDemoConfig.unknownTag1 — vestigial
  "unknown_demo_id_2": 0,            //   "                .unknownTag2 — vestigial

  "general_settings": 76,
  "general_settings_bits": {         // decoded ProjectBaseConfig
    "encrypt_game": false,
    "allow_clash": false,
    "enable_story_mode": true,
    "enable_1v1_mode": true,
    "enable_team_mode": false,
    "show_hp_after_bar": false,
    "press_to_start": true,
    "raw": 76
  },

  "throw_reactions": [...],  // 200

  "score_digit_sprites_array": "<hex>",        // 264B raw
  "score_digit_sprites_array_u16": [0,1,2,...],// 132 × u16 (semantic view)

  "char_sel_start_pos_x": 296, ... // 14 × i16/u16 fields

  "player_selectable_infos": [3,3,3,...,0,0],  // 50 bytes (CSS eligibility per slot)
  "reserved_tail_946": "<hex>"                 // 946 bytes (always zero in observed corpus)
}
```

## Use case (Xem's pattern: catching editor corruption)

```bash
# Snapshot before editing
python3 kgt.py parse work.kgt -o before.json

# …edit in the editor (Ctrl+S, add hit junction, etc.)…

# Diff in canonical form
python3 kgt.py parse work.kgt -o after.json
diff <(jq -S . before.json) <(jq -S . after.json) | head -200
```

JSON is sorted-key-friendly so `diff <(jq -S ...)` gives clean field-level
deltas. If a hex `_raw` blob diverges but the matching `_str` stays the
same, that's exactly the kind of editor-corruption signature you're
hunting.

## Semantic script-item decode

`--decode-blocks` adds a `script_items_decoded` array to the JSON
output. Each 16-byte script item is decoded into a typed Block dict
matching Xem85's [fm2ndparser](https://github.com/xem85/fm2ndparser)
catalog. 25 named opcode types covered (M, DS, S, O, E, RC, SF, SG,
SC, I, EB, GS, GL, RP, GC, R, FA, FD, PS, C, V, Rnd, Color, Com, AI)
plus a "settings" sentinel for the first block of each skill — 26
dispatch entries total. Verified on 124,341 blocks across 121 files:
zero unknown opcodes, zero decode errors. The original raw payload bytes are still preserved on
`script_items[*].payload`, so round-trip is byte-exact regardless of
whether `--decode-blocks` is set.

Block decoder lives in `blocks.py` and is reusable independently of
the top-level Kgt parser.

## `.player` / `.stage` / `.demo` support — `fm2nd.py`

The companion `fm2nd.py` adds round-trip parsers for the other three
FM2K project file types. Auto-dispatches by extension; same CLI shape
as `kgt.py`.

```bash
python3 fm2nd.py verify path/to/file.player
python3 fm2nd.py verify path/to/file.stage
python3 fm2nd.py verify path/to/file.demo
python3 fm2nd.py parse  --decode-blocks path/to/file.player -o out.json
python3 fm2nd.py info   path/to/file.demo
python3 fm2nd.py pack   out.json rebuilt.player
```

All three share the kgt common-resource section (signature, name,
scripts, script_items, sprite_frames, palettes, sounds) — code reused
via the `CommonHeader` dataclass. The tails:

- **`.stage`**: 8B (unknown_gap + bgm_sound_id i32) + 1029B zero pad
- **`.demo`**: 13B (unknown_gap1 + bgm_sound_id i16 + skip flag + ug2 + total_time u32) + 1024B zero pad
- **`.player`**: opaque trailing region (~40K bytes of structured
  character data — animations, hitboxes, AI; not currently decoded
  because no public reference documents it. The 010 template's
  `byte tail;` is grossly incomplete. We preserve the bytes verbatim
  for round-trip.)

`.player` decoding is a future research project — Unity's
`PlayerFileReader.cs` only reads the common section too.

## Sound modding — `soundtool.py`

Modder-facing tool to list / extract / replace sounds in
`.player` / `.stage` / `.demo` / `.kgt` files **without touching any
script block** — scripts reference sounds by index into the flat
`sounds[]` table, so an in-place payload swap is invisible to game logic.

```bash
python3 soundtool.py list    Char.player                     # every slot: type, size, WAV format, name
python3 soundtool.py extract Char.player -o out_dir/         # dump all sounds as WAV/MID
python3 soundtool.py extract Char.player --sound 7 -o s.wav  # one sound (index or name)
python3 soundtool.py replace Char.player --sound punch1.wav new.wav      # in-place, keeps Char.player.bak
python3 soundtool.py replace Char.player --sound 7 new.wav -o Out.player
```

Replacement WAVs are validated against the **engine's actual RIFF
walker** (`ParseRIFFWaveFormat` @0x416043 in WW 0946): RIFF/WAVE magic,
a truthful RIFF size field (the engine trusts it as its walk bound; an
overstated value reads out of bounds in-game), a `fmt ` chunk ≥ 14
bytes, and a `data` chunk. The fmt chunk is handed raw to DirectSound
`CreateSoundBuffer`, so non-PCM / 24-bit / >2-channel files are refused
unless `--force`. MIDI slots accept `.mid` (MThd check).

Safety rails: the file must round-trip byte-exact *before* editing
(aborts otherwise), and after editing the output is re-parsed and every
untouched sound is compared byte-for-byte. Replacing a sound with its
own extracted bytes produces a byte-identical file.

### Coverage

All three formats use the same `2DKGT2K\0` / `2DKGT2G\0` signatures as
`.kgt` — detection is by extension, not magic number. FM95
(`KGTGAME\0`, `2DKGT95\0`) is gracefully skipped.

**Full-corpus verification (7252 files across `C:\games\2dfm`,
`C:\dev\wanwan`, `D:\games\fm2k` — kgt + player + stage + demo):**

| Format    | Total | OK              | FM2K | FM95 | Fail |
|-----------|-------|-----------------|------|------|------|
| `.player` | 2494  | **2494 (100%)** | 2470 | 24   | 0    |
| `.stage`  | 1228  | 1227 (99.92%)   | 1227 | 0    | 1    |
| `.demo`   | 3418  | **3418 (100%)** | 3346 | 72   | 0    |
| `.kgt`    | 112   | **112 (100%)**  | 110  | 2    | 0    |
| **All**   | **7252** | **7251 (99.99%)** | 7153 | 98 | 1 |

The single hard failure is `Kakuhina (locked)/S02.stage` — a
**truncated download** from MyAbandonware. The stage's last sound
entry ("素子のテーマ", a 20,808,606-byte 44.1kHz stereo PCM WAV)
agrees with the internal RIFF chunk header on its size, but the file
ends 3,788,621 bytes short of that. Last bytes of the file are
mid-stream PCM samples, not any WAV trailer. The "(locked)" tag in
the directory name is from the archival site's metadata describing
the game itself as locked content, *not* a kgt format flag — sibling
stages S04 and S06 in the same directory round-trip fine. The other
file in this same `_NODEV/` tree all parse cleanly too. Treating the
failure as "missing input data" rather than "parser limitation":
genuine FM2K/FM95 coverage is 7251 of 7251 = 100%.

## Adding FM95 support

FM95 uses a 0x78D48-byte fixed-size game-system block with hard-coded
offsets (no count-prefixed sections). See `FM2K_KgtParser.cpp` lines
200-228 for the offsets:

```
g_kgt_data              file offset 0
g_player_file_name_array          0x110
g_stage_file_name_array           0x3FA8
g_demo_file_name_array            0x71A8
```

Detect via `KGTGAME\0` / `2DKGT95\0` signature, branch into a separate
`KgtFM95.parse()` path.
```
