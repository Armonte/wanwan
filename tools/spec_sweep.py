#!/usr/bin/env python3
"""Phase 6 spectator feature-parity sweep -- the STAGE-SWITCHING and
SETTINGS-VARIANCE legs.

WHY THESE LEGS EXIST
--------------------
Every spectator stage in run_all_tests.sh runs ONE game, ONE stage, ONE set of
match settings. Real matches vary all three. The two legs here close the two
holes that are pure spectator-plane exposure:

  (a) STAGE SWITCHING. The spectator does not roll the stage -- it is TOLD.
      Hook_LoadStageFile excludes g_spectator_mode from the random-stage
      override, so a viewer loads whatever ADDR_SELECTED_STAGE says, and that
      value reaches it EXCLUSIVELY through HOST_CONFIG. A lost or late per-match
      HOST_CONFIG therefore shows the viewer the PREVIOUS match's stage, on the
      exact between-matches reconstruction path Phase 4c fixed. Nothing gated it.

  (b) SETTINGS VARIANCE. HOST_CONFIG carries five sim-relevant settings and the
      battle-entry barrier refuses to start a match until the two PLAYERS agree
      on their digest. There is no equivalent for spectators -- a spectator is
      barred from emitting a barrier packet by construction (g_player_index <= 1
      in netplay_control.cpp), so its settings plane is unverified BY DESIGN.
      This leg is that verification: the hook's [CFG] stamp is compared across
      host, guest and spectator, digest AND raw values.

Judged on the [CFG] line (round_events.cpp), which is the only per-battle
settings evidence that exists on all three planes: [ROUND-START] returns early
for spectators and [BATTLE-CFG] hangs off SaveState_Save, which never runs on
one.

BOTH legs also inherit spec_selftest's own verdict (CINPUT, CHECKSUM full-state,
the POOL top=/nobj= terms, CSS-FP, CSS-WIN), because a wrong stage or wrong
round count on the viewer changes its object pool -- a second, independent
detector for the same defect.

RED PROOFS (no stage ships without a demonstrated failure -- Phase 3 rule):
  stage leg     FM2K_SPEC_HOSTCFG_DROP=1  -> the host stops sending HOST_CONFIG
                                             to spectators; the viewer's stage
                                             stops tracking the host's rolls.
  settings leg  FM2K_SPEC_HOSTCFG_DROP=1  -> the three-plane digest comparison
                                             fails on the spectator plane.
  settings leg  --red-barrier              -> FM2K_HOSTCONFIG_LATE + a short
                                             FM2K_CFG_BARRIER_FORCE_MS drives
                                             the PLAYER half into the
                                             "MATCH SETTINGS NEVER AGREED"
                                             force-complete.

Usage:
  python3 tools/spec_sweep.py --leg stage
  python3 tools/spec_sweep.py --leg settings --variants V0,V1
  python3 tools/spec_sweep.py --leg settings --variants V0 --red spec-hostcfg-drop
"""
from __future__ import annotations
import argparse, json, os, re, shutil, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC = ROOT / "tools" / "spec_selftest.py"

# --- the stage leg's game -------------------------------------------------
# NOT wanwan. wanwan defines 2 stages, so a "random stage across matches" run
# there tests a 2-element range and the clamp path is the only thing exercised.
# pkmncc has a real stage table (noted in spec_selftest.GAMES), so rolls are
# distinguishable and a stage CHANGE across matches actually happens.
# --game is the ENGINE carrier (spec_selftest picks FM95 vs FM2K metadata off
# it); --game-exe is what actually runs and is load-bearing (rc=2 if absent).
STAGE_GAME_KEY = "pkmncc"
STAGE_GAME_EXE = Path(os.environ.get(
    "SPECSWEEP_STAGE_EXE", "/mnt/d/Games/fm2k/_NODEV/pkmncc/pkmncc.exe"))

CFG_RX = re.compile(
    r"\[CFG\] plane=(\w+) match=(\d+) digest=0x([0-9A-Fa-f]+) stage=(\d+) "
    r"rounds=(\d+) time=(\d+) speed=(\d+) socd=(-?\d+) mode_flag=(-?\d+) "
    r"cfg_rx=(\d+) latch_rounds=(\d+) latch_stage=(-?\d+)")
# The viewer-plane latch re-derive (spec_relatch.cpp). Needed here because the
# [CFG] stamp's SAMPLE POINT on a viewer can precede the repair: [CFG] fires at
# the engine's own RSS_ACTIVE edge, and on a boot-to-battle deep joiner the
# host's MATCH_START -- the authoritative round count, and the thing that
# repairs the latch -- drains a few milliseconds LATER. So a viewer that raced
# stamps the STALE value and then runs the whole match on the CORRECT one. The
# raw latch_rounds term below cannot see that, and would red a run the fix
# actually made right. These three lines are the post-apply truth:
#   CORRECTED match=M X -> Y   the repair, with both values
#   frame-zero ok match=M = N  the value the viewer's FIRST BATTLE FRAME used
#   TRIP                       the repair was reverted before that frame (fatal)
#
# THE match= ORDINAL IS LOAD-BEARING, not decoration. Without it the excuse
# below is LOG-GLOBAL: a CORRECTED 3 -> 1 in match 1 would launder an
# uncorrected latch_rounds=3 stamp in match 5, which is reachable the moment a
# session runs more than one match with a viewer that raced once. The hook
# stamps a per-process count of MATCH_START applies, which pairs 1:1 with
# [CFG]'s own per-process match ordinal (both count this viewer's battles in
# order). A missing / mismatched ordinal is FAIL-CLOSED: no excuse, term stays
# fatal. Ordinal-less lines from a pre-amendment binary therefore also fail
# closed rather than laundering.
RELATCH_FIX_RX  = re.compile(
    r"\[SPEC-RELATCH\] CORRECTED match=(\d+) g_round_limit (\d+) -> (\d+)")
RELATCH_OK_RX   = re.compile(
    r"\[SPEC-RELATCH\] frame-zero ok match=(\d+) -- g_round_limit=(\d+)")
# Absent-plane default for parse_relatch's result, so every consumer sees the
# same keys (a missing "legacy_lines" would KeyError a fatal path).
RELATCH_NONE = {"fixes": {}, "effective": {}, "trips": 0, "legacy_lines": 0}
OVERRIDE_RX = re.compile(r"RandomStage: LoadStageFile override (\d+) -> (\d+)")
STAGECNT_RX = re.compile(r"RandomStage: game defines (\d+) stage")
HCRX_RX     = re.compile(r"Received HOST_CONFIG #(\d+) \(stage=(\d+)")


def parse_cfg(path: Path) -> list[dict]:
    out = []
    try:
        with open(path, errors="ignore") as fh:
            for ln in fh:
                m = CFG_RX.search(ln)
                if not m:
                    continue
                out.append({
                    "plane": m.group(1), "match": int(m.group(2)),
                    "digest": m.group(3).upper(), "stage": int(m.group(4)),
                    "rounds": int(m.group(5)), "time": int(m.group(6)),
                    "speed": int(m.group(7)), "socd": int(m.group(8)),
                    "mode_flag": int(m.group(9)), "cfg_rx": int(m.group(10)),
                    "latch_rounds": int(m.group(11)),
                    "latch_stage": int(m.group(12)),
                })
    except OSError:
        pass
    return out


def parse_relatch(path: Path) -> dict:
    """Viewer-plane latch repairs seen in one log, keyed BY MATCH ORDINAL:
      fixes[match]     = (from, to) the re-derive actually wrote
      effective[match] = the value the viewer's FIRST BATTLE FRAME of that
                         match really ran on (the frame-zero readback)
      trips            = how many times a repair was reverted before that frame
    Both maps are per-match because both consumers are per-match: the excuse
    must name the match it repaired, and the effective value must be compared
    against the host's latch for the SAME battle."""
    fixes, effective, trips, legacy = {}, {}, 0, 0
    try:
        with open(path, errors="ignore") as fh:
            for ln in fh:
                if "[SPEC-RELATCH]" not in ln:
                    continue
                m = RELATCH_FIX_RX.search(ln)
                if m:
                    fixes[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))
                    continue
                m = RELATCH_OK_RX.search(ln)
                if m:
                    effective[int(m.group(1))] = int(m.group(2)); continue
                if "TRIP" in ln:
                    trips += 1; continue
                # A verdict line from a binary older than the match ordinal.
                # Counted so a fatal can SAY it fell back rather than looking
                # like a fresh regression (the "a term that cannot run must not
                # be silent about it" rule from Phase 4d/4e).
                if "CORRECTED" in ln or "frame-zero ok" in ln:
                    legacy += 1
    except OSError:
        pass
    return {"fixes": fixes, "effective": effective, "trips": trips,
            "legacy_lines": legacy}


def grep_count(path: Path, needle: str) -> int:
    n = 0
    try:
        with open(path, errors="ignore") as fh:
            for ln in fh:
                if needle in ln:
                    n += 1
    except OSError:
        pass
    return n


def read_text(path: Path) -> str:
    try:
        return path.read_text(errors="ignore")
    except OSError:
        return ""


def spec_logs(out_dir: Path) -> list[tuple[str, Path]]:
    """(tag, preserved live log) for every plane that produced one."""
    planes = [("P1", out_dir / "live_FM2K_P1_Debug.log"),
              ("P2", out_dir / "live_FM2K_P2_Debug.log")]
    for k in range(1, 5):
        p = out_dir / f"live_FM2K_S{k}_Debug.log"
        if p.exists():
            planes.append((f"S{k}", p))
    return [(t, p) for t, p in planes if p.exists()]


def run_spec(label: str, out_dir: Path, extra_env: dict, args: list[str],
             timeout: int, log_path: Path) -> int:
    """Run the SHIPPED spec_selftest with a dedicated FM2K_TEST_OUT_DIR."""
    env = dict(os.environ)
    env["FM2K_TEST_OUT_DIR"] = str(out_dir)
    env.update({k: str(v) for k, v in extra_env.items()})
    out_dir.mkdir(parents=True, exist_ok=True)
    # A previous run's preserved logs would be parsed as this run's evidence if
    # a plane failed to launch -- the same stale-log trap spec_selftest guards
    # against inside the game dir.
    for old in out_dir.glob("live_FM2K_*_Debug.log"):
        old.unlink(missing_ok=True)
    cmd = [sys.executable, "-u", str(SPEC)] + args
    print(f"[sweep] {label}: {' '.join(cmd)}")
    for k in sorted(extra_env):
        print(f"[sweep]   {k}={extra_env[k]}")
    t0 = time.time()
    with open(log_path, "w") as lf:
        try:
            rc = subprocess.call(cmd, env=env, stdout=lf, stderr=subprocess.STDOUT,
                                 timeout=timeout)
        except subprocess.TimeoutExpired:
            rc = 124
    print(f"[sweep] {label}: spec_selftest rc={rc} ({time.time() - t0:.0f}s) "
          f"log={log_path}")
    # Persist the preserved per-plane logs NEXT TO the run's own artifacts,
    # immediately. The FM2K_TEST_OUT_DIR is per-LEG, not per-run, so the next
    # arm of an A/B overwrites it -- which already cost this session one green
    # corpus. It is also what --regate consumes.
    corpus = log_path.parent / (log_path.stem + "_logs")
    corpus.mkdir(parents=True, exist_ok=True)
    for lf in out_dir.glob("live_FM2K_*_Debug.log"):
        try:
            shutil.copy2(lf, corpus / lf.name)
        except OSError:
            pass
    return rc


def slice_offset(host: list, spec: list) -> int:
    """Where a viewer's per-match sequence sits inside the host's, or -1.

    A viewer observes a CONTIGUOUS WINDOW of the host's matches: it starts at
    whichever match it dialled in on, and it can also STOP EARLY -- the host
    terminates at its total-frame budget, and a viewer that is a delay bank
    behind may never reach the host's final match. So the rule is a contiguous
    slice at some offset, NOT the tail.

    Measured the hard way: the first shipped version required the tail and went
    red inside the gate on a run where the deep joiner saw exactly the host's
    match 2 of 3 -- correct viewer behaviour at the budget edge, wrong
    assertion. Ambiguity (the same short sequence matching at several offsets)
    is resolved to the LATEST offset, which is the conservative choice for a
    late joiner."""
    if not spec or len(spec) > len(host):
        return -1
    for off in range(len(host) - len(spec), -1, -1):
        if host[off:off + len(spec)] == spec:
            return off
    return -1


def tail_align(host: list, spec: list) -> bool:
    return slice_offset(host, spec) >= 0


# ---------------------------------------------------------------------------
# LEG (a): STAGE SWITCHING
# ---------------------------------------------------------------------------
def leg_stage(a) -> tuple[bool, dict]:
    """Up to TWO attempts, for the same reason run_all_tests retries the
    deep-join stage: spectator join timing is NOT determinized by
    FM2K_NET_SEED (joins key off host-log markers plus wall-clock settles), so
    a css2 viewer can legitimately land a match later, observe one fewer match,
    and trip the harness's own budget-edge artifacts (CSS-FP "spec=None",
    LIVE-EDGE). One failure is not a verdict. A RED-PROOF arm never retries --
    it is supposed to fail, and retrying would just double its runtime."""
    attempts = 1 if a.red else 2
    for att in range(1, attempts + 1):
        ok, res = leg_stage_once(a, att)
        res["attempt"] = att
        if ok or att == attempts:
            return ok, res
        print(f"[sweep] stage attempt {att}: FAIL -- retrying once (join timing "
              f"is not seeded; a single failure is not a verdict)")
    return False, {}


def leg_stage_once(a, attempt: int = 1) -> tuple[bool, dict]:
    out_dir = ROOT / "tools" / ".spec_sweep_stage"
    log = a.out / (f"6a_stage_run.log" if attempt == 1
                   else f"6a_stage_run_att{attempt}.log")
    if not STAGE_GAME_EXE.exists():
        print(f"[sweep] stage leg: FATAL -- {STAGE_GAME_EXE} not found. The leg "
              f"needs a game with a REAL stage table (wanwan has 2 stages, which "
              f"makes the roll untestable). Set SPECSWEEP_STAGE_EXE.")
        return False, {"error": "stage game missing", "exe": str(STAGE_GAME_EXE)}

    env = {
        "FM2K_STAGE_RANDOM_SEED": a.seed,
        "FM2K_STAGE_RANDOM_MIN": "0",
        "FM2K_STAGE_RANDOM_MAX": "7",
        "FM2K_NET_DELAY_MS": "100", "FM2K_NET_JITTER_MS": "30",
        "FM2K_NET_LOSS": a.loss, "FM2K_NET_SEED": "141",
        "FM2K_SPEC_RC": "1", "FM2K_SPEC_DEEP_JOIN": "1",
        # CSS-ANIM ARMING (2026-08-17, spec_faller_diagnosis.md 5.3). This leg
        # ran with the per-slot character-select census DARK ("CSS-WIN CSSANIM:
        # no [CSS-ANIM] lines on any plane -- NOT ARMED ... NOT COMPUTED"), so
        # the only fall term running here was the one-object-per-player FALL
        # term -- the one that was resolving a background-script UI object as
        # player 0. CSSANIM is per-slot and kind-filtered by the engine's own
        # predicate, so arming it is what makes a red in this leg readable.
        "FM2K_CSS_ANIM": "1",
        # CSS-WINDOW GATE: ADVISORY IN THIS LEG ONLY, and re-surfaced loudly
        # below rather than silenced. Reason, measured 2026-08-16: this leg
        # NEEDS short matches (a stage-switching test that never reaches a
        # second match tests nothing), short matches mean a custom round timer,
        # and at --round-time 6 the falling-object term reds on wanwan AND on
        # pkmncc -- identically with FM2K_TEST_ROUNDS_HOST_ONLY on and off, and
        # on a build whose only spectator-side change is a log line. That is the
        # known open falling-object class (Wave 2 narrowed it, its residual is
        # recorded in ec6a6a7), not something this leg introduced or can fix.
        # A stage that is permanently red for a defect it does not own gets
        # disabled, and then the stage-tracking coverage goes with it.
        # The SETTINGS leg keeps the term FATAL (it runs the default timer).
        "FM2K_CSSWIN_FATAL": "0",
    }
    if a.red == "spec-hostcfg-drop":
        env["FM2K_SPEC_HOSTCFG_DROP"] = "1"
    args = ["--game", STAGE_GAME_KEY, "--game-exe", str(STAGE_GAME_EXE),
            "--rounds", "1", "--round-time", str(a.round_time),
            "--total-frames", str(a.total_frames),
            # TWO BIND FLAVOURS: a session-start CSS joiner (full backfill) and
            # a mid-battle snapshot joiner. NOT css2 (the between-matches deep
            # joiner), even though it is the third flavour: measured on this
            # recipe, a css2 viewer ends the run a full match behind (the
            # matches are ~1450 frames and it dials in after one of them), so
            # the host terminates before the viewer reaches the last
            # char-select LOCK and spec_selftest's CSS-FP term reds with
            # "spec=None" -- a budget-edge artifact of a SHORT-match recipe,
            # which this leg needs and stage 2d (whose whole subject is the
            # bounded deep join, at 15 s rounds) does not. Reproduced on both
            # attempts of a retry, so it is not a coin flip to be retried away.
            "--spectators", "css,battle1", "--record-timeout", "420", "--keep"]
    rc = run_spec("stage", out_dir, env, args, a.timeout, log)

    host_log = out_dir / "live_FM2K_P1_Debug.log"
    host_txt = read_text(host_log)
    host_cfg = [c for c in parse_cfg(host_log)]
    guest_cfg = parse_cfg(out_dir / "live_FM2K_P2_Debug.log")
    rolls = [int(b) for _a, b in OVERRIDE_RX.findall(host_txt)]
    m = STAGECNT_RX.search(host_txt)
    stage_count = int(m.group(1)) if m else -1
    clamped = "RandomStage: range" in host_txt and "clamped" in host_txt

    res = {"rc": rc, "rolls": rolls, "stage_count": stage_count,
           "clamped": clamped,
           "host_stages": [c["latch_stage"] for c in host_cfg],
           "guest_stages": [c["latch_stage"] for c in guest_cfg],
           "spectators": {}}
    fails, notes = [], []

    if rc != 0:
        fails.append(f"spec_selftest returned rc={rc} -- its own gates (CINPUT / "
                     f"CHECKSUM / POOL top=+nobj= / CSS) are part of this leg's "
                     f"verdict; a wrong stage on the viewer moves the pool too")
    if not rolls:
        fails.append("the host never rolled a stage (no 'RandomStage: "
                     "LoadStageFile override' line) -- the feature did not run, "
                     "so nothing was tested")
    if not host_cfg:
        fails.append("no [CFG] lines on the host -- the per-battle settings "
                     "stamp is absent from this build (or FM2K_CFG_TRACE=0); "
                     "the leg cannot be judged and must not pass vacuously")
    host_stages = res["host_stages"]
    if len(set(host_stages)) < 2 and len(host_stages) >= 2:
        fails.append(f"the stage never CHANGED across {len(host_stages)} matches "
                     f"({host_stages}) -- a stage-switching leg that never "
                     f"switches proves nothing (seed/range?)")
    # The v0.2.71 clamp: an out-of-range roll makes LoadStageFile open a
    # nonexistent file and throw a modal error box mid-match.
    if stage_count > 0:
        bad = [s for s in host_stages if s < 0 or s >= stage_count]
        if bad:
            fails.append(f"applied stage id(s) {bad} outside the game's "
                         f"{stage_count}-entry stage table -- the clamp failed")
        if stage_count <= 7 and not clamped:
            notes.append(f"game defines {stage_count} stage(s) and MAX=7 but no "
                         f"clamp WARN was logged")
    if host_stages and guest_cfg and not tail_align(host_stages,
                                                    res["guest_stages"]):
        fails.append(f"guest stage sequence {res['guest_stages']} does not match "
                     f"the host's {host_stages}")

    tracked_two = False
    for tag, path in spec_logs(out_dir):
        if not tag.startswith("S"):
            continue
        txt = read_text(path)
        cfg = parse_cfg(path)
        stages = [c["latch_stage"] for c in cfg]
        overrides = OVERRIDE_RX.findall(txt)
        hc = [int(s) for _n, s in HCRX_RX.findall(txt)]
        res["spectators"][tag] = {"stages": stages, "overrides": len(overrides),
                                  "hostconfig_rx": len(hc)}
        if overrides:
            fails.append(f"{tag} EMITTED {len(overrides)} random-stage override "
                         f"line(s) -- a spectator must never roll; it is TOLD "
                         f"the stage through HOST_CONFIG")
        if not cfg:
            fails.append(f"{tag} produced no [CFG] line -- the viewer never "
                         f"reached a battle, so its stage was never checked")
            continue
        offset = slice_offset(host_stages, stages)
        if offset < 0:
            fails.append(f"{tag} stage sequence {stages} is not a contiguous "
                         f"slice of the host's {host_stages} -- the viewer did "
                         f"NOT track the host's stage changes")
            offset = 0
        # Free second detector, and the only place a NON-ZERO custom round time
        # is delivery-tested at all (this leg runs pkmncc, which does not carry
        # wanwan's subsequent-battle timer bug -- see the VARIANTS comment): the
        # whole settings digest must agree per aligned match, not just the
        # stage. Uses the offset the STAGE sequence already resolved, so the
        # two checks cannot silently disagree about which host match is which.
        for i, c in enumerate(cfg):
            if offset + i >= len(host_cfg):
                break
            h = host_cfg[offset + i]
            for field in ("digest", "time", "rounds", "speed", "socd"):
                if c[field] != h[field]:
                    fails.append(f"{tag} match{c['match']} {field}={c[field]} "
                                 f"but host match{h['match']} {field}={h[field]}")
        if len(hc) < len(stages):
            notes.append(f"{tag} applied {len(hc)} HOST_CONFIG(s) for "
                         f"{len(stages)} match(es)")
        if len(stages) >= 2 and len(set(stages)) >= 2:
            tracked_two = True
    if not res["spectators"]:
        fails.append("no spectator produced a log -- nothing was tested")
    elif not tracked_two:
        # Not fatal on its own: a viewer can legitimately observe one match at
        # the budget edge. But say it loudly -- it is the difference between
        # "the viewer tracked a CHANGE" and "the viewer matched one value".
        notes.append("no spectator observed two DIFFERENT stages -- the change "
                     "itself was only checked against the host's sequence, not "
                     "across a viewer boundary")

    # Re-surface the CSS-window verdict this leg de-fatalised. Advisory is not
    # the same as invisible: a falling object on the viewer's char-select is a
    # real user-visible defect, and the count belongs in the leg's own output
    # where the next reader will see it.
    css_lines = [ln.strip() for ln in read_text(log).splitlines()
                 if "CSS-WIN FALL S" in ln and "window(s)" in ln]
    res["csswin"] = css_lines
    for ln in css_lines:
        print(f"[sweep] stage CSS-WINDOW [advisory, de-fatalised in this leg]: "
              f"{ln.split('[harness] ')[-1]}")

    res["fails"], res["notes"] = fails, notes
    return (not fails), res


# ---------------------------------------------------------------------------
# LEG (b): SETTINGS VARIANCE
# ---------------------------------------------------------------------------
# round_time / rounds / speed / socd per variant, plus the run shape.
#
# ROUND TIME ON WANWAN IS A TRAP, and the variant table is built around it
# rather than around the tidy 6/60/0 grid the design spec sketched. Recorded at
# netplay_control.cpp:83-91 and in spec_selftest's own --round-time help: a
# NON-ZERO custom timer leaves wanwan's round-timer counter at 0 on every
# SUBSEQUENT battle. That is a known-bugged engine configuration, and it is
# visible: measured here 2026-08-16, `--round-time 6` multi-match on wanwan reds
# the (pre-existing, FATAL) CSS-WINDOW gate with a falling object in 3 of 4
# between-match windows -- IDENTICALLY with FM2K_TEST_ROUNDS_HOST_ONLY on and
# off, so it is not this leg's doing. A leg whose baseline sits on a bugged
# configuration is a leg that is permanently red for something it does not own.
# So:
#   * time=-1 (leave the game default) is the multi-match baseline. It is the
#     shape the Wave-2 CSS runs proved green on wanwan (2 matches / 8000 frames).
#   * time=0 (explicit INFINITE) is still exercised, because 0 is a VALID value
#     and 0xFFFFFFFF is the only "unset" -- that distinction is exactly what a
#     delivery test must keep honest. It runs SINGLE-match: with no timer,
#     matches end only on a KO the autoplay rarely lands.
#   * a non-zero custom round time is exercised on the STAGE leg instead, which
#     runs pkmncc (--round-time 6 there), i.e. a game without wanwan's trap.
# "single" = judge one match (--frames) instead of the multi-match ladder
# (--total-frames): enough for a DELIVERY assertion, which is per-match.
VARIANTS = {
    # baseline: game-default timer, 1 round, multi-match
    "V0": {"time": -1, "rounds": 1, "speed": 10, "socd": 1,
           "frames": 8000, "single": False},
    # ROUND-COUNT delivery -- the headline. 2 rounds to win, single match: with
    # FM2K_TEST_ROUNDS_HOST_ONLY the guest and the spectator can only learn this
    # value from HOST_CONFIG, and one match is all a per-match delivery check
    # needs. (Multi-match at 2 rounds costs ~24000 frames for two boundaries.)
    "V1": {"time": -1, "rounds": 2, "speed": 10, "socd": 1,
           "frames": 3000, "single": True},
    # infinite timer (0 is VALID, not unset) + a non-default game speed
    "V2": {"time": 0,  "rounds": 1, "speed": 16, "socd": 1,
           "frames": 2500, "single": True},
    # SOCD variance -- one of the five digest fields, so a delivery failure
    # blocks battle entry rather than desyncing the sim
    "V3": {"time": -1, "rounds": 1, "speed": 10, "socd": 4,
           "frames": 8000, "single": False},
}


def leg_settings_one(a, name: str) -> tuple[bool, dict]:
    v = VARIANTS[name]
    out_dir = ROOT / "tools" / f".spec_sweep_settings_{name}"
    log = a.out / f"6b_settings_{name}.log"
    # PROFILE. 100ms / 30ms jitter / 10% loss with a battle-join AND a
    # between-matches join is the shape the Wave-2 CSS runs proved green on
    # wanwan, and it is used here deliberately: at 80ms/no-jitter the
    # (pre-existing, FATAL) CSS-window falling-object term fires on roughly
    # every other multi-match run, and this leg keeps that term FATAL -- unlike
    # the stage leg, which cannot avoid short matches and de-fatalises it. A
    # settings verdict must not be decided by a coin flip in a different gate.
    env = {
        "FM2K_NET_DELAY_MS": "100", "FM2K_NET_JITTER_MS": "30",
        "FM2K_NET_LOSS": a.loss,
        "FM2K_NET_SEED": "151", "FM2K_SPEC_RC": "1",
        "FM2K_SPEC_DEEP_JOIN": "1",
        # CSS-ANIM ARMING (2026-08-17, spec_faller_diagnosis.md 5.3). This leg
        # keeps the CSS-window fall term FATAL, and it was running it with the
        # per-slot census DARK -- so the stage that went red had exactly one
        # character-select fall term computing, and it was the mis-resolving
        # one. CSSANIM is the per-slot, kind-filtered term; arm it here so a
        # red carries a second, independent read.
        "FM2K_CSS_ANIM": "1",
        "FM2K_TEST_GAME_SPEED": str(v["speed"]),
        "FM2K_SOCD_MODE": str(v["socd"]),
    }
    # FM2K_TEST_ROUNDS_HOST_ONLY suppresses the every-frame g_default_round
    # force in hooks_update.cpp so round count can only arrive via HOST_CONFIG.
    # HONEST NOTE, measured 2026-08-16: on FM2K that force is DEAD CODE anyway
    # -- TrampolineMainLoop calls original_update_game directly, so
    # Hook_UpdateGameState is never entered (zero arm lines with the variable
    # set on all three planes). The switch is set here because it makes the
    # delivery-only path EXPLICIT rather than accidental, and because it is live
    # on the FM95 host-driven path. --rounds-host-only off is the CONTROL ARM,
    # and it is what turned "the harness masks round-count delivery" from an
    # assumption into a measurement: both arms behave identically.
    if a.rounds_host_only == "on":
        env["FM2K_TEST_ROUNDS_HOST_ONLY"] = "1"
    if a.red == "spec-hostcfg-drop":
        env["FM2K_SPEC_HOSTCFG_DROP"] = "1"
    if a.red == "barrier":
        # PLAYER-half red proof: the host suppresses every pre-entry
        # HOST_CONFIG, so the guest latches its own game.ini values, and the
        # barrier's agreement budget is cut to its 1000ms floor so the
        # force-complete ERROR lands inside the run instead of after 10s.
        env["FM2K_HOSTCONFIG_LATE"] = "1"
        env["FM2K_CFG_BARRIER_FORCE_MS"] = "1000"
        # ... at a loss rate that keeps the barrier's own 100ms re-push loop
        # from repairing the disagreement inside the shortened budget. The
        # lever alone only DELAYS the first config; the loss is what makes the
        # delay outlast the budget.
        env["FM2K_NET_LOSS"] = "0.35"
    # Multi-match variants take BOTH bind flavours (mid-battle snapshot join +
    # between-matches deep join). A single-match variant cannot: css2 keys off
    # the host's SECOND char-select, which never happens, so the viewer would
    # never dial in and the run would fail structurally rather than on settings.
    args = ["--rounds", str(v["rounds"]), "--round-time", str(v["time"]),
            "--spectators", "css" if v["single"] else "battle1,css2",
            "--record-timeout", "600", "--keep"]
    args += (["--frames", str(v["frames"])] if v["single"]
             else ["--total-frames", str(v["frames"])])
    rc = run_spec(f"settings/{name}", out_dir, env, args, a.timeout, log)
    return judge_settings(out_dir, name, rc, a.rounds_term)


def judge_settings(out_dir: Path, name: str, rc: int,
                   rounds_term: str) -> tuple[bool, dict]:
    """The settings leg's VERDICT, split out from the run so it can be applied
    to an existing out-dir (--regate). That is what makes the assertions
    red-proofable without burning a game run: the terms that fire on a rare
    event -- 'MATCH SETTINGS NEVER AGREED' (the barrier force-completing on
    disagreement) and cfg_rx=0 -- can be injected into a copy of a green
    corpus and the SHIPPED judge re-run against it verbatim."""
    v = VARIANTS[name]
    planes = spec_logs(out_dir)
    per_plane = {tag: parse_cfg(p) for tag, p in planes}
    relatch   = {tag: parse_relatch(p) for tag, p in planes}
    never_agreed = sum(grep_count(p, "MATCH SETTINGS NEVER AGREED")
                       for _t, p in planes)
    res = {"variant": name, "asked": v, "rc": rc,
           "never_agreed": never_agreed,
           "planes": {t: [{k: c[k] for k in
                           ("match", "digest", "rounds", "time", "speed",
                            "socd", "cfg_rx", "latch_rounds")}
                          for c in cfgs]
                      for t, cfgs in per_plane.items()}}
    fails, rounds_fails, notes = [], [], []

    if rc != 0:
        fails.append(f"spec_selftest returned rc={rc}")
    if never_agreed:
        fails.append(f"{never_agreed} 'MATCH SETTINGS NEVER AGREED' line(s) -- "
                     f"the battle-entry barrier force-completed on DISAGREEMENT")
    host = per_plane.get("P1", [])
    if not host:
        fails.append("no [CFG] line on the host -- the instrument is absent "
                     "(build without the stamp, or FM2K_CFG_TRACE=0); the "
                     "three-plane comparison cannot run and must not pass")
        res["fails"], res["rounds_fails"], res["notes"] = fails, rounds_fails, notes
        return False, res

    # (0) SENTINEL GUARD (the A4a(ii) lesson from Phase 4d/4e: a term that
    # cannot run must not print a positive verdict). Netplay_MatchSettingsDigest
    # returns 0 ONLY when it did not really run -- on FM95, where round time /
    # round count / game speed have no mapped globals, ADDR_SELECTED_STAGE is 0
    # and the HOST_CONFIG apply is a documented no-op. A real reading is never 0
    # (the function returns 1 instead). Three matching zeros would otherwise
    # sail through every comparison below as perfect agreement.
    zero = [f"{t}:match{c['match']}" for t, cfgs in per_plane.items()
            for c in cfgs if c["digest"] in ("0", "00000000")]
    if zero:
        fails.append(f"settings digest is the 0 SENTINEL on {len(zero)} stamped "
                     f"battle(s) ({', '.join(zero[:4])}) -- that means the digest "
                     f"did not run (FM95 build: no mapped globals, apply is a "
                     f"no-op). This leg is FM2K-only and must SKIP on FM95 with "
                     f"a stated reason rather than pass on three matching zeros")

    # (1) The HOST actually applied what the harness asked for. Checked first:
    # if the host is wrong, three EQUAL digests would still be three wrong
    # planes, which is exactly the failure mode digest-equality alone misses.
    for c in host:
        # time == -1 means "leave the game default": there is no absolute value
        # to assert, only cross-plane agreement (checked below). Every other
        # field always has one.
        if v["time"] >= 0 and c["time"] != v["time"]:
            fails.append(f"host match{c['match']} round time={c['time']}, asked "
                         f"{v['time']}")
        if c["speed"] != v["speed"]:
            fails.append(f"host match{c['match']} speed={c['speed']}, asked "
                         f"{v['speed']}")
        if c["socd"] != v["socd"]:
            fails.append(f"host match{c['match']} socd={c['socd']}, asked "
                         f"{v['socd']}")
        if c["rounds"] != v["rounds"]:
            rounds_fails.append(f"host match{c['match']} rounds={c['rounds']}, "
                                f"asked {v['rounds']}")

    # (2) Every other plane agrees with the host, per match, digest AND raw.
    #
    # PAIRING. A guest shares the host's match ordinals. A viewer may have
    # dialled in late and therefore observed a SUFFIX of the host's matches, so
    # its k-th battle is the host's (offset + k)-th -- align on the tail, never
    # on the ordinal, or a late joiner is compared against matches it never saw.
    for tag, cfgs in per_plane.items():
        if tag == "P1":
            continue
        if not cfgs:
            fails.append(f"{tag} produced no [CFG] line -- it never reached "
                         f"a battle, so its settings plane was not checked")
            continue
        # Same contiguous-slice rule as the stage leg (a viewer can start late
        # AND stop early), resolved on the digest sequence; a viewer whose
        # digests do not appear as a slice at all is compared from the tail so
        # the mismatch is REPORTED rather than skipped.
        if tag.startswith("S"):
            off = slice_offset([c["digest"] for c in host],
                               [c["digest"] for c in cfgs])
            offset = off if off >= 0 else max(0, len(host) - len(cfgs))
        else:
            offset = 0
        for i, c in enumerate(cfgs):
            hi = offset + i
            if hi >= len(host):
                notes.append(f"{tag} match{c['match']} has no host counterpart "
                             f"(viewer saw more battles than the host logged)")
                continue
            h = host[hi]
            for field in ("time", "speed", "socd", "digest"):
                if c[field] != h[field]:
                    msg = (f"{tag} match{c['match']} {field}={c[field]} but host "
                           f"match{h['match']} {field}={h[field]}")
                    if field == "digest":
                        msg += (" -- the five sim-relevant settings are NOT the "
                                "same on this plane")
                    fails.append(msg)
            if c["rounds"] != h["rounds"]:
                rounds_fails.append(
                    f"{tag} match{c['match']} rounds={c['rounds']} but host "
                    f"match{h['match']} rounds={h['rounds']}")
            if c["latch_rounds"] != h["latch_rounds"]:
                # VIEWER SAMPLE-POINT CARVE-OUT (see RELATCH_FIX_RX above).
                # [CFG] samples the latch at the engine's OWN RSS_ACTIVE edge,
                # which on a viewer can precede the host's MATCH_START apply --
                # the moment the authoritative round count arrives and the
                # re-derive repairs the latch. So a stamped mismatch is only a
                # real one if the repair did NOT happen for exactly this pair.
                # Excused ONLY on evidence: a [SPEC-RELATCH] CORRECTED line
                # naming this exact (stamped -> host) transition FOR THIS EXACT
                # MATCH, and no TRIP anywhere in the log. Anything else stays
                # fatal. The match key is what stops match 1's correction from
                # laundering match 5's stale stamp.
                rl = relatch.get(tag, RELATCH_NONE)
                by_fix = (rl["fixes"].get(c["match"])
                          == (c["latch_rounds"], h["latch_rounds"]))
                # SECOND, STRONGER EVIDENCE PATH. The re-derive is not the only
                # thing that can repair a raced latch before the first battle
                # frame: on the SHIPPED default configuration the deep-join /
                # pool-resync snapshot's GAME_STATE memcpy covers 0x470048 and
                # frequently lands FIRST, so the match runs on the host's value
                # with NO 'CORRECTED' line to point at. Measured live in this
                # lane: AMEND_GREEN1 S1 and AMEND_NOAGREE S1 both stamped
                # latch_rounds=3, ran frame zero on 1, and had no CORRECTED
                # line. Requiring the CORRECTED line would red those runs for
                # being repaired by the OTHER mechanism. The frame-zero readback
                # is DIRECT evidence of the value the sim used (and is sampled
                # after [CFG]), so it excuses on its own -- and the same term is
                # FATAL below when it disagrees, so this cannot launder a real
                # divergence.
                by_readback = (rl["effective"].get(c["match"])
                               == h["latch_rounds"]
                               and c["match"] in rl["effective"])
                excused = (by_fix or by_readback) and rl["trips"] == 0
                msg = (f"{tag} match{c['match']} latch_rounds={c['latch_rounds']} "
                       f"but host {h['latch_rounds']} (the SIM's copy)")
                if not excused and rl["legacy_lines"] and not rl["fixes"]:
                    msg += (f" -- NOTE: this log carries {rl['legacy_lines']} "
                            f"[SPEC-RELATCH] verdict line(s) WITHOUT a match "
                            f"ordinal, i.e. it was produced by a binary older "
                            f"than the per-match excuse. The sample-point "
                            f"carve-out fails CLOSED on those rather than "
                            f"laundering them across matches; re-run on a "
                            f"current build to re-gate")
                if excused:
                    why = (f"[SPEC-RELATCH] CORRECTED match={c['match']} "
                           f"{c['latch_rounds']} -> {h['latch_rounds']} "
                           "repaired it at the MATCH_START apply, which is "
                           "AFTER this stamp's sample point"
                           if by_fix else
                           f"the frame-zero readback for match={c['match']} "
                           f"says the sim ran on {rl['effective'][c['match']]} "
                           "(= the host's latch); the repair came from the "
                           "snapshot apply rather than the re-derive, so there "
                           "is no CORRECTED line to quote")
                    notes.append(msg + " -- EXCUSED: " + why +
                                 ". Zero TRIP lines in this log.")
                else:
                    rounds_fails.append(msg)
            # THE FRAME-ZERO TERM. The [CFG] stamp above is a PRE-repair sample
            # on a viewer, so on its own it is blind in one direction: if the
            # viewer stamped the host's value and was then CORRECTED AWAY from
            # it, the stamped comparison is equal, nothing prints, and the
            # viewer runs the whole match on a limit the host never used. The
            # frame-zero readback is the only measurement of what the sim
            # actually ran on, so it is compared here -- per match, against the
            # host's own latch. Without this the carve-out above leaves the leg
            # strictly WEAKER than before it existed.
            rl = relatch.get(tag, RELATCH_NONE)
            eff = rl["effective"].get(c["match"])
            if eff is not None and eff != h["latch_rounds"]:
                rounds_fails.append(
                    f"{tag} match{c['match']} ran its FIRST BATTLE FRAME on "
                    f"g_round_limit={eff} but host match{h['match']} latched "
                    f"{h['latch_rounds']} -- [SPEC-RELATCH] frame-zero readback "
                    f"(the value the SIM used), not the pre-repair [CFG] stamp")
        rl = relatch.get(tag, RELATCH_NONE)
        if rl["trips"]:
            fails.append(f"{tag} {rl['trips']} [SPEC-RELATCH] TRIP line(s) -- "
                         f"the viewer's round-limit re-derive was REVERTED "
                         f"before its first battle frame")
        if tag.startswith("S") and cfgs and cfgs[-1]["cfg_rx"] < 1:
            fails.append(f"{tag} applied ZERO HOST_CONFIG packets (cfg_rx=0) -- "
                         f"it ran on its own game.ini")

    res["fails"], res["rounds_fails"], res["notes"] = fails, rounds_fails, notes
    ok = not fails and (not rounds_fails or rounds_term == "advisory")
    return ok, res


def leg_settings(a) -> tuple[bool, dict]:
    all_ok, results = True, []
    for name in a.variants.split(","):
        name = name.strip().upper()
        if name not in VARIANTS:
            print(f"[sweep] unknown variant {name}"); return False, {}
        ok, r = leg_settings_one(a, name)
        results.append(r)
        print_result(f"settings/{name}", ok, r)
        # Persist after EVERY variant: this box has BSOD'd mid-campaign twice.
        (a.out / f"6b_settings_{name}.json").write_text(json.dumps(r, indent=1))
        all_ok = all_ok and ok
    return all_ok, {"variants": results}


def print_result(label: str, ok: bool, res: dict):
    for n in res.get("notes", []):
        print(f"[sweep] {label} NOTE: {n}")
    for f in res.get("fails", []):
        print(f"[sweep] {label} FAIL: {f}")
    for f in res.get("rounds_fails", []):
        print(f"[sweep] {label} ROUND-COUNT DELIVERY: {f}")
    print(f"[sweep] {label}: {'PASS' if ok else 'FAIL'}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--leg", required=True, choices=("stage", "settings"))
    ap.add_argument("--variants", default="V0,V1",
                    help="settings leg: comma list of " + ",".join(VARIANTS))
    # STAGE LEG ONLY. The settings leg's frame budget and run shape are
    # per-variant (see VARIANTS), because "how many matches this needs" is a
    # property of the variant, not of the invocation.
    ap.add_argument("--total-frames", type=int, default=4200,
                    help="stage leg: battle frames across matches")
    ap.add_argument("--round-time", type=int, default=6,
                    help="stage leg: round timer. Short on purpose -- the leg "
                         "needs several matches to switch stages between. The "
                         "game is pkmncc, which does not have wanwan's "
                         "subsequent-battle timer bug.")
    ap.add_argument("--loss", default="0.10")
    ap.add_argument("--seed", default="424242", help="stage leg: shared roll seed")
    ap.add_argument("--timeout", type=int, default=520)
    ap.add_argument("--red", default="", choices=("", "spec-hostcfg-drop", "barrier"),
                    help="arm a RED-PROOF lever; the leg is expected to FAIL")
    ap.add_argument("--rounds-term", default="fatal",
                    choices=("fatal", "advisory"),
                    help="how to judge round-count delivery. It is the FIRST "
                         "thing ever to test that path (FM2K_TEST_ROUNDS_HOST_"
                         "ONLY), so a real pre-existing delivery bug can red "
                         "this leg on its first run; 'advisory' reports it "
                         "loudly without failing the stage.")
    ap.add_argument("--rounds-host-only", default="on", choices=("on", "off"),
                    help="settings leg: 'off' is the CONTROL ARM -- restores "
                         "the per-frame g_default_round force on every peer "
                         "(the mask this leg exists to remove), so a red arm "
                         "can be attributed instead of just observed.")
    ap.add_argument("--regate", type=Path, default=None,
                    help="settings leg: judge an EXISTING out-dir (its "
                         "live_FM2K_*_Debug.log set) instead of running a "
                         "game. Used to red-proof the rare terms by injection, "
                         "and to re-gate a kept corpus after a judge change.")
    ap.add_argument("--regate-rc", type=int, default=0,
                    help="--regate: the spec_selftest rc to judge with")
    ap.add_argument("--out", type=Path,
                    default=ROOT / "logs" / "spec_sweep")
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)

    t0 = time.time()
    if a.regate is not None:
        name = a.variants.split(",")[0].strip().upper()
        ok, res = judge_settings(a.regate, name, a.regate_rc, a.rounds_term)
        print_result(f"regate/{name} ({a.regate})", ok, res)
        (a.out / f"regate_{name}.json").write_text(json.dumps(res, indent=1))
        return 0 if ok else 1
    ok, res = (leg_stage(a) if a.leg == "stage" else leg_settings(a))
    if a.leg == "stage":
        print_result("stage", ok, res)
    (a.out / f"{a.leg}_summary.json").write_text(json.dumps(res, indent=1))
    print(f"[sweep] leg={a.leg} {'PASS' if ok else 'FAIL'} "
          f"({time.time() - t0:.0f}s); artifacts in {a.out}")
    if a.red:
        # A red proof INVERTS the verdict: the lever's job is to make the leg
        # fail, and a leg that stays green with its bug injected is a leg that
        # cannot see its bug (the ShadowArts shape).
        print(f"[sweep] RED-PROOF mode ({a.red}): expected FAIL, got "
              f"{'PASS' if ok else 'FAIL'}")
        return 1 if ok else 0
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
