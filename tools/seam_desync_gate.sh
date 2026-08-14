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
# Phase 2c report first (/home/teo/specrel-2026-08-07/seam_p2c_implementation.md):
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
# Usage:
#   tools/seam_desync_gate.sh                 # 1 run, seed 130
#   SEAM_SEEDS="130 131" tools/seam_desync_gate.sh
#
# Env: SEAM_SEEDS(130) SEAM_TOTAL(6000) SEAM_TIMEOUT(480)
#      SEAM_GAMEDIR(/mnt/c/games/2dfm/wanwan) SEAM_OUT(logs/seam_desync)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SEAM_SEEDS="${SEAM_SEEDS:-130}"
SEAM_TOTAL="${SEAM_TOTAL:-6000}"
SEAM_TIMEOUT="${SEAM_TIMEOUT:-480}"
SEAM_GAMEDIR="${SEAM_GAMEDIR:-/mnt/c/games/2dfm/wanwan}"
OUT="${SEAM_OUT:-$ROOT/logs/seam_desync}"
SPEC_LIVE="$ROOT/tools/.spec_selftest"
mkdir -p "$OUT"

kill_games() { for p in WonderfulWorld_ver_0946 WonderfulRvl FM2K_RollbackLauncher; do
    taskkill.exe /F /IM "${p}.exe" >/dev/null 2>&1; done; }

seam_run() {   # $1 = net seed; 0 = PASS
    local seed="$1"
    local log="$OUT/seam_seed${seed}.log"
    local ev="$OUT/seam_seed${seed}_evidence.txt"
    kill_games; sleep 0.6
    # Evidence hygiene: both the preserved live logs AND the seam CSVs are
    # per-run files that the NEXT run overwrites. A stale CSV left by an
    # earlier run would otherwise be checked as if it belonged to this one.
    rm -f "$SPEC_LIVE"/live_FM2K_*_Debug.log
    rm -f "$SEAM_GAMEDIR"/FM2K_P1_seamring.csv "$SEAM_GAMEDIR"/FM2K_P2_seamring.csv
    FM2K_SEAM_TRACE=1 FM2K_NET_DELAY_MS=230 FM2K_NET_JITTER_MS=50 \
      FM2K_NET_LOSS=0.20 FM2K_NET_SEED="$seed" \
      timeout "$SEAM_TIMEOUT" python3 "$ROOT/tools/spec_selftest.py" \
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
                  "$OUT/seam_seed${seed}_${p}_seamring.csv" 2>/dev/null; }
    done
    if [ ${#rings[@]} -gt 0 ]; then
        python3 "$ROOT/tools/seam_ring_check.py" "${rings[@]}" \
            > "$OUT/seam_seed${seed}_ring.txt" 2>&1 || ring_rc=$?
        ring_note=$(grep -a "^SEAM-RING-CHECK:" "$OUT/seam_seed${seed}_ring.txt" 2>/dev/null | tail -1)
    fi
    {   echo "== seed $seed  harness rc=$rc  [ROUND-START]=$rounds"
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
    echo "[seam] seed $seed: rc=$rc rounds=$rounds ring=${ring_note:-n/a}"
    if [ -n "$dsy" ]; then
        echo "[seam] seed $seed: FAIL -- GekkoNet DESYNC (match-end seam class):"
        echo "$dsy" | head -3 | sed 's/^/    /'
        return 1
    fi
    if [ "$ring_rc" != 0 ]; then
        echo "[seam] seed $seed: FAIL -- seam_ring_check violations (a resim did not"
        echo "[seam]           reproduce its forward save); see $OUT/seam_seed${seed}_ring.txt"
        grep -a "VIOLATION" "$OUT/seam_seed${seed}_ring.txt" 2>/dev/null | head -3 | sed 's/^/    /'
        return 1
    fi
    if [ "$rounds" -lt 2 ]; then
        echo "[seam] seed $seed: FAIL -- NO COVERAGE: only $rounds [ROUND-START] in the host"
        echo "[seam]           log, so the session never crossed a match-end seam. That is a"
        echo "[seam]           harness/deployment failure, not a green run."
        return 1
    fi
    echo "[seam] seed $seed: PASS (0 DESYNC, $rounds matches started)"
    return 0
}

echo "[seam] profile: delay=230ms jitter=50ms loss=0.20 --round-time 20"
echo "[seam]          --total-frames $SEAM_TOTAL --spectators css  seeds: $SEAM_SEEDS"
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
echo "[seam] seamdesync: FAIL (evidence: $OUT/seam_seed*_evidence.txt)"; exit 1
