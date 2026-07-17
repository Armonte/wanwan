#!/usr/bin/env python3
"""FM95 stress determinism smoke — the Phase-2c first-tick gate.

Launches ONE CPW instance in --stress mode (GekkoStressSession forces a
rollback every FM2K_CHECK_DISTANCE frames). StartStressSession auto-sets
FM95_TRAMPOLINE=1 for FM95, and the hook's autoplay-nav walks title->CSS->
battle unattended (RE-9). We then scan the game log for:
  - reached battle (trampoline phase = BATTLE),
  - save/load activity (rollback actually ran),
  - desync markers (gekko DESYNC / FM95_P1_desync_f*.log).

A clean run = battle reached + rollbacks ran + ZERO desyncs over N frames =
"the FM95 sim is rollback-deterministic." Any desync is a real bug to chase.

Usage:  python3 tools/fm95_stress_smoke.py [frames] [check_distance]
"""
import sys, os, time, glob, subprocess, threading, re

sys.path.insert(0, "tools")
import spec_selftest as s

frames  = sys.argv[1] if len(sys.argv) > 1 else "1500"
check_d = sys.argv[2] if len(sys.argv) > 2 else "10"

cpw = s.GAMES["cpw"]
if not cpw.exists():
    print(f"CPW not found: {cpw}"); sys.exit(2)
game_arg = s.to_win(cpw)
log_dir = cpw.parent / "logs"

# Clear old desync dumps so we only count this run's.
for f in glob.glob(str(cpw.parent / "FM95_P*_desync_f*.log")) + \
         glob.glob(str(log_dir / "FM95_P*_desync_f*.log")):
    try: os.remove(f)
    except OSError: pass

s.kill_strays(); time.sleep(1.0)

# Harness env for the game. CRITICAL: a plain Popen env= dict does NOT reach
# a Windows .exe launched via WSL interop — the vars must ALSO be listed in
# WSLENV or WSL drops them at the WSL→Windows boundary (that's why an earlier
# run got check_distance=0 and reached battle via CPW's attract demo instead
# of autoplay). FM95_TRAMPOLINE + FM2K_STRESS_MODE come from StartStressSession
# (set Windows-side by the launcher), so they were unaffected.
harness_env = {
    "FM2K_CHECK_DISTANCE":          str(check_d),   # force rollback every N frames
    "FM2K_PARITY_AUTOPLAY":         "1",            # title/CSS nav
    "FM2K_PARITY_AUTOPLAY_BATTLE":  "1",            # deterministic battle input
    "FM2K_AUTO_TITLE_SKIP":         "1",
    "FM2K_AUTO_TERMINATE_AT_FRAME": str(frames),
    "FM2K_PRODUCTION_MODE":         "0",
}
env = dict(os.environ)
env.update(harness_env)
prev_wslenv = env.get("WSLENV", "")
env["WSLENV"] = ":".join(harness_env.keys()) + ((":" + prev_wslenv) if prev_wslenv else "")
print(f"[fm95-stress] CPW stress: frames={frames} check_distance={check_d} "
      f"(FM95_TRAMPOLINE auto-set by StartStressSession)")

# Launch DIRECTLY (argv + env cross to Windows as UTF-16/clean), NOT via
# s.launch()'s temp-.bat — cmd.exe mangles the fullwidth ＣＰＷ path in ANSI,
# which broke DetectEngineForExe (→ engine=FM2K → launch fail).
slog = open(s.OUT_DIR / "fm95_stress.log", "w")
proc = subprocess.Popen([str(s.LAUNCHER), "--stress", game_arg],
                        env=env, stdout=slog, stderr=subprocess.STDOUT)
# Give it up to ~90s to walk to battle + run frames + auto-terminate.
time.sleep(90)
try: proc.terminate()
except Exception: pass
s.kill_strays()
subprocess.run(["powershell.exe", "-c",
                "Get-Process ＣＰＷ,CPW -ErrorAction SilentlyContinue | Stop-Process -Force"],
               capture_output=True)
time.sleep(0.5)

# ---- verdict from the game debug log ----
dbg = sorted(glob.glob(str(log_dir / "FM2K_P*_Debug.log")), key=os.path.getmtime)
if not dbg:
    print("[fm95-stress] FAIL — no game debug log produced"); sys.exit(1)
text = open(dbg[-1], encoding="utf-8", errors="replace").read()

reached_battle = "trampoline tick phase = BATTLE" in text
# check_distance actually applied (env crossed WSLENV) — otherwise no rollbacks.
cd_override = re.search(r"FM2K_CHECK_DISTANCE override -> (\d+)", text)
cd_applied  = int(cd_override.group(1)) if cd_override else 0
# Peak rollback count from the BATTLE STATUS lines (rb=N) — proves save/load/
# resim actually ran, not just a clean forward sim.
rb_counts   = [int(m) for m in re.findall(r"BATTLE STATUS:.*? rb=(\d+)", text)]
max_rb      = max(rb_counts) if rb_counts else 0
max_frame   = max([int(m) for m in re.findall(r"BATTLE STATUS: frame=(\d+)", text)] or [0])
desync_log  = glob.glob(str(cpw.parent / "FM95_P*_desync_f*.log")) + \
              glob.glob(str(log_dir / "FM95_P*_desync_f*.log"))
desync_msgs = len(re.findall(r"desync=[1-9]|DESYNC|[Dd]esync detected", text))
crash       = ("[CRASH]" in text or "[TERMINATE]" in text)

print(f"[fm95-stress] reached_battle={reached_battle} check_distance={cd_applied} "
      f"max_rb={max_rb} max_frame={max_frame} "
      f"desync_dumps={len(desync_log)} desync_msgs={desync_msgs} crash={crash}")
if crash:
    print("[fm95-stress] FAIL — crash/terminate marker in log"); sys.exit(1)
if not reached_battle:
    print("[fm95-stress] INCOMPLETE — autoplay did not reach battle; log:", dbg[-1])
    sys.exit(3)
if cd_applied == 0 or max_rb == 0:
    print("[fm95-stress] INCOMPLETE — no forced rollbacks (check_distance not "
          "applied or rb=0); clean forward sim only, determinism NOT exercised.")
    sys.exit(3)
if desync_log or desync_msgs:
    print(f"[fm95-stress] DESYNC — {len(desync_log)} dump(s), {desync_msgs} msg(s); "
          "the FM95 sim is not yet rollback-deterministic.", desync_log)
    sys.exit(4)
print(f"[fm95-stress] PASS — battle reached, {max_rb} rollbacks over {max_frame} "
      "frames, zero desyncs. FM95 SIM IS ROLLBACK-DETERMINISTIC.")
