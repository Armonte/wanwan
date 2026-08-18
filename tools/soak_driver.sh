#!/usr/bin/env bash
# soak_driver.sh -- extended local soak of the full stack (pre-cut confidence
# builder) doubling as the evidence trap for the intermittent player-plane
# `top=`/`bind=` divergence (phantom_hunt.md 1.3 ERRATA + RECURRENCE).
#
# ONE CYCLE =
#   A. run_all_tests.sh          (FULL=1 on odd cycles, base on even -- recorded)
#   B. wanwan extended netplay   (230ms / 50ms jitter / 20% loss, 6000 frames,
#                                 rotating seed 130 + (N-1) % 12, POOLSET armed)
#   C. vanpri spectated 16000f   (the stage-2g recipe, standalone)
#
# AFTER EVERY SUB-STAGE:
#   * any PVP `top=`/`bind=` mismatch line -> the whole out-dir is copied to
#     soak_runs/topred_<n>/ BEFORE anything else can run. That copy is the
#     entire point of this lane: the same recurrence has now been lost twice to
#     a shared out-dir.
#   * any `DESYNC #` (gekko's term, phantom lines excluded) or a red gate ->
#     capture, write SOAK_STOP, exit. A red in soak is a finding, not a flake.
#
# One line per cycle is appended to soak_ledger.md IMMEDIATELY, so a BSOD or a
# session restart loses at most the cycle in flight.
#
# Usage:  START=1 CYCLES=12 nohup bash tools/soak_driver.sh &
# Resume: START=<next cycle> CYCLES=12 nohup bash tools/soak_driver.sh &
set -uo pipefail

# IN-TREE as of 2026-08-18 (review H7). This driver is the ONLY end-to-end
# enforcement point of the machine-stall VOID rule, and a rule whose enforcement
# lives in one person's home directory is invisible to every other machine and
# lane. SOAK_DIR still points at the evidence/ledger directory, which is
# deliberately OUTSIDE the repo (it holds gigabytes of preserved game logs).
SOAK="${SOAK_DIR:-/home/teo/specrel-2026-08-07}"
REPO="${REPO:-/mnt/c/dev/wanwan}"
RUNS="$SOAK/soak_runs"
LEDGER="$SOAK/soak_ledger.md"
STOP="$SOAK/SOAK_STOP"
CYCLES="${CYCLES:-12}"
START="${START:-1}"
VANPRI_EXE=/mnt/c/games/2dfm/vanguard-princess/vanpri.exe

# FM2K_TEST_OUT_DIR MUST SIT ON THE WINDOWS FILESYSTEM. The game and launcher
# are Windows processes; a WSL-side path under /home is invisible to them, the
# harness writes .bat launchers and parity .pty paths through to_win(), and the
# run comes back with "host parity capture missing", "the HOST emitted no
# [CHECKSUM] at all" and spec_max_frame=0 -- a STRUCTURAL failure that looks
# nothing like a desync but fails the run. Cycle 1's legs B and C were lost to
# exactly this (recorded in soak_report.md; the campaign already had the trap
# written down for the gate stages, and this driver reintroduced it).
# So: out-dirs live under the repo on /mnt/c, and the logs are copied back into
# the cycle dir afterwards.
SOAK_WIN_ROOT="$REPO/tools/.soak"
mkdir -p "$SOAK_WIN_ROOT"

mkdir -p "$RUNS"
[ -f "$LEDGER" ] || {
    echo "# Soak ledger -- one line per cycle, appended as it completes." > "$LEDGER"
    echo "" >> "$LEDGER"
}

note() { echo "[soak] $*"; }
ledger() { echo "$*" >> "$LEDGER"; }

# ---------------------------------------------------------------------------
# Evidence capture. $1 = tag, $2 = out-dir (raw per-process logs), $3 = harness
# log, $4 = reason. Copies EVERYTHING that cannot be regenerated.
capture() {
    local tag="$1" outdir="$2" hlog="$3" reason="$4"
    local n=0 dst
    while :; do dst="$RUNS/${tag}_$n"; [ -d "$dst" ] || break; n=$((n+1)); done
    mkdir -p "$dst"
    echo "$reason" > "$dst/REASON.txt"
    { echo "captured: $(date -Is)"; echo "outdir:   $outdir";
      echo "harness:  $hlog"; } >> "$dst/REASON.txt"
    [ -f "$hlog" ] && cp -f "$hlog" "$dst/harness.log"
    if [ -d "$outdir" ]; then
        cp -f "$outdir"/live_FM2K_*_Debug.log "$dst/" 2>/dev/null
        cp -f "$outdir"/*.log "$dst/" 2>/dev/null
    fi
    # Pre-extract the pool evidence so the ticket starts from a diff, not a grep.
    for p in P1 P2; do
        local f="$dst/live_FM2K_${p}_Debug.log"
        [ -f "$f" ] || continue
        grep -a "\[POOLSET\]"  "$f" > "$dst/poolset_${p}.txt"  2>/dev/null
        grep -a "\[POOLTOPO\]" "$f" > "$dst/pooltopo_${p}.txt" 2>/dev/null
        grep -a "\[CHECKSUM\]" "$f" > "$dst/checksum_${p}.txt" 2>/dev/null
    done
    note "CAPTURED -> $dst ($reason)"
    echo "$dst"
}

# PVP pool-term advisory red? (the trap). Prints the matching lines.
pvp_pool_red() {   # $1 = harness log
    grep -aE "PVP match[0-9]+: (top|bind)= [0-9]+/[0-9]+ mismatches" "$1" 2>/dev/null
}

# Real gekko desync in the per-process logs (phantom health-slot lines excluded).
desync_lines() {   # $1 = out-dir
    grep -ah "DESYNC #" "$1"/live_FM2K_*_Debug.log 2>/dev/null | grep -av phantom
}

# Counter tallies for one out-dir -- the accumulating soak record.
tally() {   # $1 = out-dir
    local d="$1" rc endseam trip bg rdv freerun
    rc=$(grep -ah "\[RC-STATS\]"      "$d"/live_FM2K_*_Debug.log 2>/dev/null | wc -l)
    endseam=$(grep -ah "\[ENDSEAM-FREE\]" "$d"/live_FM2K_*_Debug.log 2>/dev/null | wc -l)
    trip=$(grep -ah "CSSPARK-TRIP\|ENDSEAM-OOB" "$d"/live_FM2K_*_Debug.log 2>/dev/null | wc -l)
    bg=$(grep -ah "\[BGMODE\] re-minimized\|degraded to NOACTIVATE\|not a recognised value" \
         "$d"/live_FM2K_*_Debug.log 2>/dev/null | wc -l)
    rdv=$(grep -ah "\[CSS-RDV\] rendezvous" "$d"/live_FM2K_*_Debug.log 2>/dev/null | wc -l)
    freerun=$(freerun_nonzero "$d" | wc -l)
    echo "rc=$rc endseam=$endseam trip=$trip bgwarn=$bg rdv=$rdv freerun_nz=$freerun"
}

# THE bbfc89f CONTRACT, PARSED. The lockstep-CSS fix makes both pre-rendezvous
# legs stall instead of free-running, so every [CSS-RDV] line must report
# freerun_sim_ticks=0. A nonzero value means the fix did not engage on that
# plane -- which is a genuine finding, not a metric. Parsed here because the
# campaign's own hardest-won lesson is that a detector nobody reads is not a
# detector; the line existing in the log is not the same as the line being
# checked.
freerun_nonzero() {   # $1 = out-dir; prints offending lines
    grep -ah "\[CSS-RDV\] rendezvous" "$1"/live_FM2K_*_Debug.log 2>/dev/null \
        | grep -av "freerun_sim_ticks=0 "
}

# Window-title sampler: a "-WARNING- Using slow software rendering" title is the
# background-mode regression signature (cnc-ddraw dropped to the software blit
# because the window was minimized before its first present).
# NOTE the >/dev/null on the background subshell. Without it the subshell
# inherits the command substitution's stdout pipe, so `$(title_watch_start ...)`
# blocks until the WATCHER exits -- i.e. forever. First launch of this driver
# wedged on exactly that before it ever started the gate.
title_watch_start() {
    local out="$1"
    ( while [ ! -f "$out.stop" ]; do
        tasklist.exe /v /fo csv 2>/dev/null | grep -a "WARNING" >> "$out" 2>/dev/null
        sleep 10
      done ) >/dev/null 2>&1 &
    echo $!
}
title_watch_stop() { local out="$1" pid="$2"; touch "$out.stop"; kill "$pid" 2>/dev/null; sleep 0.2; rm -f "$out.stop"; }

# ---------------------------------------------------------------------------
for (( N=START; N<START+CYCLES; N++ )); do
    [ -f "$STOP" ] && { note "SOAK_STOP present -- refusing to start cycle $N"; exit 1; }
    # A re-run of cycle N VOIDS the previous attempt at N (the WSL-crash resume
    # path: void the in-flight cycle, re-run it). Starting from a clean dir stops
    # a crashed attempt's partial logs from being read as this attempt's.
    CY="$RUNS/cycle$N"; rm -rf "$CY"; mkdir -p "$CY"
    if (( N % 2 == 1 )); then TIER=1; TIERNAME=FULL; else TIER=0; TIERNAME=base; fi
    SEED=$(( 130 + (N-1) % 12 ))
    T0=$(date +%s)
    note "=== CYCLE $N  tier=$TIERNAME seed=$SEED  $(date -Is) ==="

    TW="$CY/titles.txt"; TWPID=$(title_watch_start "$TW")

    # ---- A. the gate -------------------------------------------------------
    GLOG="$CY/A_gate.log"
    # SPEC_KEEP_GATES=4: the last four cycles' raw per-run logs stay on disk
    # (~250 MB) while anything RED is copied out by capture() the moment it is
    # seen, so pruning can never take evidence with it.
    ( cd "$REPO" && FULL=$TIER GATE_ID="soak_c${N}" SPEC_KEEP_GATES=4 \
        bash tools/run_all_tests.sh ) > "$GLOG" 2>&1
    GRC=$?
    # The gate wipes logs/run_all_tests at the START of the next invocation, so
    # the per-stage harness logs are copied into the cycle dir now.
    cp -rf "$REPO/logs/run_all_tests" "$CY/A_gate_logs" 2>/dev/null
    GPASS=$(grep -aoE "pass=[0-9]+" "$GLOG" | tail -1)
    GFAIL=$(grep -aoE "fail=[0-9]+" "$GLOG" | tail -1)
    GVERD=$([ "$GRC" -eq 0 ] && echo GREEN || echo RED)
    # The gate's own per-run dirs (SPEC_KEEP_GATES=99 + GATE_ID keep them).
    GDIR="$REPO/tools/.spec_runs/soak_c${N}"
    A_TOPRED=""
    if [ -d "$GDIR" ]; then
        for sub in "$GDIR"/*/; do
            [ -d "$sub" ] || continue
            # Match the stage log by dir name so the harness log travels with it.
            base=$(basename "$sub")
            # The stage log's name does not always equal the out-dir's name
            # (dir `2e_seam` <-> log `2e_seamdesync.log`). Falling back to the
            # WHOLE gate log was wrong: it contains every stage's advisory
            # lines, so one real red in stage 2 made every other stage's dir
            # look red too (cycle 1 captured a bogus `2e_seam`). A stage whose
            # log cannot be identified is now SKIPPED for the pool-term scan
            # (the desync scan below reads the per-process logs directly and is
            # unaffected).
            hl=""
            for cand in "$REPO/logs/run_all_tests/${base}.log" \
                        "$REPO"/logs/run_all_tests/"${base}"*.log; do
                [ -f "$cand" ] && { hl="$cand"; break; }
            done
            if [ -n "$hl" ] && [ -n "$(pvp_pool_red "$hl")" ]; then
                A_TOPRED="$A_TOPRED $base"
                capture "topred" "$sub" "$hl" \
                    "cycle $N stage A ($base): PVP top=/bind= advisory red" >/dev/null
            fi
            if [ -n "$(desync_lines "$sub")" ]; then
                capture "desync" "$sub" "${hl:-$GLOG}" \
                    "cycle $N stage A ($base): gekko DESYNC #" >/dev/null
            fi
        done
    fi

    # ---- MACHINE-STALL VOID (H2, 2026-08-18) -------------------------------
    # spec_selftest exits rc=4 and prints OVERALL VOID when every game process
    # stopped logging at the same instant. That is not a PASS and not a FAIL --
    # it is "this run measured nothing", and the 2026-08-18 cycle-4 red is what
    # happens when the distinction does not exist: a 6.26s all-four-process
    # freeze was triaged as a spectator starve. Rule: re-run the leg ONCE,
    # record BOTH attempts, and if it VOIDs twice in a row STOP the soak with a
    # machine-unfit reason (that is the memtest signal, not a product ticket).
    VOID_STREAK=0
    # ...and separately, how many of those VOIDs the harness could CONFIRM as
    # machine (review H5, 2026-08-18). A VOID whose viewer-content discriminator
    # did not fire is "this run measured nothing", not "your hardware is
    # broken": with only onset coincidence in hand, a reproducible PRODUCT hang
    # is still a candidate, and a soak that ends by telling the owner to run
    # memtest for a real deadlock is worse than no message at all. Only
    # MACHINE-CONFIRMED VOIDs may write the machine-unfit SOAK_STOP.
    VOID_CONFIRMED=0

    # ---- B. wanwan extended netplay ---------------------------------------
    BDIR="$SOAK_WIN_ROOT/c${N}_B"; rm -rf "$BDIR"; mkdir -p "$BDIR"
    BLOG="$CY/B_netplay.log"
    # FM2K_CSSWIN_FATAL=0 is REQUIRED here and is not a weakening. Leg B is
    # deliberately INSTRUMENT-FREE -- it does not arm FM2K_CSS_ANIM, because the
    # 2026-08-17 A/B proved the host-only character-select census roughly DOUBLES
    # the incidence of the real player-plane pool bug, and leg B's job is
    # extended soak at the field-representative perturbation level. But the
    # CSS-WIN gate's CSSANIM term cannot be computed without that census and
    # (correctly) refuses to call not-computed a pass -- so an instrument-free
    # run is OVERALL FAIL by construction, every time, for a term it was never
    # asked to measure. Cycle 2 leg B burned exactly that: rc=1 with all three
    # matches nobj/top/bind/crc IDENTICAL, spectator full-state identical, GATE
    # PASS and no DESYNC. The profile's real verdict is `DESYNC #`, which the
    # driver reads separately from the per-process logs.
    ( cd "$REPO" && FM2K_TEST_OUT_DIR="$BDIR" FM2K_POOLSET=1 FM2K_SEAM_TRACE=1 \
        FM2K_CSSWIN_FATAL=0 \
        FM2K_NET_DELAY_MS=230 FM2K_NET_JITTER_MS=50 FM2K_NET_LOSS=0.20 \
        FM2K_NET_SEED="$SEED" \
        timeout 600 python3 tools/spec_selftest.py --rounds 1 --round-time 20 \
        --total-frames 6000 --spectators css --keep ) > "$BLOG" 2>&1
    BRC=$?
    if [ "$BRC" -eq 4 ]; then
        note "cycle $N leg B VOID (machine stall) -- re-running once"
        grep -aE "^\[harness\] (MACHINE|OVERALL VOID)" "$BLOG" | tee -a "$LEDGER" >/dev/null
        cp -f "$BLOG" "$CY/B_netplay_VOID_attempt1.log"
        mkdir -p "$CY/B_logs_VOID_attempt1"; cp -f "$BDIR"/live_FM2K_*_Debug.log "$CY/B_logs_VOID_attempt1/" 2>/dev/null
        rm -rf "$BDIR"; mkdir -p "$BDIR"
        ( cd "$REPO" && FM2K_TEST_OUT_DIR="$BDIR" FM2K_POOLSET=1 FM2K_SEAM_TRACE=1 \
            FM2K_CSSWIN_FATAL=0 \
            FM2K_NET_DELAY_MS=230 FM2K_NET_JITTER_MS=50 FM2K_NET_LOSS=0.20 \
            FM2K_NET_SEED="$SEED" \
            timeout 600 python3 tools/spec_selftest.py --rounds 1 --round-time 20 \
            --total-frames 6000 --spectators css --keep ) > "$BLOG" 2>&1
        BRC=$?
        if [ "$BRC" -eq 4 ]; then
            VOID_STREAK=$((VOID_STREAK+1))
            grep -qa "MACHINE-CONFIRMED" "$BLOG" && VOID_CONFIRMED=$((VOID_CONFIRMED+1))
        fi
    fi
    B_DSY=$(desync_lines "$BDIR" | head -2 | tr '\n' ' ')
    B_TOP=$(pvp_pool_red "$BLOG" | head -2 | tr '\n' ' ')
    [ -n "$B_TOP" ] && capture "topred" "$BDIR" "$BLOG" \
        "cycle $N stage B (wanwan 6000f seed $SEED): PVP top=/bind= advisory red" >/dev/null
    [ -n "$B_DSY" ] && capture "desync" "$BDIR" "$BLOG" \
        "cycle $N stage B (wanwan 6000f seed $SEED): gekko DESYNC #" >/dev/null
    B_TALLY=$(tally "$BDIR")

    # ---- C. vanpri spectated 16000f ---------------------------------------
    CDIR="$SOAK_WIN_ROOT/c${N}_C"; rm -rf "$CDIR"; mkdir -p "$CDIR"
    CLOG="$CY/C_vanpri.log"
    if [ -f "$VANPRI_EXE" ]; then
        ( cd "$REPO" && FM2K_TEST_OUT_DIR="$CDIR" FM2K_CSS_ANIM=1 \
            FM2K_SPEC_DEEP_JOIN=1 FM2K_NET_DELAY_MS=100 FM2K_NET_JITTER_MS=30 \
            FM2K_NET_LOSS=0.10 \
            timeout 900 python3 -u tools/spec_selftest.py --game vanpri \
            --game-exe "$VANPRI_EXE" --rounds 1 --total-frames 16000 \
            --spectators battle1,css2 --record-timeout 600 --keep ) > "$CLOG" 2>&1
        CRC=$?
        if [ "$CRC" -eq 4 ]; then
            note "cycle $N leg C VOID (machine stall) -- re-running once"
            grep -aE "^\[harness\] (MACHINE|OVERALL VOID)" "$CLOG" | tee -a "$LEDGER" >/dev/null
            cp -f "$CLOG" "$CY/C_vanpri_VOID_attempt1.log"
            mkdir -p "$CY/C_logs_VOID_attempt1"; cp -f "$CDIR"/live_FM2K_*_Debug.log "$CY/C_logs_VOID_attempt1/" 2>/dev/null
            rm -rf "$CDIR"; mkdir -p "$CDIR"
            ( cd "$REPO" && FM2K_TEST_OUT_DIR="$CDIR" FM2K_CSS_ANIM=1 \
                FM2K_SPEC_DEEP_JOIN=1 FM2K_NET_DELAY_MS=100 FM2K_NET_JITTER_MS=30 \
                FM2K_NET_LOSS=0.10 \
                timeout 900 python3 -u tools/spec_selftest.py --game vanpri \
                --game-exe "$VANPRI_EXE" --rounds 1 --total-frames 16000 \
                --spectators battle1,css2 --record-timeout 600 --keep ) > "$CLOG" 2>&1
            CRC=$?
            if [ "$CRC" -eq 4 ]; then
                VOID_STREAK=$((VOID_STREAK+1))
                grep -qa "MACHINE-CONFIRMED" "$CLOG" && VOID_CONFIRMED=$((VOID_CONFIRMED+1))
            fi
        fi
    else
        echo "vanpri not installed" > "$CLOG"; CRC=77
    fi
    C_DSY=$(desync_lines "$CDIR" | head -2 | tr '\n' ' ')
    C_TOP=$(pvp_pool_red "$CLOG" | head -2 | tr '\n' ' ')
    [ -n "$C_TOP" ] && capture "topred" "$CDIR" "$CLOG" \
        "cycle $N stage C (vanpri 16000f): PVP top=/bind= advisory red" >/dev/null
    [ -n "$C_DSY" ] && capture "desync" "$CDIR" "$CLOG" \
        "cycle $N stage C (vanpri 16000f): gekko DESYNC #" >/dev/null
    C_TALLY=$(tally "$CDIR")

    title_watch_stop "$TW" "$TWPID"
    SWRENDER=$( [ -s "$TW" ] && echo YES || echo no )

    # Copy the per-process logs off the Windows filesystem into the cycle dir
    # so a later prune of tools/.soak cannot take the record with it.
    for leg in B:"$BDIR" C:"$CDIR"; do
        tag="${leg%%:*}"; src="${leg#*:}"
        [ -d "$src" ] || continue
        mkdir -p "$CY/${tag}_logs"
        cp -f "$src"/live_FM2K_*_Debug.log "$CY/${tag}_logs/" 2>/dev/null
    done

    T1=$(date +%s); MINS=$(( (T1-T0)/60 ))

    B_VERD=$([ "$BRC" -eq 0 ] && echo PASS || { [ "$BRC" -eq 4 ] && echo "VOID(machine stall)" || echo "FAIL(rc=$BRC)"; })
    C_VERD=$([ "$CRC" -eq 0 ] && echo PASS || { [ "$CRC" -eq 77 ] && echo SKIP || { [ "$CRC" -eq 4 ] && echo "VOID(machine stall)" || echo "FAIL(rc=$CRC)"; }; })

    # ---- POST-bbfc89f detectors, parsed ------------------------------------
    # Both are STOP conditions from cycle 3 onward. Before the lockstep fix a
    # top=/bind= red was an expected ~1-in-3 advisory and the driver captured and
    # continued; on the fixed build the expectation is ZERO, so any occurrence is
    # a genuine finding and gets the full stop-and-capture treatment.
    FREERUN_NZ=""
    for legdir in "$BDIR" "$CDIR"; do
        [ -d "$legdir" ] || continue
        fr=$(freerun_nonzero "$legdir" | head -2 | tr '\n' ' ')
        [ -n "$fr" ] && { FREERUN_NZ="$FREERUN_NZ $fr"
            capture "freerun" "$legdir" "$BLOG" \
                "cycle $N: [CSS-RDV] freerun_sim_ticks != 0 -- the lockstep fix did not engage" >/dev/null; }
    done
    if [ -d "$GDIR" ]; then
        for sub in "$GDIR"/*/; do
            [ -d "$sub" ] || continue
            fr=$(freerun_nonzero "$sub" | head -2 | tr '\n' ' ')
            [ -n "$fr" ] && { FREERUN_NZ="$FREERUN_NZ $fr"
                capture "freerun" "$sub" "$GLOG" \
                    "cycle $N gate $(basename "$sub"): [CSS-RDV] freerun_sim_ticks != 0" >/dev/null; }
        done
    fi
    ANY_TOPRED=""
    [ -n "${A_TOPRED// /}" ] && ANY_TOPRED="gate:$A_TOPRED"
    [ -n "$B_TOP" ] && ANY_TOPRED="$ANY_TOPRED legB"
    [ -n "$C_TOP" ] && ANY_TOPRED="$ANY_TOPRED legC"

    ledger "| $N | $TIERNAME | ${MINS}m | gate=$GVERD ${GPASS:-} ${GFAIL:-} | wanwan(seed $SEED)=$B_VERD | vanpri=$C_VERD | topred:${ANY_TOPRED:-none} | desync:${B_DSY:-none}${C_DSY:+ C} | freerun_nz:${FREERUN_NZ:-none} | swrender=$SWRENDER | B[$B_TALLY] C[$C_TALLY] |"
    note "cycle $N done in ${MINS}m -- gate=$GVERD B=$B_VERD C=$C_VERD topred=${ANY_TOPRED:-none}"

    # ---- stop rules --------------------------------------------------------
    # STOP on: a red gate, any gekko DESYNC on either leg, or leg C failing.
    # NOT on leg B's exit code alone, and that asymmetry is deliberate: leg B
    # runs the EXTREME profile (230/50/0.20) where the campaign has already
    # established that OVERALL FAIL from tail-CINPUT truncation and spectator
    # stalls are known artifacts -- `seam_desync_gate.sh` judges that profile by
    # `DESYNC #` alone for exactly this reason, and an OVERALL-based stop would
    # halt the soak on non-bugs. Leg B's rc is still recorded and its evidence
    # captured. Leg C runs the gentle stage-2g recipe whose shipped verdict IS
    # the harness exit code, so there rc != 0 is a real stop.
    if [ "$BRC" -ne 0 ] && [ "$BRC" -ne 4 ]; then
        capture "legBfail" "$BDIR" "$BLOG" \
            "cycle $N leg B rc=$BRC with no DESYNC -- extreme-profile OVERALL FAIL, recorded not fatal" >/dev/null
    fi
    # A VOID leg is NOT a stop and NOT a pass: it already re-ran once above.
    # Two VOIDs in one cycle means the machine, not the product, and the soak
    # stops on THAT -- with a reason that points at memtest rather than at a
    # ticket. rc=4 is excluded from the ordinary leg-C stop rule for the same
    # reason a story-only rc=3 is: it is not a verdict about the product.
    #
    # ...but ONLY when the harness could CONFIRM the machine (review H5). An
    # unconfirmed VOID -- onsets coincided, no viewer-content evidence -- may
    # still be a reproducible PRODUCT hang, which reproduces on the retry
    # exactly like a sick machine does. Calling that "memtest" is the worst
    # possible terminal message for a real deadlock, so the driver keeps
    # cycling and says what it does and does not know instead.
    if [ "$VOID_STREAK" -ge 2 ] && [ "$VOID_CONFIRMED" -ge 1 ]; then
        {   echo "STOPPED after cycle $N -- MACHINE UNFIT"
            echo "Two legs VOIDed on machine stalls in one cycle, each after a"
            echo "re-run, and at least one was MACHINE-CONFIRMED (a viewer went"
            echo "silent while still HOLDING buffered content -- neither a"
            echo "starve nor a host wedge can do that). No product term measured"
            echo "anything. Do NOT file this as a regression. Check: memtest,"
            echo "real-time AV scanning of the game dir, the 10s tasklist"
            echo "poller, and the 8-process load."
            date -Is
        } > "$STOP"
        ledger ""
        ledger "**SOAK STOPPED after cycle $N -- MACHINE UNFIT (2 machine-stall VOIDs, >=1 CONFIRMED).**"
        note "STOP: machine unfit to measure with. Halting."
        exit 1
    fi
    if [ "$VOID_STREAK" -ge 2 ]; then
        ledger "- cycle $N: **2 VOIDs, NONE machine-confirmed** -- NOT calling the"
        ledger "  machine unfit. Read cycle$N's preserved logs: a reproducible"
        ledger "  product hang looks exactly like this and must not be excused."
        note "WARN: 2 unconfirmed VOIDs in cycle $N -- continuing, evidence kept."
    fi
    if [ "$GVERD" = RED ] || [ -n "$B_DSY" ] || [ -n "$C_DSY" ] \
       || [ -n "$ANY_TOPRED" ] || [ -n "${FREERUN_NZ// /}" ] \
       || { [ "$CRC" -ne 0 ] && [ "$CRC" -ne 77 ] && [ "$CRC" -ne 4 ]; }; then
        {   echo "STOPPED after cycle $N"
            echo "gate=$GVERD  B_desync='$B_DSY'  C_desync='$C_DSY'  C_rc=$CRC"
            echo "topred='$ANY_TOPRED'   (post-bbfc89f this must be empty)"
            echo "freerun_nonzero='$FREERUN_NZ'"
            date -Is
        } > "$STOP"
        ledger ""
        ledger "**SOAK STOPPED after cycle $N** -- see SOAK_STOP and soak_runs/."
        note "STOP: a red in soak is a finding, not a flake. Halting."
        exit 1
    fi
done
note "soak complete: cycles $START..$((START+CYCLES-1))"
