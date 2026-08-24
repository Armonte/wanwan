#!/usr/bin/env bash
# seam_desync_gate.sh -- the MATCH-END SEAM rollback-desync gate (967f89f class).
#
# Runs the seeded extreme-profile multi-match spectator session and returns ONE
# exit code (0 = clean, 1 = the class is present / no evidence). Called by
# tools/run_all_tests.sh as stage "seamdesync"; runnable standalone, which is
# how the red-on-broken proof is taken against an older build.
#
# WHAT IT OWNS. Commit 967f89f (the host match-end AV fix) parked type-4 script
# VMs from a SaveState_Load site whose gate depended on THIS peer's rollback
# schedule, so two peers re-simulating the frames around a match-end seam froze
# their RNG at different values and gekko's fingerprint comparison went red
# (`DESYNC #1 f~2526-2632`). It shipped and survived the whole gate, because NO
# stage ran a multi-match session at a profile harsh enough to force a
# non-identity rollback across the battle-exit boundary. That gap is what this
# script closes.
#
# THE RECIPE IS NOT ARBITRARY. It is the V1 shape from the Phase 2c validation
# battery (230ms delay / 50ms jitter / 20% loss, --round-time 20,
# --total-frames 6000 -> ~3 matches, one css spectator), which reproduced the
# bug ~6/6 on the broken build and 0/8 on the fixed one. The SHORTER stressor
# shape (--round-time 5, tens of matches) is deliberately NOT used: it ends with
# the laggard peer at rc=124 in ~2 of 4 runs because one peer hits the
# confirmed-frame cap and auto-terminates a few frames before the other, which
# would make this gate flaky for a reason that has nothing to do with the bug.
#
# THE VERDICT IS DELIBERATELY NARROW: a `DESYNC #` line (GekkoNet's per-frame
# state-fingerprint mismatch) in EITHER player log, and nothing else. At this
# profile the run legitimately produces noise that is NOT this bug and must not
# red the gate:
#   * OVERALL FAIL from tail CINPUT truncation at a hard-terminated final match
#   * spectator stalls / late-join CHECKSUM lines / CSS-SPEC on the trailing
#     char-select the host never finished
#   * rc=124 on a laggard peer
# Anyone tempted to "strengthen" this by asserting OVERALL PASS should read the
# Phase 2c report first (indexed in docs/dev/matchend_seam_campaign.md):
# those artifacts are a property of the profile, and gating on them buys nothing
# while guaranteeing red weeks.
#
# SECOND, STRONGER CRITERION (when the build supports it): gekko only compares
# ONE frame per tick, so "no DESYNC line" is a sampled statement.
# FM2K_SEAM_TRACE=1 makes the hook record every SaveState_Save into a ring, and
# tools/seam_ring_check.py asserts the rollback contract itself on it -- for
# every frame saved more than once, the LAST save must be bit-identical to the
# FIRST. That check is red-on-broken (24 violations naming `vm_live 146 -> 0`
# plus a frozen rng under FM2K_SEAM_LEGACY_PARK=1) and green-on-fixed. It is
# applied ONLY when the CSVs exist, because a build older than the
# instrumentation (including the red-proof build this gate was validated
# against) emits none, and "no CSV" must never be confusable with "no
# violations".
#
# NO-VACUOUS-PASS GUARD: a green verdict additionally requires the host log to
# show >= 2 `[ROUND-START]` lines, i.e. the session actually completed a match
# and started another one -- a run that never reached a match-end seam cannot
# have tested this class. `[ROUND-START]` is used (not a harness-side counter)
# because every hook build back through the broken one emits it, so the guard
# measures the same thing on both arms of the red/green proof.
#
# CONTENT MONOCULTURE (Wave 2). Every spectator/seam stage ran wanwan and only
# wanwan, which is the ShadowArts shape: the vanpri leg reproduced a real
# spectator divergence 1 run in 1 the first time anyone pointed a stage at it.
# SEAM_GAME / SEAM_GAME_EXE add the second game to THIS gate. Lane A already
# took the red/green pair on it (its runs 8 and 9, same recipe shape as below,
# ~2 min each): FM2K_SEAM_LEGACY_PARK=1 -> ring check FAIL with 9 violations
# naming `vm_live 59 -> 0` and a frozen rng, i.e. the wanwan mechanism
# reproduced on vanpri for the first time and with NO `DESYNC #` line (the
# sampled comparator missed what the ring caught); default -> PASS, 389
# multiply-recorded frames, 0 violations. So the fix has no vanpri-shaped hole
# and this leg is red-proofed before it ships.
#
# SEAM_GAME_EXE IS LOAD-BEARING when SEAM_GAME is set (Phase 4e review A4d):
# it is passed through as --game-exe, which the harness fails on (rc=2) if the
# file is absent, instead of silently resolving its own hardcoded path. Do NOT
# use the 16000-frame / 2-spectator shape at this profile: Lane A runs 6 and 7
# both died on the #56 battle-entry wedge at match 2, in BOTH arms.
#
# Usage:
#   tools/seam_desync_gate.sh                 # 1 run, seed 130, wanwan
#   SEAM_SEEDS="130 131" tools/seam_desync_gate.sh
#   SEAM_GAME=vanpri SEAM_GAME_EXE=/mnt/c/games/2dfm/vanguard-princess/vanpri.exe \
#     SEAM_GAMEDIR=/mnt/c/games/2dfm/vanguard-princess tools/seam_desync_gate.sh
#
# Env: SEAM_SEEDS(130) SEAM_TOTAL(6000) SEAM_TIMEOUT(480)
#      SEAM_GAMEDIR(/mnt/c/games/2dfm/wanwan) SEAM_OUT(logs/seam_desync)
#      SEAM_GAME("") SEAM_GAME_EXE("") SEAM_TAG(derived)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SEAM_SEEDS="${SEAM_SEEDS:-130}"
SEAM_TOTAL="${SEAM_TOTAL:-6000}"
SEAM_TIMEOUT="${SEAM_TIMEOUT:-480}"
SEAM_GAMEDIR="${SEAM_GAMEDIR:-/mnt/c/games/2dfm/wanwan}"
SEAM_GAME="${SEAM_GAME:-}"
SEAM_GAME_EXE="${SEAM_GAME_EXE:-}"
# Evidence files are named per game so a wanwan leg and a vanpri leg sharing one
# SEAM_OUT cannot overwrite each other's logs/CSVs.
SEAM_TAG="${SEAM_TAG:-${SEAM_GAME:-wanwan}}"
OUT="${SEAM_OUT:-$ROOT/logs/seam_desync}"
# SEAM_SPEC_LIVE: where spec_selftest preserves its per-process logs. Defaults
# to the historical shared dir when run standalone; run_all_tests hands over a
# per-invocation one so a gate run can never overwrite another gate run's raw
# evidence (2026-08-17 -- two `top=` recurrences were lost to exactly that).
# Per-seed subdirs below, for the same reason within one invocation.
SPEC_LIVE_BASE="${SEAM_SPEC_LIVE:-$ROOT/tools/.spec_selftest}"
SPEC_LIVE="$SPEC_LIVE_BASE"
mkdir -p "$OUT"

# --game/--game-exe plumbing. Both or neither: --game without --game-exe is the
# exact silent-fallback defect A4d closed on stage 2g, so refuse it here.
GAME_ARGS=()
if [ -n "$SEAM_GAME" ]; then
    if [ -z "$SEAM_GAME_EXE" ]; then
        echo "[seam] FAIL -- SEAM_GAME=$SEAM_GAME set without SEAM_GAME_EXE."
        echo "[seam]        The harness would fall back to its own hardcoded path"
        echo "[seam]        for that registry key and the stage would silently test"
        echo "[seam]        a different install (review A4d). Refusing."
        exit 2
    fi
    if [ ! -f "$SEAM_GAME_EXE" ]; then
        echo "[seam] FAIL -- SEAM_GAME_EXE=$SEAM_GAME_EXE does not exist."
        exit 2
    fi
    GAME_ARGS=(--game "$SEAM_GAME" --game-exe "$SEAM_GAME_EXE")
fi

# The list must cover WHATEVER GAME THIS LEG RUNS, not just wanwan. Under
# run_all_tests the stage's own kill_games (which does include vanpri) runs
# first, so this was harmless there -- but a STANDALONE vanpri leg leaked the
# game process, and a multi-seed SEAM_SEEDS would have started seed 2 with seed
# 1's game still alive. vanpri is named explicitly because it is the shipped
# second leg; SEAM_GAME_EXE's basename covers every future one.
kill_games() {
    local names=(WonderfulWorld_ver_0946 WonderfulRvl vanpri FM2K_RollbackLauncher)
    if [ -n "$SEAM_GAME_EXE" ]; then
        local stem; stem="$(basename "$SEAM_GAME_EXE")"; stem="${stem%.exe}"
        [ -n "$stem" ] && names+=("$stem")
    fi
    for p in "${names[@]}"; do
        taskkill.exe /F /IM "${p}.exe" >/dev/null 2>&1
    done
}

seam_run() {   # $1 = net seed; 0 = PASS
    local seed="$1"
    local log="$OUT/seam_${SEAM_TAG}_seed${seed}.log"
    local ev="$OUT/seam_${SEAM_TAG}_seed${seed}_evidence.txt"
    # Per-seed, per-tag live dir. A multi-seed SEAM_SEEDS used to have seed 2
    # overwrite seed 1's raw logs, and a wanwan leg + a vanpri leg sharing one
    # invocation overwrote each other's (only the derived evidence files were
    # named per game).
    SPEC_LIVE="$SPEC_LIVE_BASE/seam_${SEAM_TAG}_seed${seed}"
    mkdir -p "$SPEC_LIVE"
    kill_games; sleep 0.6
    # Evidence hygiene: both the preserved live logs AND the seam CSVs are
    # per-run files that the NEXT run overwrites. A stale CSV left by an
    # earlier run would otherwise be checked as if it belonged to this one.
    rm -f "$SPEC_LIVE"/live_FM2K_*_Debug.log
    rm -f "$SEAM_GAMEDIR"/FM2K_P1_seamring.csv "$SEAM_GAMEDIR"/FM2K_P2_seamring.csv
    FM2K_TEST_OUT_DIR="$SPEC_LIVE" \
      FM2K_SEAM_TRACE=1 FM2K_SEAM_HASH="${FM2K_SEAM_HASH:-0}" \
      FM2K_NET_DELAY_MS=230 FM2K_NET_JITTER_MS=50 \
      FM2K_NET_LOSS=0.20 FM2K_NET_SEED="$seed" \
      timeout "$SEAM_TIMEOUT" python3 "$ROOT/tools/spec_selftest.py" \
        "${GAME_ARGS[@]+"${GAME_ARGS[@]}"}" \
        --rounds 1 --round-time 20 --total-frames "$SEAM_TOTAL" \
        --spectators css --keep > "$log" 2>&1
    local rc=$?
    # Read the players' logs from BOTH the harness-preserved copies and the
    # game directory: if `timeout` killed the harness itself, preservation
    # never ran and only the game-dir copies exist. Identical content is
    # deduped by sort -u. "phantom" lines are gekko health-slot artifacts the
    # hook already declines to act on, never a state divergence.
    local plogs=() f
    for f in "$SPEC_LIVE/live_FM2K_P1_Debug.log" "$SPEC_LIVE/live_FM2K_P2_Debug.log" \
             "$SEAM_GAMEDIR/logs/FM2K_P1_Debug.log" "$SEAM_GAMEDIR/logs/FM2K_P2_Debug.log"; do
        [ -f "$f" ] && plogs+=("$f")
    done
    local dsy=""
    [ ${#plogs[@]} -gt 0 ] && dsy=$(grep -ah "DESYNC #" "${plogs[@]}" 2>/dev/null \
        | grep -av "phantom" | sed 's/^\[[0-9:.]*\] //' | sort -u)
    local host_log="$SPEC_LIVE/live_FM2K_P1_Debug.log"
    [ -f "$host_log" ] || host_log="$SEAM_GAMEDIR/logs/FM2K_P1_Debug.log"
    local rounds=0
    [ -f "$host_log" ] && rounds=$(grep -ac "\[ROUND-START\]" "$host_log" 2>/dev/null)
    # Seam ring: check whatever CSVs this build produced, and keep a copy --
    # the next run's `rm -f` above would otherwise take the evidence with it.
    local rings=() p ring_rc=0
    local ring_note="n/a (no seam CSV -- build predates FM2K_SEAM_TRACE)"
    for p in P1 P2; do
        [ -f "$SEAM_GAMEDIR/FM2K_${p}_seamring.csv" ] && {
            rings+=("$SEAM_GAMEDIR/FM2K_${p}_seamring.csv")
            cp -f "$SEAM_GAMEDIR/FM2K_${p}_seamring.csv" \
                  "$OUT/seam_${SEAM_TAG}_seed${seed}_${p}_seamring.csv" 2>/dev/null; }
    done
    if [ ${#rings[@]} -gt 0 ]; then
        python3 "$ROOT/tools/seam_ring_check.py" "${rings[@]}" \
            > "$OUT/seam_${SEAM_TAG}_seed${seed}_ring.txt" 2>&1 || ring_rc=$?
        ring_note=$(grep -a "^SEAM-RING-CHECK:" "$OUT/seam_${SEAM_TAG}_seed${seed}_ring.txt" 2>/dev/null | tail -1)
    fi
    {   echo "== game ${SEAM_TAG}  seed $seed  harness rc=$rc  [ROUND-START]=$rounds"
        echo "== DESYNC # lines (the verdict) =="
        [ -n "$dsy" ] && echo "$dsy" || echo "(none)"
        echo "== seam ring =="
        echo "${ring_note:-(no summary line)}"
        echo "== [SEAM] summaries (absent on pre-instrumentation builds) =="
        grep -ah "\[SEAM\] summary" "${plogs[@]}" 2>/dev/null | tail -4
        grep -ahc "\[SEAM\] OPEN" "${plogs[@]}" 2>/dev/null | paste -sd' ' - | sed 's/^/[SEAM] OPEN counts per log: /'
        echo "== harness verdicts (INFORMATIONAL -- not gated) =="
        grep -aE "rcs:|CINPUT P1-vs-P2|CHECKSUM S|CSS-DET|OVERALL" "$log" 2>/dev/null
    } > "$ev" 2>&1
    echo "[seam] ${SEAM_TAG} seed $seed: rc=$rc rounds=$rounds ring=${ring_note:-n/a}"
    if [ -n "$dsy" ]; then
        echo "[seam] ${SEAM_TAG} seed $seed: FAIL -- GekkoNet DESYNC (match-end seam class):"
        echo "$dsy" | head -3 | sed 's/^/    /'
        return 1
    fi
    if [ "$ring_rc" != 0 ]; then
        echo "[seam] ${SEAM_TAG} seed $seed: FAIL -- seam_ring_check violations (a resim did not"
        echo "[seam]           reproduce its forward save); see $OUT/seam_${SEAM_TAG}_seed${seed}_ring.txt"
        grep -a "VIOLATION" "$OUT/seam_${SEAM_TAG}_seed${seed}_ring.txt" 2>/dev/null | head -3 | sed 's/^/    /'
        return 1
    fi
    if [ "$rounds" -lt 2 ]; then
        echo "[seam] ${SEAM_TAG} seed $seed: FAIL -- NO COVERAGE: only $rounds [ROUND-START] in the host"
        echo "[seam]           log, so the session never crossed a match-end seam. That is a"
        echo "[seam]           harness/deployment failure, not a green run."
        return 1
    fi
    echo "[seam] ${SEAM_TAG} seed $seed: PASS (0 DESYNC, $rounds matches started)"
    return 0
}

echo "[seam] profile: delay=230ms jitter=50ms loss=0.20 --round-time 20"
echo "[seam]          --total-frames $SEAM_TOTAL --spectators css  seeds: $SEAM_SEEDS"
echo "[seam] game:    ${SEAM_TAG}  exe=${SEAM_GAME_EXE:-<harness default>}  dir=$SEAM_GAMEDIR"
echo "[seam] verdict: GekkoNet 'DESYNC #' only (+ seam_ring_check when CSVs exist)."
echo "[seam]          OVERALL FAIL / spectator stalls / rc=124 are NOT failures here."
ok=1
for sd in $SEAM_SEEDS; do
    seam_run "$sd" || ok=0
done
kill_games
if [ "$ok" = 1 ]; then
    echo "[seam] seamdesync: PASS (evidence: $OUT)"; exit 0
fi
echo "[seam] seamdesync: FAIL (evidence: $OUT/seam_${SEAM_TAG}_seed*_evidence.txt)"; exit 1
