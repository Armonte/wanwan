// trampoline_spec_watchdog.cpp -- the spectator's LIVENESS watchdog, split out
// of trampoline_spectator.cpp (which was at 998 lines; the 1000-line rule).
//
// One concern: "is this stream still alive, and if not, what do we do about
// it". Everything that decides whether the viewer keeps running lives here --
// the graceful SESSION_END drains, the self-stall credit, the post-stall rung
// catch-up, the host-gone give-up and the connect-establishment deadline.
// Pure move of the block that used to sit inline in RunSpectatorTick, PLUS the
// two 2026-08-18 additions marked [SPEC-SELFSTALL] below.
#include "trampoline_internal.h"
#include "globals.h"
#include "../netplay/spectator_node.h"
#include "../netplay/control_channel.h"   // ControlChannel_TestFreezePoll (test lever)
#include <SDL3/SDL_log.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <windows.h>

// A tick gap above this is a SELF-STALL: this viewer process did not run.
// 500ms is 50x the 10ms spectator tick and well below the lowest ladder rung
// (3500ms), so it can never be reached by ordinary scheduling jitter and can
// never mask a rung.
static constexpr uint32_t kSelfStallMs = 500;
// TOTAL self-stall credit one session may hand itself, in ms. The bound is the
// whole reason this is safe: without it a viewer that is genuinely wedged
// (its own tick loop broken) would credit itself forever and NEVER give up on
// a dead host.
//
// THE NUMBER IS DERIVED, NOT PICKED (adversarial review H2, 2026-08-18). The
// first version said 8000 "is below the 12000ms give-up budget, so a viewer
// that burns the whole cap still gives up on the very next real silence". That
// sentence was WRONG BY EIGHT SECONDS: every credited ms is subtracted from the
// stall clock the give-up reads, so the credit ADDS to the budget rather than
// fitting inside it. The real arithmetic, and the ceiling it is solved against:
//
//   declared-dead latency = gone_budget + credit spent this session
//   gone_budget           = s_gone_ms                      (ordinary path, 12000 default)
//                         = max(s_gone_ms, 20000)          (graceful SESSION_END drain)
//   ceiling               = kDeclaredDeadCeilingMs = 20000
//
//   ordinary 12000 budget : cap 4000 -> worst case 16000ms  (was 20000)
//   graceful  20000 budget: cap    0 -> worst case 20000ms  (was 28000)
//
// 20000 is not a new number: it is the largest declared-dead latency the
// shipped tree ALREADY accepts on any path (the graceful drain's own budget).
// The credit is simply forbidden to push any path past the worst the product
// already tolerates. Same shape as the escalation-budget precedent, where
// SPECTATOR_STARVE_ESCALATE_HARD_MS is 7500 rather than the transport's 9000
// precisely so 7500 + the 4000 floor stays inside 12000.
//
// WHY 4000 AND NOT 8000. The class this credit exists for is the 2026-08-18
// whole-machine stall: 6.26s worst cluster, ~7.3s of viewer tick gap. At the
// SHIPPED 12000 budget that class does not kill a viewer even with ZERO credit
// (H1 restored the budget; the capture died because the harness had pinned
// 5000). So the credit's field benefit begins only for freezes LONGER than the
// budget, and 4000 buys coverage out to 16s -- 2.2x the worst freeze ever
// measured here -- for half the give-up slip. Both red/green arms still work at
// 4000: the 7s arm at a truncated 5000 budget lands at 3015ms residual and
// survives, the 12s arm lands at ~8016ms residual, survives, and still runs the
// post-stall rung.
static constexpr uint32_t kSelfStallCreditCapMs = 4000;
// The outermost declared-dead latency any spectator path may reach, credit
// included. See the derivation above.
static constexpr uint32_t kDeclaredDeadCeilingMs = 20000;

void SpecWatchdogTick(bool offline_replay) {
    const bool s_offline_replay_env_active = offline_replay;
    // ---- TEST LEVER: FM2K_SPEC_TEST_SELFSTALL_MS (dark by default) ---------
    // The red/green for the self-stall credit. Freezes this viewer's ENTIRE
    // network-facing surface once, mid-stream, for the requested number of ms
    // (see ControlChannel_TestFreezePoll for why a plain Sleep would not do).
    // Without the credit and at a truncated FM2K_SPEC_HOST_GONE_MS the viewer
    // then exits HOST UNREACHABLE on the very next tick, reporting 0 recovery
    // rungs -- the 2026-08-18 capture exactly. With it, the viewer logs
    // [SPEC-SELFSTALL] and keeps watching. Fires ONCE, only after playback is
    // genuinely established, and only for a spectator.
    if (!s_offline_replay_env_active) {
        static int s_test_stall_ms = -1;
        if (s_test_stall_ms < 0) {
            const char* v = std::getenv("FM2K_SPEC_TEST_SELFSTALL_MS");
            s_test_stall_ms = (v && v[0]) ? std::atoi(v) : 0;
            if (s_test_stall_ms < 0) s_test_stall_ms = 0;
        }
        static bool s_test_stall_done = false;
        if (s_test_stall_ms > 0 && !s_test_stall_done &&
            SpectatorNode_HasEverAdmitted() &&
            SpectatorNode_MsSinceLastAdmit() < 500) {
            static uint64_t s_first_admit_seen_ms = 0;
            const uint64_t tnow = GetTickCount64();
            if (s_first_admit_seen_ms == 0) s_first_admit_seen_ms = tnow;
            // 15s of healthy playback first: past the delay-bank pre-arm window
            // and well inside any recipe, so the freeze lands on a viewer that
            // is demonstrably live rather than one still joining.
            if (tnow - s_first_admit_seen_ms > 15000) {
                s_test_stall_done = true;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[SPEC-SELFSTALL-TEST] FM2K_SPEC_TEST_SELFSTALL_MS=%d -- "
                    "freezing this viewer's whole network surface now",
                    s_test_stall_ms);
                ControlChannel_TestFreezePoll((uint32_t)s_test_stall_ms);
            }
        }
    }
    // ---- No-forward-progress watchdog (host gone OR wedged) ----------------
    // A live spectator that stops admitting NEW input frames is watching a host
    // that either died (TerminateProcess/crash -- no graceful SESSION_END) or
    // WEDGED. The wedge that motivated this: under loss the host can get stuck
    // at the battle->CSS swap barrier, re-sending control-channel BATTLE_END
    // forever (keeping pb_queue non-empty with stale resends) while producing no
    // admittable content -- so the OLD queue-empty watchdog never tripped (q
    // bounced 0<->8). MsSinceLastAdmit() is transport-agnostic (stamped on every
    // UDP and TCP admit) and stays ~0 during normal battle AND CSS play, so a
    // multi-second stall is unambiguous. Returns 0 before the first admit (no
    // false-fire during connect/snapshot) and is skipped for offline replay
    // (which has its own drain-exit below). Tunable via FM2K_SPEC_HOST_GONE_MS.
    if (!s_offline_replay_env_active) {
        // 12000, RAISED FROM 8000. This budget is the LAST rung of the
        // spectator recovery ladder and it must sit above the whole thing, or
        // the viewer kills itself while its own repairs are still running --
        // which is exactly what the 2026-08-07 mid-stream starve was. At 8000
        // the RC ordered-stall repair also fired at 8000: same tick, coin
        // flip, and it had in fact never executed once, anywhere. The ladder,
        // all measured on THIS clock (ms since the last admitted INPUT):
        //   3500  RC ordered-stall repair    (RC_ORDERED_STALL_SEC)
        //   4000  starve escalation #1       (re-JOIN + RC endpoint reset)
        //   8000  starve escalation #2       (SPECTATOR_STARVE_ESCALATE_FLOOR_MS)
        //  12000  give up (here)             -- two full escalations deep
        // A genuinely-crashed host therefore costs 4s more before the window
        // closes; a wedged-but-reachable one now gets two chances at repair
        // instead of zero. S3 (2026-08-16) adds a second layout: under active
        // RC repair #1 defers to 7500 and #2 lands at 11500, still two rungs
        // inside this budget -- a cap derived from this number and
        // static_asserted in spectator_node_internal.h.
        static const uint32_t s_gone_ms = []{
            const char* v = std::getenv("FM2K_SPEC_HOST_GONE_MS");
            const uint32_t ms = (v && v[0]) ? (uint32_t)std::atoi(v) : SPECTATOR_HOST_GONE_DEFAULT_MS;
            // An override BELOW the ladder truncates it. That is legitimate
            // (the harness deliberately shortens teardown, and
            // spec_host_gone_test.sh needs to beat the harness cleanup), but
            // it must never again be a silent reason the repair "never fires":
            // say so once, at startup, so a truncated run is self-diagnosing.
            if (ms < SPECTATOR_HOST_GONE_DEFAULT_MS) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: FM2K_SPEC_HOST_GONE_MS=%u is BELOW the "
                    "recovery ladder's %ums budget (RC stall repair 3500, "
                    "escalation #1 4000 or up to 7500 under active RC repair, "
                    "escalation #2 a 4000ms floor behind it) -- this viewer "
                    "will exit before the later rungs can run",
                    ms, (unsigned)SPECTATOR_HOST_GONE_DEFAULT_MS);
            }
            return ms;
        }();
        // GRACEFUL stream end: the upstream sent SPEC_SESSION_END (the host
        // quit cleanly). Drain whatever is still queued, then close PROMPTLY
        // with a clear "stream ended" message instead of waiting out the
        // full host-gone window below. Only after we were actually live.
        if (SpectatorNode_SessionEnded() &&
            SpectatorNode_HasEverAdmitted() &&
            SpectatorNode_PendingFrameCount() == 0) {
            static std::atomic<bool> s_ended_armed{false};
            if (!s_ended_armed.exchange(true)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: stream ended -- host left the session "
                    "cleanly; closing");
                ExitProcess(0);
            }
        }
        // SESSION ENDED BEFORE PLAYBACK EVER STARTED. The queue-EMPTY,
        // never-admitted sibling of the graceful drain above: session_ended is
        // latched but not one frame was ever admitted, so there is nothing to
        // drain and nothing will ever arrive -- the host is shutting down.
        // Made materially more reachable by the H6 change in
        // SpectatorNode_HandleSessionEnd (spec_join.cpp), which now accepts
        // SESSION_END during a re-JOIN window (subscribed_upstream is cleared
        // by RequestJoin, so a shutting-down host's broadcast used to be
        // dropped). Without this branch such a viewer fell through to the 30s
        // CONNECT-ESTABLISHMENT DEADLINE below and exited with
        // "connect never established ... (join handshake failed / host
        // unreachable)", which is both a 30s stare at a dead window and a
        // wrong diagnosis: the handshake was fine, the host simply left first.
        //
        // Short grace rather than an instant exit: OP_BASELINE / backfill
        // datagrams already on the wire when the host shut down can still land
        // and admit, at which point HasEverAdmitted() flips, this branch stops
        // matching, and the normal graceful-drain path above (plus the 20s
        // drain budget below) takes over -- the viewer gets to watch what it
        // did receive instead of being killed at the door. 2s is well past a
        // loopback/LAN in-flight window and far short of the 30s stare.
        constexpr uint32_t kEndBeforePlaybackGraceMs = 2000;
        static uint64_t s_end_noplay_since = 0;
        if (!SpectatorNode_SessionEnded() || SpectatorNode_HasEverAdmitted()) {
            // Not (or no longer) in this state: drop the stamp so a later
            // latch always gets a fresh grace window. session_ended is
            // clearable by a fresh RequestJoin (spec_join.cpp), and
            // HasEverAdmitted flipping true is the normal way out of here.
            s_end_noplay_since = 0;
        } else {
            const uint64_t end_now = GetTickCount64();
            if (s_end_noplay_since == 0) s_end_noplay_since = end_now;
            if (end_now - s_end_noplay_since > kEndBeforePlaybackGraceMs) {
                static std::atomic<bool> s_end_noplay_armed{false};
                if (!s_end_noplay_armed.exchange(true)) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: host ended the session before playback "
                        "started -- SPEC_SESSION_END arrived with zero frames "
                        "ever admitted and nothing left in flight after %ums; "
                        "closing (the join handshake did NOT fail)",
                        kEndBeforePlaybackGraceMs);
                    ExitProcess(0);
                }
            }
        }
        // ---- [SPEC-SELFSTALL] credit the tick gap (P1) ---------------------
        //
        // SpectatorNode_MsSinceLastAdmit() is a plain now - last_admit_ms. It
        // cannot tell "the upstream sent nothing" from "I was not scheduled".
        // On 2026-08-18 a ~7.3s whole-machine stall froze all four processes of
        // a soak run at once; both viewers woke up, read the ENTIRE elapsed
        // silence as content-absence, and killed a stream that was healthy --
        // one of them was at the live edge and the other still held 563
        // buffered events (5.6s of content) and had issued zero gap-fill pulls.
        // Silence this viewer did not observe (because it was not running) is
        // not evidence about the host, so credit it back.
        //
        // Why this and not "raise the budget": raising 12000 to 20000 makes a
        // really-dead host take 20s to notice. Crediting the tick gap costs
        // nothing when the viewer runs normally and is exactly right when it
        // does not.
        //
        // BOUNDED, and the bound is the safety argument -- see
        // kSelfStallCreditCapMs. A viewer whose own tick loop is broken burns
        // the cap once and then behaves exactly as it does today.
        //
        // Side effect, deliberate and good: SpectatorNode_CreditSelfStall moves
        // the same clock the adaptive delay bank measures its inter-admission
        // gaps on, so a self-stall no longer inflates the bank (a 6.3s freeze
        // used to ask for a 780-frame bank on a link that never hiccuped).
        //
        // THE CAP IS SOLVED AGAINST THE BUDGET IN FORCE ON THIS TICK, not
        // against a constant. See kSelfStallCreditCapMs for the arithmetic; the
        // reason it has to be computed here is the graceful-drain branch below,
        // which raises the give-up budget to 20000 on its own. Stacking a credit
        // on top of that budget is how the worst case reached 28s. On the
        // drain path the effective cap is therefore ZERO -- which costs nothing
        // real, because a viewer draining a bank after SESSION_END is not
        // waiting on a host at all.
        const bool draining_after_end = SpectatorNode_SessionEnded() &&
                                        SpectatorNode_PendingFrameCount() > 0;
        const uint32_t gone_budget =
            draining_after_end ? ((s_gone_ms > 20000u) ? s_gone_ms : 20000u)
                               : s_gone_ms;
        const uint32_t credit_cap_ms =
            (gone_budget >= kDeclaredDeadCeilingMs)
                ? 0u
                : ((kDeclaredDeadCeilingMs - gone_budget < kSelfStallCreditCapMs)
                       ? (kDeclaredDeadCeilingMs - gone_budget)
                       : kSelfStallCreditCapMs);
        static uint64_t s_last_tick_ms       = 0;
        static uint32_t s_selfstall_total_ms = 0;
        static uint32_t s_giveup_hold_ticks  = 0;
        const uint64_t tick_now = GetTickCount64();
        const uint32_t tick_gap =
            (s_last_tick_ms == 0) ? 0u : (uint32_t)(tick_now - s_last_tick_ms);
        s_last_tick_ms = tick_now;
        // THE CAP GOVERNS BOTH MECHANISMS, not just the credit. A viewer whose
        // own tick loop is broken stalls on EVERY tick; if the give-up hold
        // below were unbounded it would keep deferring give-up forever even
        // after the credit ran out, and a viewer that never gives up on a dead
        // host is a worse bug than the one this fixes. Once the session has
        // spent its cap, a long tick gap stops being treated as a self-stall at
        // all and the watchdog behaves exactly as it did before this change.
        //
        // BOTH CLOCKS MUST BE QUIET, and that second condition was found by
        // measurement rather than reasoning. A long tick gap ALONE does not
        // mean "I was descheduled": the catch-up turbo sims thousands of queued
        // frames inside ONE outer tick, and a deep-joining viewer draining its
        // backlog produced 1391-1406ms tick gaps on a completely healthy run
        // (2026-08-18, vanpri 16000f). The admits kept flowing throughout, so
        // MsSinceLastAdmit stayed at 0 and there was nothing to credit -- but
        // the ladder-hold and the log line fired anyway, and the harness term
        // that reads those lines would eventually have redded a green run on
        // one. Requiring the ADMIT clock to be quiet too separates "the process
        // stopped" from "the process was busy WHILE ADMITS FLOW".
        //
        // IT DOES NOT SEPARATE "busy while the host is DEAD" -- corrected here
        // rather than left as the overclaim it was (review H2, 2026-08-18). A
        // dead host silences the admit clock, and a viewer draining its bank
        // through the emergency catch-up turbo produces the long tick gap, so
        // that one case is still credited when it should not be. The exact
        // discriminator is "did this tick simulate any frames", and it is NOT
        // implemented, deliberately: the only lever that can produce a freeze
        // (ControlChannel_TestFreezePoll) fires at the TOP of this function,
        // before the tick's playout loop, so it would make the discriminator
        // read "not busy" no matter whether the rule were right -- proving a
        // detector on the one corpus that suits it, which this campaign has
        // already been caught doing twice. What bounds the case instead is
        // arithmetic: kSelfStallCreditCapMs is solved against
        // kDeclaredDeadCeilingMs, so the worst a wrongly-credited session can
        // cost is a 16000ms declared-dead latency on the default budget.
        //
        // FM2K_SPEC_SELFSTALL=0 -- A/B SCAFFOLDING, delete with the switch.
        // The ONLY way to take a same-binary red arm for this fix: with the
        // credit and the ladder-hold disarmed, a viewer frozen past a truncated
        // host-gone budget exits HOST UNREACHABLE reporting zero recovery rungs,
        // which is the 2026-08-18 capture. Default ARMED.
        static int s_selfstall_armed = -1;
        if (s_selfstall_armed < 0) {
            const char* v = std::getenv("FM2K_SPEC_SELFSTALL");
            s_selfstall_armed = (v && v[0] == '0' && v[1] == '\0') ? 0 : 1;
        }
        const uint32_t raw = SpectatorNode_MsSinceLastAdmit();
        bool selfstall_this_tick = false;
        if (s_selfstall_armed && tick_gap > kSelfStallMs && raw > kSelfStallMs &&
            SpectatorNode_HasEverAdmitted() &&
            s_selfstall_total_ms < credit_cap_ms) {
            selfstall_this_tick = true;
            uint32_t credit = tick_gap;
            // Never credit more silence than actually exists (a tick gap can
            // straddle an admit that happened on the network thread).
            if (credit > raw) credit = raw;
            const uint32_t room = (s_selfstall_total_ms >= credit_cap_ms)
                                      ? 0u
                                      : (credit_cap_ms - s_selfstall_total_ms);
            if (credit > room) credit = room;
            if (credit > 0) {
                SpectatorNode_CreditSelfStall(credit);
                s_selfstall_total_ms += credit;
            }
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-SELFSTALL] this viewer did not run for %ums -- crediting "
                "%ums back to the host-gone budget (session total %u/%ums, "
                "stall clock was %ums, now %ums; worst-case declared-dead "
                "latency this session %ums of the %ums ceiling)",
                tick_gap, credit, s_selfstall_total_ms,
                (unsigned)credit_cap_ms, raw,
                SpectatorNode_MsSinceLastAdmit(),
                gone_budget + credit_cap_ms,
                (unsigned)kDeclaredDeadCeilingMs);
        }
        // ---- [SPEC-SELFSTALL] post-stall rung catch-up (P1b) ---------------
        //
        // The recovery ladder is level-triggered on this same clock and
        // evaluated only on ticks, so a viewer that hitches past 3500/4000/8000
        // fires NONE of them: both viewers in the capture above reported
        // "0 re-JOIN escalation(s)" having crossed 4000ms during the freeze. A
        // ladder that only exists on ticks does not exist after a hitch.
        //
        // The rung decision is taken on the POST-CREDIT clock, on purpose. The
        // credit is what separates "the host went quiet" from "I went away";
        // acting on silence this viewer merely slept through would repair a
        // problem that was never on the wire. What survives the credit is real
        // host-side silence -- exactly what the ladder is for -- and it is also
        // what a viewer that has burned its credit cap sees.
        //
        // THE RUNG IS THE PULL, NEVER THE RE-JOIN (review R1, ruling adopted
        // 2026-08-18; the escalation branch is DELETED, not flagged). A viewer
        // that just came back from a freeze has ZERO pull evidence, which is the
        // exact state the ordinary ladder is forbidden to escalate from. Full
        // argument and its measured damage on SpecRunPostStallRung itself.
        if (selfstall_this_tick) {
            SpecRunPostStallRung(tick_now, SpectatorNode_MsSinceLastAdmit());
            // ...and give-up may not arm on this tick or the next: the rung we
            // just ran needs one full tick to be answered before its failure
            // can be held against the host.
            s_giveup_hold_ticks = 1;
        } else if (s_giveup_hold_ticks > 0) {
            --s_giveup_hold_ticks;
        }
        const bool giveup_held = selfstall_this_tick || s_giveup_hold_ticks > 0;
        const uint32_t since_admit = SpectatorNode_MsSinceLastAdmit();
        // GRACEFUL-END DRAIN GRACE. After SESSION_END there is by definition
        // nothing left to admit, so since_admit climbs on a perfectly healthy
        // viewer that is simply still playing out its delay bank. At the plain
        // 8s budget that KILLED viewers mid-drain -- a bank grown to 1000+
        // frames under loss needs well over 10s at 1x -- which is the sweep's
        // "ended N frames behind the host" LAG fingerprint. Extend the budget
        // only while the stream ended cleanly AND we still hold frames; the
        // graceful exit above fires the instant the queue empties, so a healthy
        // viewer closes at exactly the same moment it does today (sooner, with
        // the end-of-stream drain below). An explicitly larger
        // FM2K_SPEC_HOST_GONE_MS still wins.
        // (draining_after_end / gone_budget are computed ABOVE the self-stall
        // credit -- the credit's cap is solved against this same budget.)
        if (since_admit > gone_budget && !giveup_held) {
            static std::atomic<bool> s_gone_armed{false};
            if (!s_gone_armed.exchange(true)) {
                // No SESSION_END arrived, we simply stopped being fed. REPORT
                // WHAT IS KNOWN, not a guess. The old message said "host
                // crashed/left ungracefully" unconditionally, and in the
                // 2026-08-07 mid-stream starve that was flatly false: the
                // control channel was answering in both directions the whole
                // time (the viewer's gap-fill JOIN_REQs reached the host on a
                // metronomic 500ms cadence and the host's JOIN_ACKs came
                // back) -- only the reliable stream had wedged. A tester who
                // files "host crashed" about a host that did not crash is an
                // unattributable report, which is precisely what the last
                // packet's age can settle. Two distinct verdicts, plus what
                // the recovery ladder actually tried.
                const uint32_t pkt_age  = SpectatorNode_MsSinceUpstreamPacket();
                const uint32_t pulls    = SpectatorNode_GapFillPullCount();
                const uint32_t escs     = SpectatorNode_StarveEscalationCount();
                const uint32_t defers   = SpectatorNode_StarveDeferralCount();
                const bool     reachable = pkt_age != 0 && pkt_age < since_admit;
                if (reachable) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: STREAM STALLED -- no new content for %ums, "
                        "but the host is still REACHABLE (last packet from it %ums "
                        "ago), so this is a wedged stream, NOT a crashed host. "
                        "Recovery tried: %u gap-fill pull(s), %u re-JOIN "
                        "escalation(s) (%u deferred while RC reported active "
                        "repair), and the RC ordered-stall repair; none restored "
                        "the feed. Closing stream",
                        since_admit, pkt_age, pulls, escs, defers);
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: HOST UNREACHABLE -- no new content for "
                        "%ums and nothing at all from the upstream %s. It left "
                        "ungracefully (crash / network drop / TerminateProcess: "
                        "no SPEC_SESSION_END). Recovery tried: %u gap-fill "
                        "pull(s), %u re-JOIN escalation(s) (%u deferred). Closing",
                        since_admit,
                        pkt_age == 0 ? "since the session began"
                                     : "for at least as long",
                        pulls, escs, defers);
                }
                ExitProcess(0);
            }
        }
        // CONNECT-ESTABLISHMENT DEADLINE. MsSinceLastAdmit is 0 until the FIRST
        // admit (so the host-gone watchdog above can't false-fire during a normal
        // connect/snapshot), which means a spectator whose JOIN handshake never
        // completes under loss hung on "Connecting..." FOREVER (had to be closed
        // by hand). If we've NEVER admitted a frame after a generous window, the
        // connect failed -- exit cleanly instead of hanging. Tunable via
        // FM2K_SPEC_CONNECT_TIMEOUT_MS (default 30s: covers a slow boot-walk +
        // retried join under heavy loss with wide margin).
        static const uint32_t s_connect_ms = []{
            const char* v = std::getenv("FM2K_SPEC_CONNECT_TIMEOUT_MS");
            return (v && v[0]) ? (uint32_t)std::atoi(v) : 30000u;
        }();
        static uint64_t s_spectate_start_ms = 0;
        if (s_spectate_start_ms == 0) s_spectate_start_ms = GetTickCount64();
        // task #58: at 4s with ZERO admits, engage the hub spec-relay
        // fallback (one-shot, no-op when the launcher supplied no relay
        // block) -- covers viewers whose direct UDP path is dead (hard
        // NAT) well before the 30s give-up below.
        if (!SpectatorNode_HasEverAdmitted() &&
            GetTickCount64() - s_spectate_start_ms > 4000) {
            SpectatorNode_EngageRelayFallback();
        }
        if (!SpectatorNode_HasEverAdmitted() &&
            GetTickCount64() - s_spectate_start_ms > s_connect_ms) {
            static std::atomic<bool> s_connect_armed{false};
            if (!s_connect_armed.exchange(true)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: connect never established after %ums "
                    "(join handshake failed / host unreachable) -- exiting process",
                    s_connect_ms);
                ExitProcess(0);
            }
        }
    }
}
