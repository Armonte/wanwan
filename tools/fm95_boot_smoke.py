#!/usr/bin/env python3
"""Phase-0 exit-gate boot smoke for FM95 (workplan 0c): two CPW instances
side by side.

Launches TWO independent `--offline <exe>` launcher instances (the CLI's
native no-netplay path -- no ports, no session; the launcher sniffs the
engine from the exe and injects FM95Hook.dll). Pass criterion is BOOT ONLY:
after ~20 s both launcher processes are still alive and no
[CRASH]/[TERMINATE] marker landed in the logs. Nothing about sync.

NOTE: until the 0b multi-instance bypass lands, instance 2 is EXPECTED to
die early (CPW WinMain FindWindowA("KGT95GAME") refusal). This script is
the gate we run AFTER that patch lands.

Usage: python3 tools/fm95_boot_smoke.py [game_key] [boot_secs]
"""
import subprocess, sys, time
sys.path.insert(0, "tools")
import spec_selftest as s

GAME_KEY  = sys.argv[1] if len(sys.argv) > 1 else "cpw"
BOOT_SECS = int(sys.argv[2]) if len(sys.argv) > 2 else 20
GAME = s.GAMES[GAME_KEY]
if not GAME.exists():
    print(f"game not found: {GAME}"); sys.exit(2)
# launcher.log / launcher.prev.log live next to the launcher exe (shared by
# both instances -- attribute markers to the run, not a specific instance).
SHARED_LOGS = [s.LAUNCHER.parent / "launcher.log",
               s.LAUNCHER.parent / "launcher.prev.log"]


def kill_all():
    s.kill_strays()  # launcher tree (/T also reaps the spawned game children)
    subprocess.run(["taskkill.exe", "/F", "/T", "/IM", GAME.name],  # orphans
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def start(label):
    # Launch DIRECTLY via WSL interop (argv crosses to Windows as UTF-16
    # intact), NOT via s.launch()'s temp-.bat trick: cmd.exe reads the .bat
    # in the ANSI codepage and mangles the full-width ＣＰＷ filename. A boot
    # smoke needs no env vars, so the .bat's `set K=V` machinery isn't
    # needed either.
    log = open(s.OUT_DIR / f"smoke_{label}.log", "w")
    return subprocess.Popen([str(s.LAUNCHER), "--offline", s.to_win(GAME)],
                            stdout=log, stderr=subprocess.STDOUT)


def markers(path):
    try:
        txt = open(path, errors="ignore").read()
    except OSError:
        return []
    return [m for m in ("[CRASH]", "[TERMINATE]") if m in txt]


s.OUT_DIR.mkdir(parents=True, exist_ok=True)
print(f"[smoke] game={GAME_KEY} ({GAME.name}) boot_secs={BOOT_SECS}")
kill_all(); time.sleep(1.0)
p1 = start("p1")
time.sleep(1.5)
p2 = start("p2")
time.sleep(BOOT_SECS)

shared_bad = [m for log in SHARED_LOGS for m in markers(log)]
all_ok = True
for label, proc in (("P1", p1), ("P2", p2)):
    alive = proc.poll() is None
    bad = markers(s.OUT_DIR / f"smoke_{label.lower()}.log") + shared_bad
    ok = alive and not bad
    all_ok &= ok
    why = "" if alive else f" (exited rc={proc.returncode})"
    if bad:
        why += f" (markers: {sorted(set(bad))})"
    print(f"[smoke] {label}: {'PASS' if ok else 'FAIL'}{why}")
    if not ok and label == "P2" and not alive:
        print("[smoke]   instance 2 dying early is expected until the "
              "multi-instance bypass lands")

kill_all()
sys.exit(0 if all_ok else 1)
