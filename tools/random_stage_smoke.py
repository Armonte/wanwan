#!/usr/bin/env python3
"""Random-stage validation: ordering fix + host/guest determinism.

The old implementation rolled inside Netplay_StartBattle and poked
ADDR_SELECTED_STAGE (0x43010c) AFTER vs_round_function had already handed that
value to LoadStageFile -- so the roll always landed one match late (match 1 of a
session played the config stage). The roll now happens in Hook_LoadStageFile at
the moment the game reads 0x43010c.

Asserts, over a multi-match autoplay session with random stage enabled:
  1. the override hook actually fires                      (feature is wired)
  2. it fires on the FIRST match, not just rematches       (the ordering fix)
  3. host and guest roll the SAME stage for every match    (determinism)
  4. the MATCH_START stage_id equals the applied stage     (no X-vs-Y mismatch)

Usage: python3 tools/random_stage_smoke.py [game] [matches]
"""
import sys, re, time, threading
sys.path.insert(0, "tools")
import spec_selftest as s

game_key = sys.argv[1] if len(sys.argv) > 1 else "wanwan"
matches  = int(sys.argv[2]) if len(sys.argv) > 2 else 3

game = s.GAMES[game_key]
if not game.exists():
    print(f"game not found: {game}"); sys.exit(2)
game_arg = s.to_win(game)

# ~1 round/match at 6s, plus CSS walk; give each match a generous slice.
total_frames = str(1400 * matches)
SEED = "424242"

s.kill_strays(); time.sleep(1.0)

common = {
    "FM2K_PARITY_AUTOPLAY":        "1",
    "FM2K_PARITY_AUTOPLAY_BATTLE": "1",
    "FM2K_AUTO_TITLE_SKIP":        "1",
    "FM2K_TEST_AUTO_CSS":          "0,0,0,0,0",
    "FM2K_TEST_ROUNDS":            "1",
    "FM2K_TEST_ROUND_TIME":        "6",
    "FM2K_AUTO_TERMINATE_TOTAL":   total_frames,
    "FM2K_AUTOPLAY_CSS_DWELL":     "8",
    # Random stage ON, shared seed -> both peers must roll identically.
    "FM2K_STAGE_RANDOM_SEED":      SEED,
    "FM2K_STAGE_RANDOM_MIN":       "0",
    "FM2K_STAGE_RANDOM_MAX":       "7",
}
p1_env = {**common, "FM2K_LOCAL_PORT": str(s.P1_PORT),
          "FM2K_REMOTE_ADDR": f"127.0.0.1:{s.P2_PORT}"}
p2_env = {**common, "FM2K_LOCAL_PORT": str(s.P2_PORT),
          "FM2K_REMOTE_ADDR": f"127.0.0.1:{s.P1_PORT}"}
p1_args = [str(s.LAUNCHER), "--host", game_arg, "--port", str(s.P1_PORT)]
p2_args = [str(s.LAUNCHER), "--connect", f"127.0.0.1:{s.P1_PORT}", game_arg,
           "--port", str(s.P2_PORT)]

p1_log = s.OUT_DIR / "rstage_p1.log"
p2_log = s.OUT_DIR / "rstage_p2.log"
# The hook's SDL_Log output goes to the GAME's debug log, not launcher stdout.
game_dir = game.parent
p1_dbg = game_dir / "logs" / "FM2K_P1_Debug.log"
p2_dbg = game_dir / "logs" / "FM2K_P2_Debug.log"
for f in (p1_dbg, p2_dbg):
    try: f.unlink()
    except Exception: pass
timeout = 120 + 40 * matches

t1 = threading.Thread(target=lambda: s.launch("RS_P1", p1_args, p1_env, p1_log, timeout, None), daemon=True)
t1.start(); time.sleep(1.5)
t2 = threading.Thread(target=lambda: s.launch("RS_P2", p2_args, p2_env, p2_log, timeout, None), daemon=True)
t2.start()
t1.join(timeout + 30); t2.join(30)
time.sleep(1.0)

OVERRIDE = re.compile(r"RandomStage: LoadStageFile override (\d+) -> (\d+)")
ROLLED   = re.compile(r"RandomStage: rolled=(\d+)")
MSTAGE   = re.compile(r"Netplay: match stage_id=(\d+)")

def scan(path):
    try:
        txt = path.read_text(errors="replace")
    except Exception:
        return None
    return {
        "overrides": [(int(a), int(b)) for a, b in OVERRIDE.findall(txt)],
        "rolled":    [int(x) for x in ROLLED.findall(txt)],
        "mstage":    [int(x) for x in MSTAGE.findall(txt)],
        "seeded":    "RandomStage: seeded" in txt,
    }

h, g = scan(p1_dbg), scan(p2_dbg)
if not h or not g:
    print("FAIL: missing logs"); sys.exit(1)

print(f"host: seeded={h['seeded']} overrides={h['overrides']} match_stage={h['mstage']}")
print(f"guest: seeded={g['seeded']} overrides={g['overrides']} match_stage={g['mstage']}")

fails = []
if not h["seeded"]:
    fails.append("host never seeded -- env did not reach the hook")
if not h["overrides"]:
    fails.append("override hook never fired -- random stage not applied at all")
# (2) the ordering fix: the very first match must already be overridden.
if h["mstage"] and h["overrides"]:
    first_applied = h["overrides"][0][1]
    if h["mstage"][0] != first_applied:
        fails.append(f"match 1 stage_id={h['mstage'][0]} != first roll {first_applied} "
                     f"-- roll still landing one match late")
# (3) determinism
n = min(len(h["overrides"]), len(g["overrides"]))
if n == 0:
    fails.append("guest produced no overrides -- cannot check determinism")
for i in range(n):
    if h["overrides"][i][1] != g["overrides"][i][1]:
        fails.append(f"match {i+1}: host rolled {h['overrides'][i][1]} but guest "
                     f"rolled {g['overrides'][i][1]} -- DESYNC")
# (4) recorded stage == applied stage
for i in range(min(len(h["mstage"]), len(h["overrides"]))):
    if h["mstage"][i] != h["overrides"][i][1]:
        fails.append(f"match {i+1}: MATCH_START stage_id={h['mstage'][i]} != "
                     f"applied {h['overrides'][i][1]}")

if fails:
    print("\nFAIL:")
    for f in fails: print("  -", f)
    sys.exit(1)
print(f"\nPASS: {n} match(es), rolls identical host==guest, "
      f"match 1 overridden, stage_id matches applied roll")
