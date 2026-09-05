# 2DFM Launcher -- Slint + SDL3 shell (i686)

Reproduces the ImGui prototype's app shell in Slint, at the size the
shipping launcher actually opens at. Not a hello-world: 1280x720, NERV
palette, persistent chrome (top bar / left nav rail / right rail / status
bar) around a routed content area, all nine views from the prototype.

```bash
rustup target add i686-pc-windows-gnu   # once
./build_host.sh && ./slint_sdl3_skeleton.exe
./slint_sdl3_skeleton.exe --bench 1920 1080 300   # full-redraw cost
./slint_sdl3_skeleton.exe --scale 2.0             # exercise the DPI path
```

Source of truth for layout, palette and view list is
`FM2K_LauncherUI_DesignSandbox.cpp` on branch `ui-imanim-test`
(commit 4d59b28), which is marked "preserved as reference, NOT for merge".
Geometry (rail 48 / right 154 / top 30 / status 18) and the palette are
lifted verbatim so the port does not quietly redesign what it is meant to
reproduce.

## Settled decisions

**32-bit.** Not a preference. The launcher injects via `CreateRemoteThread`
+ `GetProcAddress(kernel32, "LoadLibraryA")` (`FM2K_GameInstance.cpp:520`
and `:649`). A 64-bit process's `GetProcAddress` returns the *64-bit*
`LoadLibraryA`, which is not mapped in a WOW64 target. Going 64-bit means
replacing the load path with a shim, not retargeting the compiler.

**Software renderer, measured rather than assumed.** Full redraw, one core,
`RepaintBufferType::NewBuffer` so there is no partial-redraw help:

| size | avg | worst | share of a 60fps frame |
|---|---|---|---|
| 640x400 | 0.27ms | 1.08ms | 6% |
| 1280x800 | 0.61ms | 1.48ms | 9% |
| 1920x1080 | 0.92ms | 2.10ms | 13% |
| 2560x1440 | 1.45ms | 3.43ms | 21% |

And in an idle run, **1 frame in 200 redrew at all** -- `slint_ui_render`
returns 0 when nothing changed and the host skips the upload and present.
A GPU renderer would buy nothing measurable here and would cost a GL
context shared between SDL3 and Slint. The PovertyCaster session
independently force-disabled femtovg/skia/winit for the same reasons.

**SDL3 owns the window; Slint owns only the widget tree** and fills a
buffer via `MinimalSoftwareWindow`. This deletes the context handoff rather
than solving it, and nothing in `host.cpp` is Windows-specific, so it is
also the arrangement a native Linux build would use.

**No pixel fonts, and no font-family set at all.** The first version used
the prototype's retro stack (DotGothic16 + VT323). Measured on screen --
fraction of inked pixels that are mid-tone anti-alias fringe rather than
solid ink:

| | 12px | 14px | 16px | 20px | 24px | 32px |
|---|---|---|---|---|---|---|
| DotGothic16 | 90.8% | 88.9% | 92.7% | 66.2% | 57.9% | 37.6% |
| normal face | 69.9% | 63.1% | 65.4% | 56.9% | | |

**A dot-matrix face is not fixable by choosing the "right" size** -- 16px
measured WORSE than 14px, so there is no integer grid to land on. The cause
is in the outlines: its dot GAPS are 11 units on a 1000-unit em, i.e.
0.18px at 16px. The gaps that make it a dot font sit an order of magnitude
below one pixel, so the rasterizer averages every glyph into uniform grey;
they only resolve near 90px. It is a display face and nothing else.

So the UI names no font family. Slint then uses the system stack, which is
designed to be anti-aliased and ships a working CJK fallback -- and that
stack is what rendered our CP932 titles correctly in the first test, before
anything was embedded. Naming a family would trade that fallback for a hard
dependency on one machine's fonts.

Body copy also moved from `dim` (4.3:1 against the background) to `ink2`
(11.9:1). Low contrast makes anti-aliasing read as blur, independent of the
face.

**If a font is ever embedded**, `build.rs` must keep
`EmbedResourcesKind::EmbedFiles`: Slint's default is `AsAbsolutePath`,
whose own docs say "the files must still be present at the same path on the
machine running the application" -- a build-machine path baked into a
shipped exe. Note also that `EmbedForSoftwareRenderer`, which Slint's own
panic text recommends for the software renderer, embeds **only ASCII plus
characters that literally appear in .slint files** (`SLINT_FONT_SIZES` adds
sizes, not characters). It therefore cannot render game titles read from
disk, and is unusable for us.

## The blueprint

Layout, palette and view content come from
`docs/FM2K Rollback - Whole-Product Design.html` -- the packed design canvas,
whose `launcher.jsx` holds `GameLibrary`, `Lobby`, `Friends` and a tabbed
`Settings` (Display / Input / Net / Audio / Hub). Unpack it with the
manifest script blocks; the JSX is gzip+base64 inside
`<script type="__bundler/manifest">`. The ImGui prototype
(`FM2K_LauncherUI_DesignSandbox.cpp` on `ui-imanim-test`, commit 4d59b28) is
the same design rendered a second way and is the source for the chrome
geometry and the NERV palette.

## Nav: four views, not nine

LOBBY / BROWSE / REPLAYS / CONFIG.

RANK, EVENTS, STATS, CTRL MAP and PROFILE are **shelved, not deleted**. Each
is a window onto backend state that does not exist yet, and shipping an
empty leaderboard or a stats page of zeroes teaches users the app is hollow.
CTRL MAP folds into CONFIG, where input belongs beside audio/video/net.
They come back when there is something behind them.

**BROWSE is the picker; LOBBY is the room.** Choosing a game in BROWSE is
how you enter its lobby/channel, which is why BROWSE's primary actions are
LAUNCH OFFLINE and CREATE LOBBY rather than a bare "select".

The blueprint's header already reads "N TITLES INSTALLED / M AVAILABLE" --
the launcher is eventually meant to fetch games it does not have. Only the
installed half is real today, so only that half is stated. A filesystem
watch belongs with that work rather than before it: both are about the
library changing under you.

## Scenes

`scene` is above `view` and they are different things. `view` routes the
nine tabs INSIDE the shell; `scene` decides whether the shell is on screen
at all.

- **0 -- onboarding** (first run). No chrome, no nav, and no way out by
  clicking elsewhere, which is the point: the launcher cannot function
  until it is done.
- **1 -- shell.** The nine views.

Onboarding is four steps, and **only the first blocks**: Games Folders is
the one thing the launcher genuinely cannot work without -- no root means
no games, no replays, and the shipping launcher already refuses with
"Please add at least one valid games folder" (`locales/en.ini`). Display
name, controller and video are preferences changeable later, so they are
skippable and say so.

Two UX rules the steps follow, because both are cheap and both are
routinely got wrong: the step pips have FOUR states (done / current /
current-but-blocked / pending) so a stuck user can see there is a problem,
and a disabled NEXT says *why* it is disabled instead of only dimming.

The folder picker is a HOST concern. The Slint callback only raises a flag
and the host drains it next loop turn -- a native modal dialog must not be
opened from inside Slint's event dispatch, where it would pump a nested
message loop mid-update. SDL may deliver the result off-thread, so it is
parked under a lock and applied by the main loop.

## BASELINE -- the shell is not shippable without these

- [x] Slint builds + renders for i686, no winit, no GPU backend
- [x] SDL3 owns window/events/present; Slint renders to a buffer
- [x] Real window size (1280x720) and the prototype's chrome geometry
- [x] All nine views routed; lobby list with selection
- [x] CJK-capable font embedded, and a rule for which font carries data
- [x] Redraw-on-change (idle costs nothing)
- [x] Pointer input
- [x] **Scale/DPI** -- layout in logical coordinates, rasterization at
      physical. Verified with `--scale 2.0`: 2560x1440 physical against
      1280x720 logical, layout unchanged. The flag exists because you
      cannot otherwise exercise scale != 1.0 without a HiDPI monitor, and
      "the scale code is probably fine" is how blurry text ships.
      **Still unverified on a real HiDPI display** -- this dev machine is
      at 100%.
- [x] **Keyboard/text input** -- host forwards `SDL_EVENT_TEXT_INPUT` and
      mapped special keys. `SDL_StartTextInput` is required or nothing
      arrives; it is also what routes IME composition, so Japanese entry
      depends on it rather than on keycodes.
- [x] **Window resize** -- buffer and texture rebuild on
      `WINDOW_RESIZED` / `PIXEL_SIZE_CHANGED` / `DISPLAY_SCALE_CHANGED`.
      Verified by resizing to 884x521.
- [x] `-mwindows` -- ships as a GUI subsystem binary, no console.
- [x] **Scenes + first-run onboarding** -- 4 steps, native folder picker,
      real gamepad enumeration, text entry for the display name.
- [x] **Data boundary** -- real discovery across the FFI. The host walks
      the configured roots and sniffs each exe with the SAME test the
      shipping launcher uses (first 256 KB scanned for `KGT2KGAME` /
      `2DKGT2K` / `kgt2k.INI` / `KGT95GAME`, per
      `game_discovery_sniff.cpp:64`), so the spike lists exactly what the
      real launcher would. Measured on the dev library: 11 games found,
      engines correctly split FM2K/FM95. Roots and completion persist, so a
      returning user goes straight to the shell.

## Discovery

Reads the **real** launcher config so the skeleton lists what the shipping
launcher lists on this machine, with no setup:
`%APPDATA%\FM2K\RollbackLauncher\launcher.cfg`, one games root per line.
That file is treated as READ-ONLY -- a spike must not be able to corrupt the
real launcher's configuration, so roots added in onboarding go to the
spike's own file and the cache is written to a separate file, never the
launcher's `games.cache`.

Measured on the dev library (`D:\Games\fm2k`): **108 games**, names taken
from each `.kgt` project title, CJK included (`びしばし東方`).

- **Engine sniff** is the shipping launcher's own test
  (`game_discovery_sniff.cpp:64`), so the list matches what the real
  launcher would accept and reject.
- **Titles** come from the 256-byte CP932 project name at offset 16 of the
  `.kgt`, matching `ParseKgtSummary`. Without it every game shows as its
  exe stem. Companion `.kgt` is matched by stem first, then any in the
  folder -- a folder can hold several games (our URORFG folder holds four)
  and picking "any" would label them all alike.
- **Cache** keyed on path + size + mtime, including NEGATIVE results:
  most exes under a games root are not games, and re-reading 256 KB of each
  on every start is most of the cold-scan cost. Cold ~25s, warm under 3s.
- **Dedupe** by canonical path, so one game reachable through two roots is
  one row.
- **Folder column** disambiguates same-named games. A library legitimately
  holds several copies or versions; three rows all reading "colors" with
  nothing to tell them apart is worse than useless.

Remaining: two different exes in the SAME folder still look identical
(the exe stem would separate them), and there is no filesystem watch, so a
newly added game needs a restart.

## POLISH -- deliberately deferred

- Animations and view transitions (the prototype's ColdBoot sequence)
- The right rail's live content (ping wave, flag dots, region colors)
- Per-game accent colors (the prototype's kof98/3s/kof02 table)
- Style-guide screen, VGA swatch table
- Any view beyond LOBBY (all eight others are stubs on purpose)

## Open, not decided here

- Native Linux: waiting on the owner. Nothing above forecloses it -- a
  software renderer with no GL context is the same on both platforms.
