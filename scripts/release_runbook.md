# Releasing FM2K — release_runbook.md (bleeding / dev / stable)

Authoritative runbook. Lives next to the scripts, tracked in-repo. The private
machine-local notes (checkout paths, ssh aliases) are in the gitignored
`RELEASING.md` at the repo root -- that file defers to this one for procedure,
so if the two ever disagree again, this one wins.

## Version is single-sourced

`scripts/make_version.sh` → `FM2K_VERSION="X.Y.Z"` is the **only** place a version
lives. It regenerates `version_local.h` (updater/about) and `version_rc.h`
(Windows VERSIONINFO) on **every build** (CMake pre-build stamp). Both are
**gitignored** — never edit or commit them; bump `make_version.sh` instead.

Tag = `vX.Y.Z` (stable) or `vX.Y.Z-bleeding` (bleeding). **The tag is derived
from `FM2K_VERSION`**, so:

> ⚠️ **Bump `FM2K_VERSION` before every cut.** `cut_release.sh` will try to
> create `v<version>[-bleeding]`; if that tag already exists the cut fails.
> This is the #1 footgun. Check what you're bumping FROM with
> `git tag --sort=-creatordate | head -3` rather than trusting a number
> written down here -- a hardcoded "last tag was X" goes stale immediately.

## Pre-cut gate — must be GREEN

```bash
tools/run_all_tests.sh           # determinism + netplay+spectator under loss
FULL=1 tools/run_all_tests.sh    # + multigame determinism sweep (slow)
```
Exit 0 = safe to cut. Also do the human check the tests can't: **launch a real
game, play a match, confirm nothing visibly broke.**

## Cut

```bash
# 1. bump the version (substitute the real from/to -- see the tag check above)
sed -i 's/FM2K_VERSION="<old>"/FM2K_VERSION="<new>"/' scripts/make_version.sh
# 2. commit (cut_release refuses a DIRTY tree — no phantom builds)
git commit -am "release: v<new>"
# 3. ensure the upload secret is baked (else crash/desync auto-upload ships OFF):
#    FM2K_LOG_UPLOAD_SECRET must be set — put it in ~/.config/fm2k-release.env
# 4. cut (builds via go.sh, packages, creates the GitHub release)
scripts/cut_release.sh --bleeding "changelog: what changed and why"
```

`cut_release.sh` publishes to **`Armonte/fm2ktest`** (the release repo the
updater watches), NOT this source repo. Auto-update triggers when its
`LatestVersion` is bumped (the script handles this for the channel).

## What ships / what to know
- Users on the old version **cannot match** the new one until they auto-update
  (rollback has a hard version-equality gate). A big commit delta = a hard
  cutover — say so in the changelog.
- Old `.fm2krep` replays / spectate streams may not play on a new build
  (savestate content is version-specific).
- **If the change touched the hub**: deploy `hub/hub.py` to the box running
  `hub.2dfm.org` (`ssh 2dfm-root`) — and roll security rules out **log-only
  first**. The hub is a **separate repo** (`Armonte/fm2k-hub`), not this one.

## Channels
- `--bleeding` → `vX.Y.Z-bleeding`, opt-in testers.
- `--dev` / (default `stable`) → `vX.Y.Z`; stable needs `FM2KTEST_REPO_DIR` set.

Promoting a soaked dev tag to stable (no rebuild):

```bash
scripts/promote_release.sh 0.2.83      # BARE version -- NO leading "v"
```

> ⚠️ The script adds the `v` itself (`gh release edit "v${VERSION}"`) and writes
> the **bare** version into `LatestVersion`. Passing `v0.2.83` therefore looks
> for tag `vv0.2.83` and would write `v0.2.83` into the file the stable
> auto-updater version-compares against. Bare version only.

## Gotchas checklist
- [ ] `FM2K_VERSION` bumped (tag doesn't already exist)
- [ ] working tree committed (cut refuses dirty)
- [ ] `tools/run_all_tests.sh` green + played a real match
- [ ] `FM2K_LOG_UPLOAD_SECRET` set (secret baked)
- [ ] hub deployed if hub changed
