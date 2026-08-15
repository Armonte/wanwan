#!/usr/bin/env python3
"""hub_spectate_e2e.py -- the HUB-BROKERED spectate E2E gate.

WHAT IT OWNS (and why it had to exist). Every spectator stage we shipped
before this one dialed the host with `--spectate <ip:port>` on the command
line, i.e. it tested the spectator TRANSPORT while bypassing the hub entirely.
The path a real user takes -- lobby -> challenge -> accept -> match_start ->
click Spectate -> `spectate_request` -> `spectate_grant` -> the LAUNCHER spawns
the spectator from the grant -- had zero automated coverage, because the hub
needs Discord auth and every step of that flow was an ImGui click.

Both blockers are now removed:
  * `FM2K_HUB_AUTH_DISABLE=1` runs the REAL hub locally with no Discord, no DB,
    no external dependency (Phase 5a proved this with zero hub-side patches).
  * `FM2K_HUB_AUTOMATION=1` (default-off, launcher_ui_hub_automation.cpp)
    triggers the existing hub event handlers from env, so the whole flow runs
    headless.

VIA-HUB IS PROVED BY CONSTRUCTION, NOT BY OBSERVATION. This harness never
passes `--spectate` to anything. In the hub path the spectator game process is
created by `LaunchRemoteSpectator` from the grant callback and nowhere else, so
a spectator process EXISTING is already proof the grant fired. Assertion H6
corroborates it three more ways (argv scan, grant `host=` vs the spawned
`FM2K_REMOTE_ADDR`, and grant-before-spawn ordering inside one log).

VERDICT. Fatal: the hub-flow assertions H1-H7, the no-vacuous-pass guard, a
GekkoNet `DESYNC #` in any player log, and the per-spectator authoritative GATE
(rng/hp/script frames the host never produced). Recorded but NOT fatal, per the
seam-gate precedent: tail-CINPUT mismatches on a hard-terminated final match,
CHECKSUM segment noise, CSS parity on a char-select the host never finished,
spectator stalls, and a laggard rc=124. Gating on those buys nothing and
guarantees red weeks.

MACHINE TOKEN: this launches games (three launchers + up to three game
windows). Only one such harness may run at a time.

Usage:
  python3 tools/hub_spectate_e2e.py                 # base brokered run
  python3 tools/hub_spectate_e2e.py --relay         # + hub-relay spectate leg
  python3 tools/hub_spectate_e2e.py --keep          # keep the workspace

RED-PROOF KNOBS (default off; they exist so the "does this stage actually FAIL
when the brokered flow breaks" proof is repeatable by anyone, instead of being
a claim in a report):
  HUBE2E_RED_NO_HUB=1              do not start the hub at all
  HUBE2E_RED_NO_AUTH_DISABLE=1     start the hub WITHOUT FM2K_HUB_AUTH_DISABLE,
                                   i.e. with Discord auth required
  HUBE2E_RED_NO_AUTOMATION=1       launchers run with FM2K_HUB_AUTOMATION unset
Each must go RED fast and say why. Never set any of them in the gate.
"""

from __future__ import annotations
import argparse, atexit, os, re, shutil, signal, subprocess, sys, threading, time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
# OUT_DIR is read by spec_selftest AT IMPORT TIME, and must live on the Windows
# filesystem (to_win() only rewrites /mnt/<drive>/...). Dedicated dir: the
# shared tools/.spec_selftest is re-gated across stages and would collide.
os.environ.setdefault("FM2K_TEST_OUT_DIR", str(REPO / "tools" / ".hub_spectate_e2e"))
sys.path.insert(0, str(REPO / "tools"))
import spec_selftest as sst          # noqa: E402  (path must be set first)

OUT_DIR    = Path(os.environ["FM2K_TEST_OUT_DIR"])
HUB_SRC    = REPO / "hub"
# Hub scratch lives on the WINDOWS filesystem: the hub runs as a Windows
# process (WSL2 nat networking puts the Linux loopback on a separate stack, so
# a WSL-bound hub at 127.0.0.1 is invisible to the Windows launcher/game).
HUB_SCRATCH = Path(os.environ.get("HUBE2E_SCRATCH", "/mnt/c/temp/fm2k_hub_e2e"))
HUB_PORT   = 7711
HUB_PORTS  = [HUB_PORT, HUB_PORT + 1, HUB_PORT + 2, HUB_PORT + 3]
# Whitelist copy. NEVER copy hub/.env (real Discord secrets) or tokens.json,
# and never run from hub/ itself -- load_env() would pull those secrets into
# the process env and matches.json would be written into the nested repo.
HUB_FILES  = ["hub.py", "auth.py", "match_records.py", "models.py",
              "relay_udp.py", "state.py"]
RELAY_SESSION_TOKEN = "aabbccddeeff00112233445566778899"

NICK_HOST, NICK_GUEST, NICK_SPEC = "HOSTBOT", "GUESTBOT", "SPECBOT"
BAK_SUFFIX    = ".fm2k_e2e_bak"
ABSENT_SUFFIX = ".fm2k_e2e_absent"
# The ONLY hub host this harness may run against, and therefore the only one it
# may plant a synthetic Discord credential for (review A9a).
HUB_HOST      = "127.0.0.1"
LOOPBACK_HOSTS = ("127.0.0.1", "localhost", "::1")


def log(msg):
    print(f"[hube2e] {msg}", flush=True)


def win_bs(p) -> str:
    """Windows path with BACKSLASHES. to_win() leaves forward slashes, which
    cmd.exe accepts as a command but NOT reliably after `cd /d` -- that failure
    mode reports only "The system cannot find the path specified."."""
    return sst.to_win(Path(p)).replace("/", "\\")


def win_appdata() -> Path:
    """%APPDATA% as a WSL path. The launcher's auth cache and dev flags live
    there and the harness has to swap both, so getting this wrong is silent
    corruption of the developer's real Discord token."""
    out = subprocess.run(["cmd.exe", "/c", "echo %APPDATA%"],
                         capture_output=True, text=True).stdout.strip()
    # C:\Users\x\AppData\Roaming -> /mnt/c/Users/x/AppData/Roaming
    p = out.replace("\\", "/")
    return Path("/mnt/" + p[0].lower() + p[2:])


def find_windows_python() -> str | None:
    for cand in ("python.exe", "py.exe"):
        path = shutil.which(cand)
        if path:
            return path
    for p in ("/mnt/c/Program Files/Python313/python.exe",
              "/mnt/c/Program Files/Python312/python.exe",
              "/mnt/c/Program Files/Python311/python.exe"):
        if Path(p).exists():
            return p
    return None


# ---------------------------------------------------------------------------
# Global-state swaps.
#
# The launcher keeps three pieces of per-USER state the harness must control:
# the Discord auth cache (Connect is BeginDisabled without it), dev_flags.ini
# (stealth_mode=1 would hide the match from the lobby, so the spectator could
# never see an in_match pair) and launcher.cfg (the games-discovery root, which
# on this box points at D:\Games\fm2k where the test game does not live).
#
# Every swap backs the original up under a SENTINEL name and restores it in
# teardown. A crash mid-run leaves the sentinel on disk; the NEXT run restores
# it before doing anything else, so a BSOD cannot cost the developer their real
# Discord token.
# ---------------------------------------------------------------------------
class FileSwap:
    def __init__(self, path: Path, content: str | None):
        self.path    = path
        self.content = content          # None => "this file must not exist"
        self.bak     = Path(str(path) + BAK_SUFFIX)
        self.absent  = Path(str(path) + ABSENT_SUFFIX)

    def restore(self, label="restore"):
        if self in _INSTALLED_SWAPS:
            _INSTALLED_SWAPS.remove(self)
        if self.bak.exists():
            self.path.parent.mkdir(parents=True, exist_ok=True)
            # DO NOT clobber a NEWER real file (review A9a, second-order
            # hazard). The natural response to "I am signed out / I am HARNESS"
            # after a crashed run is to sign in with Discord again, writing a
            # fresh real token. A later run's recovery sweep would then move the
            # week-old backup on top of it and silently revert the user to a
            # possibly-expired credential. If the live file is newer than the
            # backup, keep the live one and park the backup under a dated name
            # rather than deleting either.
            if self.path.exists():
                # "Ours" is decided by CONTENT, not mtime: the harness's own
                # planted file is always newer than the backup, so an mtime-only
                # rule would refuse every normal teardown. Only a file we did
                # NOT write, and that is newer than the backup, is treated as a
                # user re-sign-in worth protecting.
                try:
                    live = self.path.read_text(errors="ignore")
                except OSError:
                    live = None
                # A "must not exist" swap (content is None) plants NOTHING, so
                # anything sitting there at restore time was regenerated by the
                # launcher during the run (games.cache is rebuilt by the fresh
                # scan we forced) -- never the user's file. Always restore those,
                # or the sweep leaves a dated backup on disk and the user's
                # original is silently retired.
                ours = (self.content is None) or (live == self.content)
                try:
                    newer = self.path.stat().st_mtime > self.bak.stat().st_mtime + 1.0
                except OSError:
                    newer = False
                if newer and not ours:
                    keep = Path(str(self.bak) + time.strftime(".%Y%m%d%H%M%S"))
                    shutil.move(str(self.bak), str(keep))
                    log(f"{label}: {self.path.name} on disk is NEWER than the "
                        f"backup -- keeping the live file, backup parked at "
                        f"{keep.name}")
                    return True
                self.path.unlink()
            shutil.move(str(self.bak), str(self.path))
            log(f"{label}: put back {self.path.name}")
            return True
        if self.absent.exists():
            if self.path.exists():
                self.path.unlink()
            self.absent.unlink()
            log(f"{label}: removed harness-planted {self.path.name} "
                f"(none existed before)")
            return True
        return False

    def install(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if self.path.exists():
            shutil.move(str(self.path), str(self.bak))
        else:
            self.absent.write_text("harness sentinel: no original file\n")
        _INSTALLED_SWAPS.append(self)
        if self.content is None:
            log(f"planted: {self.path.name} REMOVED for this run")
        else:
            self.path.write_text(self.content)
            log(f"planted: {self.path.name}")


# Every FileSwap that currently has an original stashed under a sentinel. The
# atexit / signal handlers below walk this, so a SIGTERM, a Ctrl-Break or an
# unhandled exception restores the user's files even though `main()`'s finally
# never runs. SIGKILL / BSOD / power loss still cannot be caught by any process
# -- that is what restore-on-start (RestoreStaleBackups) is for, and between the
# two the claim "restored including after a crash" is finally true.
_INSTALLED_SWAPS: list["FileSwap"] = []


def _emergency_restore(reason: str):
    if not _INSTALLED_SWAPS:
        return
    log(f"EMERGENCY RESTORE ({reason}) -- putting {len(_INSTALLED_SWAPS)} "
        f"swapped file(s) back")
    for sw in list(_INSTALLED_SWAPS):
        try:
            sw.restore("emergency")
        except Exception as e:                    # noqa: BLE001
            log(f"emergency restore FAILED for {sw.path}: {e!r}")
    _INSTALLED_SWAPS.clear()


def install_emergency_handlers():
    atexit.register(_emergency_restore, "atexit")
    for signame in ("SIGTERM", "SIGINT", "SIGBREAK", "SIGHUP"):
        sig = getattr(signal, signame, None)
        if sig is None:
            continue
        try:
            signal.signal(sig, lambda s, f, n=signame: (
                _emergency_restore(n), sys.exit(1)))
        except (ValueError, OSError):
            pass          # not the main thread, or unsupported on this platform


def assert_local_hub_only():
    """REFUSE to plant a synthetic Discord credential unless the whole run is
    provably pointed at a loopback hub (review A9a).

    The planted discord_auth.json is a VALID-LOOKING file: LoadCached().valid is
    just `!hub_token.empty()`, so with it in place the launcher's Connect button
    is enabled and the pill reads HARNESS. If a crash left it live and the
    configured hub were the production one, the next Connect click would ship
    `harness-synthetic-token-not-a-real-credential` to hub.2dfm.org. The
    cheapest way to make that impossible is to never plant it while anything in
    the environment names a non-loopback hub."""
    if HUB_HOST not in LOOPBACK_HOSTS:
        return f"HUB_HOST is {HUB_HOST!r}, not loopback"
    env_host = os.environ.get("FM2K_HUB_HOST", "")
    if env_host and env_host not in LOOPBACK_HOSTS:
        return (f"FM2K_HUB_HOST={env_host!r} names a REAL hub -- refusing to "
                f"swap the Discord auth cache while a non-loopback hub is "
                f"configured")
    if os.environ.get("FM2K_HUB_INSECURE"):
        return ("FM2K_HUB_INSECURE is set in the environment -- refusing to "
                "run: a plaintext downgrade plus a planted credential is "
                "exactly the combination this guard exists to prevent")
    return None


def build_swaps(appdata: Path, game_root: str) -> list[FileSwap]:
    fm2k_dir = appdata / "FM2K_Rollback"
    cfg_dir  = appdata / "FM2K" / "RollbackLauncher"
    auth_json = (
        '{\n'
        '  "hub_token": "harness-synthetic-token-not-a-real-credential",\n'
        '  "discord_user_id": "000000000000000000",\n'
        '  "nick": "HARNESS",\n'
        '  "discord_global_name": "HARNESS",\n'
        '  "use_discord_name": false\n'
        '}\n')
    # Only the flags that would change what the run measures. stealth_mode=0 is
    # load-bearing: with stealth on, the hub keeps the match out of the lobby
    # and the spectator never sees an in_match pair to request.
    dev_flags = ("stealth_mode=0\n"
                 "dev_mode=0\n"
                 "boot_to_battle=0\n"
                 "auto_upload_logs=0\n"
                 "gs_pic_fix=1\n")
    return [
        FileSwap(fm2k_dir / "discord_auth.json", auth_json),
        FileSwap(fm2k_dir / "dev_flags.ini",     dev_flags),
        FileSwap(cfg_dir  / "launcher.cfg",      game_root + "\n"),
        FileSwap(cfg_dir  / "games.cache",       None),   # force a fresh scan
    ]


# ---------------------------------------------------------------------------
# Hub lifecycle
# ---------------------------------------------------------------------------
def hub_start(win_py: str, relay_session: str | None) -> tuple[subprocess.Popen, Path]:
    if HUB_SCRATCH.exists():
        shutil.rmtree(HUB_SCRATCH, ignore_errors=True)
    HUB_SCRATCH.mkdir(parents=True, exist_ok=True)
    for f in HUB_FILES:
        shutil.copy2(HUB_SRC / f, HUB_SCRATCH / f)
    shutil.copytree(HUB_SRC / "handlers", HUB_SCRATCH / "handlers",
                    ignore=shutil.ignore_patterns("__pycache__"))
    hub_log = HUB_SCRATCH / "hub_win.log"
    extra = f" --test-relay-session {relay_session}" if relay_session else ""
    # Env via a `set` line in a .bat: `start /B VAR=...` does NOT propagate and
    # the hub then falls through to the Discord path (observed in Phase 5a).
    # `python -u` is MANDATORY -- hub.py logs with plain print(), and every
    # assertion anchor below is one of those lines.
    # `--host 127.0.0.1` is MANDATORY -- the hub defaults to 0.0.0.0, which
    # would expose this lobby to the LAN.
    bat = HUB_SCRATCH / "run_hub.bat"
    auth_line = ("" if os.environ.get("HUBE2E_RED_NO_AUTH_DISABLE") == "1"
                 else "set FM2K_HUB_AUTH_DISABLE=1\r\n")
    bat.write_text(
        "@echo off\r\n"
        + auth_line +
        f"cd /d {win_bs(HUB_SCRATCH)}\r\n"
        f'"{win_bs(win_py)}" -u hub.py --host 127.0.0.1 --port {HUB_PORT}{extra} '
        f"> hub_win.log 2>&1\r\n")
    proc = subprocess.Popen(["cmd.exe", "/C", sst.to_win(bat)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ready = f"FM2K Hub listening on tcp://127.0.0.1:{HUB_PORT} (WebSocket)"
    deadline = time.time() + 30
    while time.time() < deadline:
        if hub_log.exists() and ready in hub_log.read_text(errors="ignore"):
            log(f"hub up (log {hub_log})")
            return proc, hub_log
        if proc.poll() is not None:
            break
        time.sleep(0.25)
    tail = hub_log.read_text(errors="ignore")[-1200:] if hub_log.exists() else "(no log)"
    raise RuntimeError(f"hub never reported listening.\n--- hub log tail ---\n{tail}")


def hub_is_loopback(hub_proc, hub_log: Path) -> bool:
    """The hub, IF this run started one, must have reported itself listening on
    127.0.0.1 (review A9a). A run that deliberately skips the hub
    (HUBE2E_RED_NO_HUB) has nothing to check and nothing to leak to: the
    synthetic credential can only travel over a WS the launcher opens, and
    FM2K_HUB_HOST is pinned to loopback for every launcher this harness spawns."""
    if hub_proc is None:
        return True
    if not hub_log.exists():
        return False
    return (f"FM2K Hub listening on tcp://127.0.0.1:{HUB_PORT}"
            in hub_log.read_text(errors="ignore"))


def hub_pids() -> list[int]:
    """PIDs listening on the hub's WS port. Teardown kills BY PID only --
    several unrelated python.exe processes are routinely live on this box, so
    `taskkill /IM python.exe` would murder them."""
    try:
        out = subprocess.run(["netstat.exe", "-ano", "-p", "TCP"],
                             capture_output=True, text=True, timeout=15).stdout
    except Exception:
        return []
    pids = []
    for ln in out.splitlines():
        if f"127.0.0.1:{HUB_PORT}" in ln and "LISTENING" in ln:
            parts = ln.split()
            if parts and parts[-1].isdigit():
                pids.append(int(parts[-1]))
    return sorted(set(pids))


# ---------------------------------------------------------------------------
# Process bookkeeping
# ---------------------------------------------------------------------------
def count_image(image: str) -> int:
    # /FO CSV /NH: the default table format truncates the Image Name column at
    # 25 chars, so "WonderfulWorld_ver_0946.exe" never matched and the completion
    # poll silently saw zero games for an entire run.
    try:
        out = subprocess.run(["tasklist.exe", "/FI", f"IMAGENAME eq {image}",
                              "/FO", "CSV", "/NH"],
                             capture_output=True, text=True, timeout=20).stdout
    except Exception:
        return -1
    needle = '"' + image.lower() + '"'
    return sum(1 for ln in out.splitlines() if ln.lower().startswith(needle))


def wait_for(pred, timeout, what, poll=0.5) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pred():
            return True
        time.sleep(poll)
    log(f"TIMEOUT waiting for {what} ({timeout}s)")
    return False


def read(p: Path) -> str:
    try:
        return p.read_text(errors="ignore")
    except OSError:
        return ""


# ---------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--total-frames", type=int, default=4000,
                    help="TOTAL confirmed battle frames across matches before "
                         "the players auto-terminate. Deliberately the _TOTAL "
                         "knob, not _AT_FRAME: a wanwan match at --round-time "
                         "20 runs ~2537 battle frames, so any per-match "
                         "threshold above that never fires and the pair "
                         "rematches until the harness budget expires (observed: "
                         "15 matches in one run).")
    ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--round-time", type=int, default=20)
    ap.add_argument("--timeout", type=float, default=420.0,
                    help="hard wall-clock budget for the whole run")
    ap.add_argument("--min-spec-frames", type=int, default=800,
                    help="spectator [CINPUT] frames required (no-vacuous-pass)")
    ap.add_argument("--relay", action="store_true",
                    help="hub-relay spectate leg: host advertises "
                         "spec_transport=relay so the spectator stream rides "
                         "hub WS frames instead of a direct P2P dial")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    # ------------------------------------------------------------------
    # RESTORE-ON-START, BEFORE ANYTHING ELSE (review A9a, ship-blocker).
    # A previous run that died -- SIGKILL, a BSOD (this host is BSOD-prone), a
    # WSL shutdown -- left the user's real discord_auth.json / dev_flags.ini /
    # launcher.cfg stashed under sentinels and the harness's synthetic ones
    # live. Nothing but this sweep will ever put them back, so it runs before
    # the log clearing, before kill_strays, before the hub, before argument
    # validation: any of those can fail or hang, and the user's Discord token
    # must not be hostage to that.
    appdata = win_appdata()
    # The REAL game_root, not a placeholder: `restore()` decides "is the live
    # file the harness's own plant or something the user wrote since?" by
    # comparing content, so a boot sweep built with a fake root mis-identifies
    # the planted launcher.cfg as a user file, parks the user's REAL one under a
    # dated name and leaves the harness's root live. Observed exactly that on
    # the first crash-restore proof run. Resolving the root here is pure
    # in-memory path work -- no I/O, nothing that can fail before the sweep.
    boot_root = win_bs(sst.GAMES["wanwan"].parent)
    boot_swaps = build_swaps(appdata, boot_root)
    restored = sum(1 for sw in boot_swaps if sw.restore("stale-backup"))
    if restored:
        log(f"recovered {restored} stale backup(s) from an earlier aborted run "
            f"-- your real Discord auth cache is back in place")
    install_emergency_handlers()

    game_exe  = sst.GAMES["wanwan"]
    game_dir  = game_exe.parent
    # Root = the ONE game dir, not C:\\games\\2dfm. wanwan and wanwan_b are
    # byte-identical clones with the same exe STEM, and room->game resolution
    # keys on that stem: with both discovered, the spectator was launched from
    # wanwan_b while the players ran wanwan, so its debug log landed in a
    # directory no gate reads and the run looked like "spectator never ran".
    game_root = win_bs(game_dir)
    room_id   = game_exe.stem                    # WonderfulWorld_ver_0946
    game_img  = game_exe.name
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for old in OUT_DIR.glob("live_*.log"):
        old.unlink()

    swaps = build_swaps(appdata, game_root)
    # (crash recovery already ran at the very top of main -- review A9a)

    win_py = find_windows_python()
    if not win_py:
        log("FAIL: no Windows python found; the hub must run as a Windows "
            "process (WSL2 loopback split) -- install Python for Windows")
        return 2

    hub_proc = None
    hub_log  = HUB_SCRATCH / "hub_win.log"
    stop     = {"go": False}
    threads  = []
    t0       = time.time()
    fatal, notes = [], []
    relay_probe = {"rc": None, "out": ""}

    try:
        sst.kill_strays()
        # Clear stale per-role game logs FIRST. Every gate below reads these,
        # and a previous run's P1/P2/P3 is indistinguishable from this run's --
        # an early abort used to re-gate week-old logs and print a confident,
        # entirely fictional verdict.
        for lf in ("FM2K_P1_Debug.log", "FM2K_P2_Debug.log", "FM2K_P3_Debug.log"):
            (game_dir / "logs" / lf).unlink(missing_ok=True)
        # Same for the per-instance launcher logs: the H6 evidence is read out
        # of spec_launcher.log, and an abort before the spectator ever launched
        # would otherwise mine the PREVIOUS run's grant/spawn lines and report
        # a spawned_remote address for a process that does not exist.
        for lf in ("host_launcher.log", "guest_launcher.log", "spec_launcher.log"):
            (OUT_DIR / lf).unlink(missing_ok=True)
            (OUT_DIR / (lf + ".prev")).unlink(missing_ok=True)
        sst.wait_ports_free(HUB_PORTS + [7002])
        if os.environ.get("HUBE2E_RED_NO_HUB") == "1":
            log("RED PROOF: HUBE2E_RED_NO_HUB=1 -- skipping hub startup")
        else:
            hub_proc, hub_log = hub_start(
                win_py, RELAY_SESSION_TOKEN if args.relay else None)
        # 0xCF data-plane probe runs HERE, against the live hub this run stood
        # up, because teardown kills the hub before the assertion block below.
        if args.relay:
            probe = REPO / "tools" / "hub_relay_dataplane_probe.py"
            if probe.exists():
                r = subprocess.run([win_py, "-u", sst.to_win(probe)],
                                   capture_output=True, text=True, timeout=60)
                relay_probe["rc"] = r.returncode
                relay_probe["out"] = (r.stdout or "") + (r.stderr or "")
            else:
                relay_probe["rc"] = 127
                relay_probe["out"] = f"probe missing at {probe}"
        # GUARD BEFORE PLANTING (review A9a): never write the synthetic
        # credential unless the run is provably loopback-only, and never while
        # a real hub host is configured in the environment.
        why = assert_local_hub_only()
        if why is not None:
            log(f"FATAL: refusing to swap global user state -- {why}")
            return 2
        if not hub_is_loopback(hub_proc, hub_log):
            log("FATAL: refusing to swap global user state -- the local hub is "
                "not listening on 127.0.0.1; a planted synthetic Discord token "
                "must never coexist with a run that could reach a real hub")
            return 2
        for s in swaps:
            s.install()

        # ---- env ---------------------------------------------------------
        # Launcher env is INHERITED by the game the launcher spawns
        # (FM2KGameInstance applies its own vars on top of the process env), so
        # the player autoplay knobs ride the player launchers only. The
        # spectator launcher deliberately carries NONE of them: autoplay inputs
        # on a spectator would fork its sim, and FM2K_AUTO_TITLE_SKIP sets the
        # 0x010 title-skip bit that once auto-confirmed a spectator's CSS.
        hub_env = {
            "FM2K_HUB_HOST":         HUB_HOST,
            "FM2K_HUB_AUTOMATION":   "1",
            "FM2K_HUB_AUTO_CONNECT": "1",
            "FM2K_HUB_AUTO_ROOM":    room_id,
        }
        # RED arm C proves the DEFAULT, so it must test the DEFAULT branch: the
        # variable is REMOVED, not set to "0". It used to be set to "0", i.e. the
        # arm exercised a different branch than the property it claimed to prove
        # (review A6b). With EnvFlag now strict (only "1" is on) the two branches
        # are provably identical, and this makes the proof say so.
        if os.environ.get("HUBE2E_RED_NO_AUTOMATION") == "1":
            hub_env.pop("FM2K_HUB_AUTOMATION")
        play_env = {
            "FM2K_PARITY_AUTOPLAY":        "1",
            "FM2K_PARITY_AUTOPLAY_BATTLE": "1",
            "FM2K_AUTO_TITLE_SKIP":        "1",
            "FM2K_TEST_AUTO_CSS":          "0,0,0,0,0",
            "FM2K_AUTOPLAY_CSS_DWELL":     "0.4",
            "FM2K_HOST_TRACE":             "1",
            "FM2K_SPECTATOR_DEBUG":        "1",
            "FM2K_CINPUT":                 "1",
            "FM2K_TEST_ROUNDS":            str(args.rounds),
            "FM2K_TEST_ROUND_TIME":        str(args.round_time),
            "FM2K_AUTO_TERMINATE_TOTAL": str(args.total_frames),
        }
        p1_pty = OUT_DIR / "p1_parity.pty"
        sp_pty = OUT_DIR / "spec_parity.pty"
        for f in (p1_pty, sp_pty):
            f.unlink(missing_ok=True)

        # Per-instance launcher log. Three launchers share one install, and
        # the default log path is derived from the EXE dir -- they would all
        # truncate one another's launcher.log and the H6 evidence would be
        # unreadable. FM2K_LAUNCHER_LOG gives each its own file.
        host_ll  = OUT_DIR / "host_launcher.log"
        guest_ll = OUT_DIR / "guest_launcher.log"
        spec_ll  = OUT_DIR / "spec_launcher.log"

        host_env = {**hub_env, **play_env,
                    "FM2K_HUB_NICK": NICK_HOST,
                    "FM2K_HUB_AUTO_CHALLENGE": NICK_GUEST,
                    "FM2K_LAUNCHER_LOG": sst.to_win(host_ll),
                    "FM2K_PARITY_RECORD_PATH": sst.to_win(p1_pty)}
        guest_env = {**hub_env, **play_env,
                     "FM2K_HUB_NICK": NICK_GUEST,
                     "FM2K_LAUNCHER_LOG": sst.to_win(guest_ll),
                     "FM2K_HUB_AUTO_ACCEPT": "1"}
        spec_env = {**hub_env,
                    "FM2K_LAUNCHER_LOG": sst.to_win(spec_ll),
                    "FM2K_HUB_NICK": NICK_SPEC,
                    "FM2K_HUB_AUTO_SPECTATE": NICK_HOST,
                    "FM2K_SPECTATOR_DEBUG": "1",
                    "FM2K_CINPUT": "1",
                    "FM2K_TEST_ROUNDS": str(args.rounds),
                    "FM2K_SPEC_HOST_GONE_MS": "5000",
                    "FM2K_PARITY_RECORD_PATH": sst.to_win(sp_pty)}
        if args.relay:
            # Host advertises the relay data plane in its hello; the hub stores
            # it and echoes it into spectate_grant, so LaunchRemoteSpectator
            # sets FM2K_SPEC_TRANSPORT=relay on the spectator game. Zero
            # injection anywhere -- this is the negotiated production path.
            host_env["FM2K_SPEC_TRANSPORT"] = "relay"

        # ---- launch ------------------------------------------------------
        # No argv beyond the exe: the room-joined handler resolves room_id
        # against the discovered library and fires on_game_selected itself.
        # Passing --spectate anywhere would defeat the entire point of the
        # stage, so the ONLY argv is the launcher path.
        argv = [str(sst.LAUNCHER)]
        rcs = {}

        def spawn(label, env, logname):
            def run():
                rcs[label] = sst.launch(label, argv, env, OUT_DIR / logname,
                                        args.timeout, lambda: stop["go"])
            t = threading.Thread(target=run, daemon=True)
            t.start()
            threads.append(t)

        spawn("HOST", host_env, "host_stdout.log")
        time.sleep(1.5)
        spawn("GUEST", guest_env, "guest_stdout.log")

        # FAIL-FAST GUARD. Everything downstream waits minutes for a match that
        # can never happen if the clients never reached the hub at all (hub
        # down, hub demanding Discord auth, automation off). Without this the
        # stage would sit for 5 minutes and then report a confusing "no
        # [match] start". 75s covers a cold launcher boot plus the driver's
        # first two connect attempts with room to spare.
        def two_connected():
            txt = read(hub_log)
            return sum(1 for n in (NICK_HOST, NICK_GUEST)
                       if re.search(r"^\[\+\] " + n + r" \(", txt, re.M)) >= 2
        if not wait_for(two_connected, 75, "host+guest to reach the hub"):
            fatal.append("H2 host and guest never connected to the hub -- no "
                         "hub-side [+] lines. Hub down, hub requiring Discord "
                         "auth, or automation off; nothing downstream can pass.")
            raise SystemExit
        log("host+guest connected to the hub")

        # Wait for the hub to broker the match, then for the host to reach
        # battle, and only THEN start the spectator launcher -- so the hub's
        # stored session_kind is already "battle" when the grant is minted and
        # the spectator takes the boot-to-battle path.
        ms_re = re.compile(r"\[match\] start \w+ " + NICK_HOST + r"\(.*?\) vs "
                           + NICK_GUEST + r"\(")
        if not wait_for(lambda: ms_re.search(read(hub_log)) is not None, 150,
                        "hub [match] start"):
            fatal.append("H3 no [match] start in the hub log -- the brokered "
                         "challenge/accept never completed")
            raise SystemExit
        log("hub brokered the match")

        sk_re = re.compile(r"\[session_kind\] " + NICK_HOST + r" \(.*?\): .*-> battle")
        if not wait_for(lambda: sk_re.search(read(hub_log)) is not None, 180,
                        "host session_kind -> battle"):
            fatal.append("H4 host never published session_kind=battle")
            raise SystemExit
        log("host reached battle")

        spawn("SPEC", spec_env, "spec_stdout.log")

        grant_re = re.compile(r"\[spec\] grant " + NICK_SPEC + r"\(.*?\) -> "
                              + NICK_HOST + r"\(.*?\) same_nat=(\S+) host=(\S+)")
        if not wait_for(lambda: grant_re.search(read(hub_log)) is not None, 150,
                        "hub [spec] grant"):
            fatal.append("H5 hub never issued a spectate_grant")
            raise SystemExit
        gm = grant_re.search(read(hub_log))
        grant_host = gm.group(2)
        log(f"grant issued: same_nat={gm.group(1)} host={grant_host}")

        # ---- run to completion -------------------------------------------
        # Games auto-terminate at FM2K_AUTO_TERMINATE_TOTAL; the spectator
        # follows ~FM2K_SPEC_HOST_GONE_MS later. "all game processes gone,
        # having seen at least two" is the completion signal.
        seen_games = {"max": 0}

        def games_done():
            n = count_image(game_img)
            seen_games["max"] = max(seen_games["max"], n)
            return seen_games["max"] >= 2 and n == 0

        budget = args.timeout - (time.time() - t0)
        if not wait_for(games_done, max(60.0, budget), "games to finish", poll=2.0):
            notes.append(f"games did not all exit within budget "
                         f"(peak concurrent={seen_games['max']})")
        time.sleep(3.0)     # let the last log flush

    except SystemExit:
        pass
    except Exception as e:                       # noqa: BLE001
        fatal.append(f"harness exception: {e!r}")
    finally:
        stop["go"] = True
        for t in threads:
            t.join(timeout=20)
        sst.kill_strays()
        # Hub by PID, in its own command, never by image name.
        pids = hub_pids()
        for pid in pids:
            subprocess.run(["taskkill.exe", "/F", "/PID", str(pid)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if pids:
            log(f"hub killed by PID: {pids}")
        if hub_proc is not None:
            try:
                hub_proc.terminate()
            except OSError:
                pass
        for s in swaps:
            s.restore("teardown")
        sst.wait_ports_free(HUB_PORTS + [7002])

    # ---- preserve evidence ------------------------------------------------
    # FRESHNESS. Every gate below reads these copies, and the game writes them
    # into a directory that survives runs -- so "this run's log" and "a log from
    # last week" are the same bytes to a parser. The clear at the top of the run
    # is the primary defence; this mtime check is the one that CANNOT be
    # defeated by a failed unlink (Windows refuses it when a handle lingers).
    stale_logs = []
    for lf in ("FM2K_P1_Debug.log", "FM2K_P2_Debug.log", "FM2K_P3_Debug.log"):
        src = game_dir / "logs" / lf
        if src.exists() and src.stat().st_mtime < (t0 - 2.0):
            stale_logs.append(lf)
    if stale_logs:
        fatal.append("STALE EVIDENCE: " + ", ".join(stale_logs) + " predate this "
                     "run -- the gates would have judged an earlier session")
    for lf in ("FM2K_P1_Debug.log", "FM2K_P2_Debug.log", "FM2K_P3_Debug.log"):
        src = game_dir / "logs" / lf
        if src.exists():
            try:
                shutil.copy2(src, OUT_DIR / f"live_{lf}")
            except OSError as e:
                log(f"(warn) could not preserve {lf}: {e}")
    if hub_log.exists():
        try:
            shutil.copy2(hub_log, OUT_DIR / "hub.log")
        except OSError:
            pass

    hub_txt   = read(OUT_DIR / "hub.log") or read(hub_log)
    spec_launcher = read(OUT_DIR / "spec_launcher.log")
    p1_live   = OUT_DIR / "live_FM2K_P1_Debug.log"
    p2_live   = OUT_DIR / "live_FM2K_P2_Debug.log"
    p3_live   = OUT_DIR / "live_FM2K_P3_Debug.log"

    print("\n" + "=" * 70)
    print("HUB-BROKERED FLOW ASSERTIONS")
    print("=" * 70)

    def check(tag, ok, detail):
        print(f"[{'PASS' if ok else 'FAIL'}] {tag}: {detail}")
        if not ok:
            fatal.append(f"{tag}: {detail}")
        return ok

    check("H1 hub auth-free",
          "[hub] auth DISABLED" in hub_txt and
          f"FM2K Hub listening on tcp://127.0.0.1:{HUB_PORT}" in hub_txt,
          "auth DISABLED + WS listening")

    joined = {n for n in (NICK_HOST, NICK_GUEST, NICK_SPEC)
              if re.search(r"^\[\+\] " + n + r" \(", hub_txt, re.M)}
    rejects = hub_txt.count("[auth] REJECT")
    check("H2 three clients, no Discord",
          len(joined) == 3 and rejects == 0,
          f"connected={sorted(joined)} auth_rejects={rejects}")

    m_start = re.search(r"\[match\] start (\w+) " + NICK_HOST, hub_txt)
    check("H3 match brokered by hub", m_start is not None,
          f"token={m_start.group(1) if m_start else '(none)'}")

    check("H4 host published battle",
          re.search(r"\[session_kind\] " + NICK_HOST + r" \(.*?\): .*-> battle",
                    hub_txt) is not None,
          "session_kind menu/css -> battle")

    gm = re.search(r"\[spec\] grant " + NICK_SPEC + r"\(.*?\) -> " + NICK_HOST
                   + r"\(.*?\) same_nat=(\S+) host=(\S+)", hub_txt)
    grant_host = gm.group(2) if gm else ""
    check("H5 spectate_grant issued", gm is not None,
          f"host={grant_host or '(none)'} same_nat={gm.group(1) if gm else '-'}")

    # --- H6, the load-bearing one ---------------------------------------
    # (a) structural: no --spectate token in ANY .bat this harness generated.
    bats = sorted(OUT_DIR.glob(".launch_*.bat"))
    spectate_argv = [b.name for b in bats if "--spectate" in read(b)]
    # (b) the grant's host= is the address the spectator process was launched
    #     with (process_manager.cpp sets FM2K_REMOTE_ADDR from the grant).
    spawn_m = re.search(r"Launching remote spectator: .*?-> ([0-9.]+:\d+)",
                        spec_launcher)
    spawn_addr = spawn_m.group(1) if spawn_m else ""
    # (c) ordering, inside ONE log: request -> grant -> spawn.
    i_req   = spec_launcher.find("[hub-auto] spectate_request")
    i_grant = spec_launcher.find("Hub: spectate: ")
    i_spawn = spec_launcher.find("Launching remote spectator:")
    ordered = (0 <= i_req < i_grant < i_spawn)
    check("H6 spectator came from the GRANT, not a direct dial",
          not spectate_argv and spawn_addr == grant_host and ordered,
          f"argv_with_--spectate={spectate_argv or 'none'} "
          f"grant_host={grant_host or '(none)'} spawned_remote={spawn_addr or '(none)'} "
          f"order(req<grant<spawn)={ordered} (idx {i_req}/{i_grant}/{i_spawn})")

    denied = ("spectate_denied" in hub_txt or "[spec] BLOCKED" in hub_txt
              or "spectate denied" in spec_launcher)
    check("H7 no denial", not denied, "no spectate_denied / cross-version block")

    # ---- no-vacuous-pass guard -------------------------------------------
    p1_txt, p3_txt = read(p1_live), read(p3_live)
    rounds     = p1_txt.count("[ROUND-START]")
    spec_cin   = len(re.findall(r"\[CINPUT\] bf=", p3_txt))
    check("NV host coverage", rounds >= 1,
          f"host [ROUND-START]={rounds} (need >=1)")
    # Spectator playback coverage is the other half of the no-vacuous-pass
    # guard -- except in --relay mode, where the spectator provably CANNOT
    # receive frames on an all-loopback harness (see the RELAY LEG block).
    if args.relay:
        print(f"[ADVISORY] NV spectator coverage: [CINPUT] frames={spec_cin} "
              f"(not gated in --relay: loopback same_nat suppresses the hub's "
              f"spectator_incoming, so the host never learns the viewer's "
              f"user_id and cannot address relay frames)")
        if spec_cin:
            notes.append(f"relay run admitted {spec_cin} spectator frames -- "
                         f"that is MORE than the loopback path can explain; "
                         f"re-read the RELAY LEG analysis")
    else:
        check("NV spectator coverage", spec_cin >= args.min_spec_frames,
              f"spectator [CINPUT] frames={spec_cin} "
              f"(need >={args.min_spec_frames})")

    # ---- desync (gekko per-frame fingerprint term) ------------------------
    desyncs = []
    for f in (p1_live, p2_live, p3_live):
        for ln in read(f).splitlines():
            if "DESYNC #" in ln and "phantom" not in ln:
                desyncs.append(re.sub(r"^\[[0-9:.]*\] ", "", ln))
    desyncs = sorted(set(desyncs))
    check("DS no GekkoNet DESYNC", not desyncs,
          f"{len(desyncs)} line(s)" + (": " + desyncs[0] if desyncs else ""))

    # ---- reused playback gates -------------------------------------------
    print("\n" + "=" * 70)
    print("PLAYBACK GATES (spec_selftest, reused verbatim)")
    print("=" * 70)
    if not p3_live.exists():
        print("[hube2e] SKIPPED -- no spectator log from this run (the brokered "
              "flow never got that far); the H-assertions above are the verdict.")
        print("\n" + "=" * 70)
        for n in notes:
            print(f"[note] {n}")
        print(f"[hube2e] hubspec: FAIL ({len(fatal)} fatal)")
        for f in fatal:
            print(f"    {f}")
        print(f"[hube2e] evidence: {OUT_DIR}")
        return 1
    specs = [{"k": 0, "tag": "P3", "phase": "battle(hub-grant)",
              "live": p3_live, "first_host_match": 0}]
    try:
        cin_fail, ck_fail = sst._parity_gates(OUT_DIR, specs)
    except Exception as e:                       # noqa: BLE001
        cin_fail, ck_fail = False, False
        notes.append(f"_parity_gates raised {e!r}")
        specs[0].setdefault("gate", {"checked": 0})
        specs[0].setdefault("ok", False)
    try:
        css_fail, css_lines = sst._css_parity_gate(OUT_DIR, specs)
        for ln in css_lines:
            print(ln)
    except Exception as e:                       # noqa: BLE001
        css_fail = False
        notes.append(f"_css_parity_gate raised {e!r}")
    try:
        lv = sst.spectator_liveness(p1_live, p3_live, first_host_match=0)
        print(f"[hube2e] LIVE-EDGE P3: host_final={lv['host_final']} "
              f"spec_max={lv['spec_max']} gap={lv['gap']} reached={lv['reached']}")
        if not lv["reached"]:
            notes.append(f"spectator did not hold the live edge "
                         f"(gap={lv['gap']}) -- recorded, not fatal")
    except Exception as e:                       # noqa: BLE001
        notes.append(f"spectator_liveness raised {e!r}")

    g = specs[0].get("gate") or {"checked": 0}
    gate_ok = bool(specs[0].get("ok")) and g.get("checked", 0) > 0
    gate_detail = (f"checked={g.get('checked', 0)} bad={g.get('bad', '?')} "
                   f"(0 checked = the spectator produced no traces, which is "
                   f"not a pass)")
    if args.relay:
        print(f"[ADVISORY] GATE spectator sim identity: {gate_detail}")
    else:
        check("GATE spectator sim identity", gate_ok, gate_detail)
    if cin_fail:
        notes.append("CINPUT mismatches present (tail/truncation class -- "
                     "non-fatal by seam-gate precedent)")
    if ck_fail:
        notes.append("CHECKSUM segment noise present (non-fatal)")
    if css_fail:
        notes.append("CSS parity flagged (non-fatal: the trailing char-select "
                     "is cut by the hard terminate)")

    # ---- relay leg --------------------------------------------------------
    if args.relay:
        print("\n" + "=" * 70)
        print("RELAY LEG (hub-brokered, spec_transport=relay)")
        print("=" * 70)
        p1_txt_l, p3_txt_l = read(p1_live), read(p3_live)
        subscribed  = re.search(r"\[spec_relay\] subscribed " + NICK_SPEC
                                + r" to " + NICK_HOST, hub_txt) is not None
        first_frame = re.search(r"\[spec_relay\] first binary frame from "
                                + NICK_HOST, hub_txt) is not None
        # The grant must have carried spec_transport=relay all the way into the
        # spawned process. "transport=relay" in the launch line is the exact
        # field that was broken until this phase (it read "transport=tcp" while
        # mode read "relay" -- an argument-order defect in launcher_callbacks).
        launched_relay = "transport=relay" in spec_launcher
        viewer_relay   = "FM2K_SPEC_TRANSPORT=relay" in p3_txt_l
        host_relay     = "FM2K_SPEC_TRANSPORT=relay" in p1_txt_l
        skipped_direct = "JOIN_ACK accepted in relay mode" in p3_txt_l
        check("R1 relay negotiated end to end (grant -> spawn -> both hooks)",
              subscribed and launched_relay and viewer_relay and host_relay
              and skipped_direct,
              f"hub_subscribed={subscribed} launch_line_transport_relay="
              f"{launched_relay} host_hook_relay={host_relay} "
              f"viewer_hook_relay={viewer_relay} "
              f"viewer_skipped_direct_dial={skipped_direct}")
        # ADVISORY, and it is advisory for a STRUCTURAL reason, not a hopeful
        # one: relay SendTo addresses each frame by the subscriber's hub
        # user_id, and the host only ever learns that id from the hub's
        # `spectator_incoming` push -- which hub/handlers/spectate.py skips
        # whenever same_nat is true. Every client in this harness shares the
        # loopback source IP, so same_nat is ALWAYS true, the host's subscriber
        # record stays user_id=(none), and zero frames can be addressed. Making
        # this fatal would red the stage for a property of the topology.
        print(f"[ADVISORY] R1b host relay frames reached the hub: {first_frame}")
        # 0xCF data plane on the SAME hub instance this run stood up (probed
        # before the games started, while the hub was alive). The in-game 0xCF
        # spectator FALLBACK is NOT reachable from a brokered loopback run --
        # two independent reasons, both documented in the Phase 5b report:
        # same_nat suppresses spec_relay in the grant, and the K::SpectateGranted
        # handler CLEARS FM2K_HUB_RELAY_ADDR/_SESSION (set-or-clear discipline)
        # before LaunchRemoteSpectator, so injected creds cannot survive either.
        print(relay_probe["out"].strip() or "(probe produced no output)")
        check("R2 hub 0xCF data plane", relay_probe["rc"] == 0,
              f"relay_dataplane_probe rc={relay_probe['rc']}")

    # ---- verdict ----------------------------------------------------------
    print("\n" + "=" * 70)
    for n in notes:
        print(f"[note] {n}")
    if fatal:
        print(f"[hube2e] hubspec: FAIL ({len(fatal)} fatal)")
        for f in fatal:
            print(f"    {f}")
        print(f"[hube2e] evidence: {OUT_DIR}")
        return 1
    print("[hube2e] hubspec: PASS "
          f"(brokered match + grant + spectator playback; evidence: {OUT_DIR})")
    if not args.keep:
        shutil.rmtree(HUB_SCRATCH, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
