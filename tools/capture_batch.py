#!/usr/bin/env python3
"""Batch the headless screenshot capture across a set of FM2K games.

Runs each game through the launcher's --offline path with the capture
env set, then reports which games actually yielded GAME-SPECIFIC front-end
frames as opposed to the engine's own boot cards.

The engine-frame filter is the point. Every 2DFM game boots through the
same sequence -- including a "Development by" card that belongs to the
engine, not the game -- so the first several front frames are byte-identical
across titles. Rather than hardcode "skip the first N", which breaks the
moment a game boots at a different speed, frames are hashed and any hash
seen in two or more games is dropped as engine-common.

Usage:
    tools/capture_batch.py --limit 8
    tools/capture_batch.py --games "C:\\path\\a.exe,C:\\path\\b.exe"
"""
import argparse, hashlib, json, os, random, re, shutil, subprocess, sys, time
from collections import Counter, defaultdict
from pathlib import Path

LAUNCHER = r"C:\games\FM2K_RollbackLauncher.exe"
OUT_ROOT = Path("/mnt/c/games/captures")
# Upper bound only. The run normally ends the moment the hook writes its
# .capture_done sentinel, which under turbo is a few seconds -- a fixed
# sleep would have thrown that saving away.
RUN_SECONDS = 45

ENV = {
    "FM2K_AUTO_CAPTURE": "1",
    # Drive the CSS and on into battle. The autoplay already randomises per
    # player -- its browse hash mixes player_id<<16 and its confirm hash
    # player_id<<8, both salted per match -- so P1 and P2 wander and lock in
    # independently without anything extra here. CSS_DWELL is how long they
    # browse before locking, which is also what makes the css_initial shot
    # show cursors in motion rather than both parked on slot 0.
    "FM2K_TURBO": "1",
    "FM2K_PARITY_AUTOPLAY": "1",
    "FM2K_AUTOPLAY_CSS_DWELL": "3",
    "FM2K_BOOT_TO_BATTLE": "0",
    "FM2K_AUTO_TITLE_SKIP": "0",
    "FM2K_SKIP_BOOT_CSS_PATCH": "1",
    # A RANDOM STAGE PER RUN. The hook's random-stage roll (random_stage.cpp,
    # the same one hub matches use) applies to any VS-mode launch when these
    # are set; the seed is replaced per game below, and MAX is clamped by
    # the hook to the game's real stage table.
    "FM2K_STAGE_RANDOM_MIN": "0",
    "FM2K_STAGE_RANDOM_MAX": "99",
    # Per-character portraits: walk the roster with RIGHT presses at CSS
    # entry and shoot each highlighted cell (char_NN.png). FM2K only.
    "FM2K_CAPTURE_CHARS": "1",
}


def safe_dirname(exe: Path) -> str:
    """Output directory name. Registry game_ids are ASCII slugs, so they are
    used directly when available -- which also satisfies the hook's
    requirement (screenshot.cpp) that the capture dir be ASCII."""
    reg = load_registry()
    hit = reg.get(_norm(exe.stem)) or reg.get(_norm(_strip_version(exe.stem)))
    if hit:
        return hit[0]
    return "x_" + _hashed_dirname(exe)


def _hashed_dirname(exe: Path) -> str:
    """ASCII-only directory name for a game's captures.

    THE HOOK CANNOT WRITE TO A NON-ASCII DIRECTORY, and it says so:
    screenshot.cpp notes that stb writes through the narrow C fopen, so a
    Japanese path mangles, and that "the launcher is responsible for picking
    an ASCII capture-dir for us". Naming the output after the game's own
    stem broke that contract for the ~15 of 108 games with Japanese exe
    names -- they produced ZERO files, not even boot.txt, which reads as
    "the game never launched" rather than "we handed the hook a path it
    cannot open". A short hash keeps the name unique and reversible via the
    report, which matters because several of these games differ only in
    characters that would otherwise be stripped to nothing.
    """
    stem = exe.stem
    ascii_part = "".join(c if (c.isalnum() or c in "._-") else "_" for c in stem
                         if ord(c) < 128).strip("_")
    digest = hashlib.sha1(str(exe).encode("utf-8")).hexdigest()[:8]
    return f"{ascii_part}_{digest}" if ascii_part else f"game_{digest}"


def win_path(p: Path) -> str:
    s = str(p)
    if s.startswith("/mnt/") and len(s) > 6:
        return s[5].upper() + ":" + s[6:].replace("/", "\\")
    return s


REGISTRY = Path(__file__).resolve().parent.parent / "games" / "registry.json"
LAUNCHER_CFG = Path.home() / ".." / ".."  # replaced below; see roots_from_cfg


def _norm(t: str) -> str:
    """Fold a title for matching: case, spacing and punctuation vary freely
    between a filename and a catalogue entry ("Battle Calling" vs the
    registry's fullwidth-dot form), and none of that variation is meaningful."""
    return "".join(ch for ch in t.lower() if ch.isalnum())


_VER_RE = re.compile(
    r"(?:[ _-]*(?:v(?:er(?:sion)?)?)?[ _-]*\d+(?:[._-]\d+)*[a-z]?)$", re.I)


def _strip_version(stem: str) -> str:
    """Drop a trailing version from an exe stem.

    Installs are versioned in the filename while the catalogue is not:
    RoHe_0_7_1, Time_and_Stopper_v02, Stay on Taboo v1_0, DragonPuppy_version01a.
    Without this, 31 real games fail to match an entry that plainly exists.
    """
    prev = None
    out = stem
    while out != prev:
        prev = out
        out = _VER_RE.sub("", out).rstrip(" _-")
    return out or stem


def load_registry():
    """stem -> (game_id, display name), from the curated registry.

    THE REGISTRY IS THE LIST, not whatever executables happen to be on the
    drive. Walking the tree and accepting any exe with the engine string in
    it cannot tell a game from a packer's output: the sweep ran AOB five
    times (packed original, Virtual Box output, a restored copy and two
    unpacked builds) and colors and 8 Marble 2X three times each, so "113
    games" was never 113 games. Worse, the Enigma-wrapped ones refuse
    injection and one of them threw a 0xc0000005 dialog.

    Matching against registry exe_stems fixes all of that structurally
    rather than by blocklist: 422 of the 450 entries carry stems, every
    stem is unique across games, and a packer artifact simply is not one.
    """
    reg = json.loads(REGISTRY.read_text(encoding="utf-8"))
    by_stem = {}
    for e in reg:
        gid = e.get("game_id")
        label = e.get("name") or gid
        # TWO WAYS IN, because only 422 of 450 entries carry exe_stems and
        # those cover a different subset of what is installed. Matching on
        # stems alone found 23 of 106 installed games; the other 83 are in
        # the registry under their NAME -- "8 Marbles Ura", "Angel Star
        # Road", "Another Force" all have entries, their exe just is not
        # listed. Names are folded (case, spaces, punctuation) because a
        # filename and a catalogue title disagree on all three without
        # meaning anything by it.
        for stem in e.get("exe_stems") or []:
            by_stem[_norm(stem)] = (gid, label)
        for alt in [label] + (e.get("alt_names") or []):
            by_stem.setdefault(_norm(alt), (gid, label))
    return by_stem


def roots_from_cfg():
    """Games roots exactly as the launcher stores them, one path per line."""
    appdata = os.environ.get("APPDATA_WSL")
    cfg = Path("/mnt/c/Users") / os.environ.get("WINUSER", "teo") / \
        "AppData/Roaming/FM2K/RollbackLauncher/launcher.cfg"
    roots = []
    if cfg.exists():
        for line in cfg.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip()
            if not line:
                continue
            if len(line) > 2 and line[1] == ":":
                line = "/mnt/" + line[0].lower() + line[2:].replace("\\", "/")
            roots.append(Path(line))
    return [r for r in roots if r.is_dir()]


# Unpacking artifacts are only used as a TIEBREAK now: the registry already
# excludes them, but when a game legitimately exists in several folders we
# prefer a path that does not look like tool output.
ARTIFACT_MARKERS = ("_packed", "_evb_out", "_restored", "_unpacked",
                    "_dump", "_orig", "_backup")


def is_artifact(exe: Path) -> bool:
    hay = (str(exe.parent) + "/" + exe.stem).lower()
    return any(m in hay for m in ARTIFACT_MARKERS)


def discover(limit):
    by_stem = load_registry()
    roots = roots_from_cfg()
    if not roots:
        print("no games roots in launcher.cfg")
        return []

    best = {}          # game_id -> (exe, is_artifact)
    unknown = set()
    for root in roots:
        for exe in root.rglob("*.exe"):
            try:
                sz = exe.stat().st_size
            except OSError:
                continue
            if not (64 * 1024 <= sz <= 8 * 1024 * 1024):
                continue
            hit = (by_stem.get(_norm(exe.stem))
                   or by_stem.get(_norm(_strip_version(exe.stem))))
            if not hit:
                # KEEP IT ANYWAY. The registry is curation, not a whitelist:
                # dropping a game because the catalogue has no entry would
                # lose more than the duplicate runs this change set out to
                # fix. Unmatched games run under a sanitised stem and are
                # reported, so gaps in the registry stay visible instead of
                # silently shrinking the sweep.
                if is_artifact(exe):
                    continue
                unknown.add(exe.stem)
                gid = "x_" + _hashed_dirname(exe)
            else:
                gid, _name = hit
            art = is_artifact(exe)
            cur = best.get(gid)
            # One run per GAME. Prefer a non-artifact path, then the
            # shallowest one -- a game's real install sits above its
            # unpacking scratch directories.
            if cur is None or (cur[1] and not art) or \
               (cur[1] == art and len(exe.parts) < len(cur[0].parts)):
                best[gid] = (exe, art)

    games = [v[0] for v in best.values()]
    games.sort(key=lambda p: p.stem.lower())
    print(f"{len(games)} game(s): {len(games) - len(unknown)} matched the "
          f"registry, {len(unknown)} not in it (kept, reported below)")
    if unknown:
        for u in sorted(unknown)[:10]:
            print(f"    not in registry: {u}")
    return games[:limit]


def run_one(exe: Path, out_dir: Path) -> float:
    # KEEP THE REVIEW. picks.txt is the owner's lead/ok/veto decision for
    # this game (host_captures.h); a re-run replaces the shots, not the
    # verdicts on the ones that survive.
    picks = out_dir / "picks.txt"
    saved = picks.read_bytes() if picks.exists() else None
    if out_dir.exists():
        shutil.rmtree(out_dir, ignore_errors=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    if saved is not None:
        picks.write_bytes(saved)
    env = dict(ENV)
    env["FM2K_STAGE_RANDOM_SEED"] = str(random.randint(1, 2**31 - 1))
    env_setup = "; ".join(f"$env:{k}='{v}'" for k, v in env.items())
    # ARGUMENT QUOTING, and it is not cosmetic. Start-Process joins
    # -ArgumentList with spaces and the receiving process re-parses the
    # result, so a path containing a space arrives split in two and the
    # launcher matches nothing -- the game never starts and the run leaves
    # an empty directory that looks exactly like a capture failure.
    # Measured: 6 of 8 games failed this way, and every one of them had a
    # space or a non-ASCII character in its path while both survivors were
    # plain ASCII. The path therefore needs embedded double quotes of its
    # own, inside the single-quoted PowerShell string.
    exe_arg = win_path(exe).replace('"', '')
    # Completion is detected from the OUTPUT, not the hook's sentinel.
    # battle.png is the last shot written, so its appearance means the run
    # has produced everything it is going to. The hook does write a
    # .capture_done sentinel, but it was not appearing here and chasing it
    # costs a rebuild cycle per attempt -- whereas the file we actually care
    # about is right there and cannot be wrong about its own existence.
    # The hook writes .capture_done once every state's series is complete
    # (front x6, css x3, battle x3); the last battle frame is the fallback
    # signal for a hook that predates the sentinel fix.
    done = out_dir / ".capture_done"
    done_alt = out_dir / "battle_03.png"
    ps = (
        f"{env_setup}; $env:FM2K_CAPTURE_DIR='{win_path(out_dir)}'; "
        f"$p = Start-Process -FilePath '{LAUNCHER}' "
        f"-ArgumentList '--offline','\"{exe_arg}\"' -PassThru; "
        f"$null = $p.Id"
    )
    subprocess.run(["powershell.exe", "-NoProfile", "-Command", ps],
                   capture_output=True, timeout=90)

    # Poll for the sentinel instead of sleeping a fixed span.
    t0 = time.time()
    while time.time() - t0 < RUN_SECONDS:
        if done.exists() or done_alt.exists():
            break
        time.sleep(0.5)
    elapsed = time.time() - t0

    kill = (f"Get-Process -Name 'FM2K_RollbackLauncher','{exe.stem}' "
            f"-ErrorAction SilentlyContinue | Stop-Process -ErrorAction SilentlyContinue")
    subprocess.run(["powershell.exe", "-NoProfile", "-Command", kill],
                   capture_output=True, timeout=60)
    return elapsed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=8)
    ap.add_argument("--games", default="")
    args = ap.parse_args()

    # --games takes Windows paths too (paths.tsv holds them): on this side
    # of WSL a "D:\..." string is one opaque filename, so its stem is the
    # whole path and the game lands in a hashed folder.
    def as_wsl(g: str) -> Path:
        g = g.strip().strip('"')
        if len(g) > 2 and g[1] == ":":
            g = "/mnt/" + g[0].lower() + g[2:].replace("\\", "/")
        return Path(g)
    games = ([as_wsl(g) for g in args.games.split(",") if g]
             if args.games else discover(args.limit))
    if not games:
        print("no games found")
        return 2

    names = {safe_dirname(e): e.stem for e in games}
    # Hand the exe -> capture-dir mapping to the transcode, which is what
    # turns thumbnails into something the launcher can look up. Resolving a
    # game to its directory means the registry and three passes of matching
    # rules; writing it down once here keeps that in ONE implementation.
    OUT_ROOT.mkdir(parents=True, exist_ok=True)
    with (OUT_ROOT / "paths.tsv").open("w", encoding="utf-8") as pf:
        for e in games:
            pf.write(f"{safe_dirname(e)}\t{win_path(e)}\n")

    print(f"{len(games)} game(s), ~{RUN_SECONDS}s each\n")
    OUT_ROOT.mkdir(parents=True, exist_ok=True)
    for i, exe in enumerate(games, 1):
        print(f"[{i}/{len(games)}] {exe.stem}", flush=True)
        secs = run_one(exe, OUT_ROOT / safe_dirname(exe))
        print(f"        {secs:.1f}s", flush=True)

    # Hash everything, then drop frames shared by two or more games.
    per_game = defaultdict(dict)
    counts = Counter()
    for d in sorted(OUT_ROOT.iterdir()):
        if not d.is_dir():
            continue
        for png in sorted(d.glob("*.png")):
            h = hashlib.sha1(png.read_bytes()).hexdigest()[:12]
            per_game[d.name][png.name] = h
        for h in set(per_game[d.name].values()):
            counts[h] += 1

    # THREE games, not two. Two versions of the SAME game legitimately share
    # front frames -- HHRTFGRelease01 and HHRTFGRelease02Hotfix are the same
    # title -- so a >=2 rule wrote both off as engine-common and reported
    # them as yielding nothing. Measured across this set: >=2 gives 5/8
    # games a unique front frame, >=3 gives 7/8, and >=4 also gives 7/8.
    # The result being flat from 3 upward is what says 3 is a real boundary
    # rather than a number tuned to this sample.
    engine = {h for h, n in counts.items() if n >= 3}
    print(f"\nengine-common frames (seen in 3+ games): {len(engine)}\n")

    ok = 0
    report = {}
    for game, files in sorted(per_game.items()):
        unique = {n: h for n, h in files.items() if h not in engine}
        fronts = [n for n in unique if n.startswith("front")]
        has_css = any(n.startswith("css") for n in unique)
        if fronts:
            ok += 1
        label = names.get(game, game)
        report[game] = {"game": label, "unique_front": sorted(fronts), "css": has_css}
        print(f"  {label[:34]:36s} front={len(fronts)} css={'y' if has_css else 'n'}")

    print(f"\n{ok}/{len(per_game)} games yielded a game-specific front frame")
    (OUT_ROOT / "report.json").write_text(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
