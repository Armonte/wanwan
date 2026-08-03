# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository.

## Project overview

Rollback netcode for Fighter Maker games. The launcher spawns the game and
injects a hook DLL that provides save-state rollback, online play, spectating
and replays. Windows 32-bit only, cross-compiled from WSL/Linux with mingw-w64
(i686) -- DLL injection plus the 32-bit game ABI are why. Native Linux is not
wired up.

Two engines are supported from one source list: **FM2K** (`FM2KHook.dll`) and
**FM95/CPW** (`FM95Hook.dll`, built from the same sources with `ENGINE_FM95=1`).
FM95 support is incomplete -- see `docs/FM95_Support_Status.md` for what works.

This is shipping software, not a prototype: stable is in the v0.2.8x range with
an auto-updater, a Discord-authed matchmaking hub, spectating and replays.

## Build

```bash
./make_build.sh   # configure + self-heal submodules (run once)
./build.sh        # compile; stripped binaries land in dist/
```

`./go.sh` is author-only convenience: it runs `build.sh` then copies into
`/mnt/c/games`. On a machine without that path it just builds. **The
contributor flow is `make_build.sh && build.sh`** -- do not tell people to run
`go.sh`.

Outputs: `FM2K_RollbackLauncher.exe`, `FM2KHook.dll`, `FM95Hook.dll`,
`FM2KUpdater.exe`. `build/` keeps unstripped copies with debug info for
symbolication and is incremental -- **do not `rm -rf` it between builds.**

Version is single-sourced in `scripts/make_version.sh`; `version_local.h` and
`version_rc.h` are generated build artifacts, gitignored, never edited by hand.

## Testing

```bash
tools/run_all_tests.sh          # determinism + netplay/spectator under loss
FULL=1 tools/run_all_tests.sh   # + multigame determinism sweep (slow)
tests/run.sh                    # host-native unit suite (doctest + standalone)
```

`run_all_tests.sh` is the gate quoted in commit messages. Know what it does and
does not cover before claiming "ALL GREEN": its stages are determinism, netplay
+ spectator E2E, the CSS gate self-test, ddraw redirect, multigame (FULL=1) and
FM95 (advisory). `tests/run.sh` is a separate host-compiler unit suite.

Determinism rules that have burned us before:
- Judge netplay by GekkoNet's per-frame desync term, **not** `.fm2krep` index
  diffs -- those are meaningless under rollback.
- Validate netplay changes at 2500+ frames under clumsy (~230ms/20% loss). A
  600-frame loopback run is only good enough to gate behavior-preserving
  refactors, which break immediately rather than at frame 2000.
- Replay determinism and live netplay rollback can trade off against each
  other. Test both paths.

## Layout

```
launcher/            core/ ui/ hub/ session/ discovery/ game/ net/ render/
FM2KHook/src/        core/ hooks/ netplay/ ui/ util/ locale/ audio/ vfs/ parity/
tests/               host-native unit tests (doctest + standalone mains)
tools/               harnesses, gates, extractors, one-off analysis scripts
scripts/             release cut/promote/version
hub/ bot/ relay/     nested separate repos (fm2k-hub, fm2k-bot); hub/ is gitignored here
studio/ editor/      2dfm Studio + editor RE work
old/ 2dfm/           reference-only source, NOT compiled -- see the READMEs there
docs/                private submodule (see below)
```

Key headers: `launcher/core/FM2K_Integration.h` (engine memory layout + config
structs; the launcher/UI class decls live in the sibling `*_decl.h` files) and
`FM2KHook/src/core/globals.h` (hook-side addresses, including the FM95 tables
transcribed from `launcher/core/FM95_Integration.h`).

Every compiled source is meant to stay **under 1000 lines**, split by concern
into sibling TUs sharing a `*_internal.h`. This is a real rule, not an
aspiration -- check before you grow a file.

## Conventions

- **Logging is quill (async). Never use blocking `printf`/`iostream` logging.**
- **No em dashes** anywhere in code, comments or commit messages -- use `--`.
- **Never `GetAsyncKeyState`.** Input must be focus-correct by construction:
  foreground-gate + `GetKeyboardState`; hotkeys via `GetKeyState`.
- User-visible launcher strings go through `T("key")` with entries in
  **all three** of `locales/{en,ja,es}.ini`. Do not hardcode English in the UI.
  `LoadIni` trims leading/trailing whitespace and decodes `\n`/`\t`/`\\`, so
  padding must be added in code, not baked into the string table.
- Release notes and commit summaries are **one line**.
- RE symbol hygiene: rename every `sub_`/`unk_`/`loc_` you understand to a
  behavior-based name before moving on, fix wrong prior guesses, save the IDB.

## Docs

`docs/` is a **private submodule** (`Armonte/wanwan-docs`) -- public clones see
an empty directory, and `make_build.sh` does not initialize it, so a contributor
without access is unaffected. Maintainers: `git submodule update --init docs`.

It holds the reverse-engineering and design corpus: `FM2K_Rollback_Research.md`
(engine analysis), `outline/` (per-system breakdowns), `dev/` (design + plan
docs), `FM95_Support_Status.md`. Prefer reading these over re-deriving from the
binary -- and prefer them over re-downloading anything already extracted.

## Tooling

IDA MCP is connected: read disassembly and original implementations at specific
offsets rather than guessing. Cross-reference against our docs. GekkoNet
examples live at `/mnt/c/dev/qoh/deps/GekkoNet/Examples`; MAME and DuckStation
sources are available locally for reference.

## Notes

- Process injection needs the hook DLL to be 32-bit, matching the game.
- Windows Defender may flag injection tooling.
- WSL drvfs makes `git status` glacial; `make_version.sh` already works around
  it (skips the dirty check). Don't reintroduce that cost.
