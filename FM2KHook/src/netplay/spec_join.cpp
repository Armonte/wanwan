// Spectator join protocol, HOST side: JOIN_REQ accept/redirect, the
// GekkoSpectator dedup set, SESSION_END broadcast, and JOIN_ACK construction.
// The viewer half (request/ack/redirect/kick) lives in the sibling
// spec_join_viewer.cpp; they share spec_join_internal.h. Public API decls in
// spectator_node.h; reaches specnode helpers via using.
#include "spectator_node.h"
#include "spectator_node_internal.h"  // shared State model + g_state (split for sibling TUs)
#include "spec_join_internal.h"       // shared with spec_join_viewer.cpp
#include "control_channel.h"
#include "netplay.h"
#include "netplay_state.h"
#include "../core/globals.h"          // FM2K::ADDR_* -- CSS char/stage reads for the JOIN_ACK
#include "../audio/sound_rollback.h"  // Op apply: SOUND_INIT
#include "../hooks/per_game_patches.h" // PerGamePatches_SetRuntimeBtbOverrides
#include "../ui/shared_mem.h"         // C10: SharedMem_PublishMatchSession / RoundResult
#include "version_local.h"            // fm2k::kAppVersion -- spectate version gate
#include "gekkonet.h"

// kAppVersion "0.M.P" -> (M, P) for the SPEC_JOIN_VERSIONED gate. Parsed
// once; a malformed string (never happens -- make_version.sh stamps it)
// degrades to 0.0, which simply fails the gate closed. Declared in
// spec_join_internal.h because the viewer TU stamps it into every JOIN_REQ.
void SpecJoin_AppVersionBytes(uint8_t* out_minor, uint8_t* out_patch) {
    static uint8_t s_minor = 0xFF, s_patch = 0xFF;
    if (s_minor == 0xFF) {
        unsigned mj = 0, mn = 0, pa = 0;
        if (std::sscanf(fm2k::kAppVersion, "%u.%u.%u", &mj, &mn, &pa) == 3) {
            s_minor = (uint8_t)mn;
            s_patch = (uint8_t)pa;
        } else {
            s_minor = 0;
            s_patch = 0;
        }
    }
    *out_minor = s_minor;
    *out_patch = s_patch;
}

#include <SDL3/SDL_log.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <array>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>
using namespace specnode;

// Per-session tracking of which GekkoSpectator addrs we've already added.
// GekkoNet has no remove-actor API (gekkonet.h:185-203 -- only
// gekko_add_actor exists), so a naive add-on-rejoin pattern leaks one
// GekkoSpectator actor per spec retry. Over a 5s-retry storm, the host's
// per-tick spectator iteration cost grows linearly and crushes the frame
// budget down to single-digit FPS.
//
// Fix: gate gekko_add_actor on this set. Cleared once per session boundary
// by SpectatorNode_ClearGekkoSpectatorTracking() (called from netplay.cpp
// after each fresh gekko_create + gekko_start -- new session = no actors
// yet = empty set). Worst case per session: one zombie actor per
// ever-seen spec addr, instead of one per retry.
//
// Keyed by "ip:port" string (matches the addr_str gekko_add_actor sees).
// std::set instead of unordered_set so we don't have to hash, and the
// member count is tiny (single-digit per match in practice).
std::set<std::string> g_gekko_spectator_addrs;

// Clear the GekkoSpectator addr-tracking set. Called from netplay.cpp
// after each fresh gekko_create + gekko_start so the next session
// starts with no "already added" entries. Without this, post-session
// spec rejoins would be skipped because their addr is "remembered"
// from the previous (now-destroyed) session.
void SpectatorNode_ClearGekkoSpectatorTracking() {
    if (!g_gekko_spectator_addrs.empty()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: cleared %zu GekkoSpectator addr tracking entries (session boundary)",
            g_gekko_spectator_addrs.size());
        g_gekko_spectator_addrs.clear();
    }
}

// Core ACK builder. `advertised_kind` is what goes on the wire; `with_chars`
// asks for the live battle char/stage/color read. They are separate arguments
// on purpose: the battle-entry refresh advertises kind 3 (not 2) but still
// wants the chars, and a CSS/NONE grant must carry
// NO chars at all -- a /F-boot seed handed to a from-frame-0 mirror is exactly
// the bug this split of the builder exists to prevent.
static CtrlPacket BuildJoinAckWithKind(uint8_t advertised_kind, bool with_chars) {
    CtrlPacket ack = {};
    ack.header.type = CtrlMsg::SPEC_JOIN_ACK;
    ack.data.spec_join_ack.host_session_kind = advertised_kind;
    // Default "unknown" -- only valid when host is in battle.
    ack.data.spec_join_ack.host_p1_char = 0xFF;
    ack.data.spec_join_ack.host_p2_char = 0xFF;
    ack.data.spec_join_ack.host_stage   = 0xFF;
    if (with_chars) {
        // Read the engine's current post-CSS-confirm chars + stage so
        // the spec can /F-boot with the RIGHT character files. These
        // live at ADDR_P1_SELECTED_CHAR / ADDR_P2_SELECTED_CHAR (the
        // same addresses Netplay_StartBattle reads for its "match
        // chars p1=N(...) p2=N(...)" log) and ADDR_SELECTED_STAGE.
        //
        // Note: g_config_value1/3 (0x4300E0/0x4300F0) are only
        // populated when the HOST itself was /F-launched -- for a
        // normal CSS walk they stay at 0, which is why the previous
        // read gave us p1=0/p2=0 and pkmncc crashed loading a
        // mirror Blaziken matchup.
        const uint32_t p1 = *(const uint32_t*)FM2K::ADDR_P1_SELECTED_CHAR;
        const uint32_t p2 = *(const uint32_t*)FM2K::ADDR_P2_SELECTED_CHAR;
        const uint32_t st = (FM2K::ADDR_SELECTED_STAGE != 0)
                              ? *(const uint32_t*)FM2K::ADDR_SELECTED_STAGE
                              : 0u;
        if (p1 < 50u) ack.data.spec_join_ack.host_p1_char = (uint8_t)p1;
        if (p2 < 50u) ack.data.spec_join_ack.host_p2_char = (uint8_t)p2;
        if (st < 50u) ack.data.spec_join_ack.host_stage   = (uint8_t)st;
        // Per-slot confirm colors (slot+0xE00B, set by AssignPlayerColor
        // from the confirm button at CSS -- the engine fact that button
        // choice IS the color). The /F boot path on the viewer hardcodes
        // P1=0/P2=1; these let it stamp the real palettes instead.
        ack.data.spec_join_ack.host_p1_color = 0xFF;
        ack.data.spec_join_ack.host_p2_color = 0xFF;
        const int32_t c1 = *(const int32_t*)0x4DFD8Bu;
        const int32_t c2 = *(const int32_t*)(0x4DFD8Bu + 0xE03Fu);
        if (c1 >= 0 && c1 < 8) ack.data.spec_join_ack.host_p1_color = (uint8_t)c1;
        if (c2 >= 0 && c2 < 8) ack.data.spec_join_ack.host_p2_color = (uint8_t)c2;
    }
    return ack;
}

// LIVE REFRESH. Carries where the host is RIGHT NOW, tagged so no viewer can
// mistake it for its grant. Used by the battle-entry re-broadcast in
// spectator_node.cpp (one packet to every subscriber, so it cannot be
// per-subscriber) and by the re-ACKs that deliberately change nothing --
// those go to demonstrably-live mirroring viewers, for which the host's
// current phase is the useful truth. Chars ride along when in battle: handing
// already-mirroring viewers the now-real chars/stage is the point of it.
CtrlPacket BuildJoinAckPacket() {
    const NetplaySessionKind k = Netplay_GetSessionKind();
    const bool battle = (k == NetplaySessionKind::BATTLE);
    return BuildJoinAckWithKind(
        static_cast<uint8_t>(static_cast<uint8_t>(k) | SPEC_ACK_LIVE_REFRESH),
        battle);
}

// Per-subscriber grant. Every ACK this host sends to a specific subscriber
// goes through here, so the kind a viewer is told is ALWAYS the kind that was
// pinned for it -- decided once, from one Netplay_GetSessionKind() read, and
// re-decided only when the host deliberately re-pins (the destructive-reset
// branch, which updates the kind and the deep-join grant in one block).
CtrlPacket BuildJoinAckPacketFor(const Subscriber& sub) {
    // No SPEC_ACK_LIVE_REFRESH bit: this IS the grant. Chars only for a BATTLE
    // grant -- handing /F-boot seeds to a viewer that is going to natural-boot
    // and mirror char-select is the defect.
    //
    // SPEC_ACK_DEEP_JOIN rides ONLY here, never on BuildJoinAckPacket's live
    // refresh, so a viewer's battle-entry hold is a function of its own grant
    // and nothing the host broadcasts can arm one on a viewer that is bit-exact
    // by simulation. pinned_ack_kind itself is left PURE (kind only) so the
    // three `== SPEC_ACK_KIND_BATTLE` tests in the bind path keep working
    // unmasked; the flag lives on its own Subscriber field.
    uint8_t advertised = sub.pinned_ack_kind;
    if (sub.deep_join_eligible) advertised |= SPEC_ACK_DEEP_JOIN;
    return BuildJoinAckWithKind(advertised,
                                sub.pinned_ack_kind == SPEC_ACK_KIND_BATTLE);
}

void SpectatorNode_HandleJoinReq(const sockaddr_in& from,
                                 uint8_t caps, uint32_t resume_frame,
                                 uint8_t ver_minor, uint8_t ver_patch) {
    // Version gate. Savestate blobs and the sim are version-specific; a
    // cross-version spectator silently black-screens (live report
    // 2026-07-17), and pre-gate viewers can't parse EVENT_BATCH2 anyway.
    // Reject unversioned (old build) and mismatched joiners with the
    // null-redirect the capacity path uses -- the viewer gives up cleanly.
    {
        uint8_t my_minor = 0, my_patch = 0;
        SpecJoin_AppVersionBytes(&my_minor, &my_patch);
        const bool versioned = (caps & SPEC_JOIN_VERSIONED) != 0;
        if (!versioned || ver_minor != my_minor || ver_patch != my_patch) {
            char addr_buf[48] = {};
            FormatAddr(from, addr_buf, sizeof(addr_buf));
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: version gate REJECTED JOIN_REQ from %s -- "
                "viewer %s0.%u.%u vs host 0.%u.%u (spectate needs the same "
                "build; viewer should update)",
                addr_buf, versioned ? "" : "unversioned/",
                ver_minor, ver_patch, my_minor, my_patch);
            CtrlPacket redir = {};
            redir.header.type = CtrlMsg::SPEC_JOIN_REDIRECT;
            redir.data.spec_redirect.redirect_ip   = 0;
            redir.data.spec_redirect.redirect_port = 0;
            ControlChannel_SendTo(redir, from);
            return;
        }
    }
    if ((caps & SPEC_JOIN_RESUME) == 0) resume_frame = 0;
    // ONE read of the host's session kind, reused for BOTH halves of the
    // decision -- the grant advertised to the viewer and the payload the bind
    // will ship. That property is the whole point of taking it here: an
    // earlier version read the kind again inside the ACK builder and a THIRD
    // time on the battle-entry re-broadcast, so a host crossing into battle
    // between two of them handed a viewer a grant its stream did not match.
    // pinned_ack_kind rides on the Subscriber so every later re-ACK repeats
    // the same answer.
    //
    // There is one join flavour: "the match happening right now". The viewer
    // does not choose. What varies is only how the host can serve it from
    // where it currently is:
    //
    //   host in battle          -> SPEC_ACK_KIND_BATTLE, i.e. "/F-boot into
    //                              the match, a snapshot is coming"
    //   host between matches    -> a CSS/NONE grant plus deep-join eligibility
    //                              (bounded backfill from the current char
    //                              select + a mandatory battle-entry blob)
    //   host with no prior match -> a CSS/NONE grant and the whole session so
    //                              far, which at that point is a title walk
    //                              and one char select. Not a "full session
    //                              replay" flavour -- the degenerate case of
    //                              the bounded one, where the bound is the
    //                              start of the session.
    const NetplaySessionKind kind_at_pin = Netplay_GetSessionKind();
    const uint8_t pinned_kind =
        (kind_at_pin == NetplaySessionKind::BATTLE)
            ? SPEC_ACK_KIND_BATTLE
            : static_cast<uint8_t>(kind_at_pin);
    // The bounded deep-join decision, taken HERE, in the same block and from
    // the same single read. The bind used to re-derive it a few ticks later
    // against moved state, so the grant a viewer booted on and the payload it
    // was actually shipped could disagree; keying the bind on this field
    // removes that class entirely (same lesson as pinned_ack_kind).
    //
    // Requires a NON-battle grant (a BATTLE grant already means "a snapshot is
    // coming" through the ordinary bind path) and an actual CSS anchor --
    // HaveBoundedAnchor() is false until a match has ENDED, because before
    // that there is no prior match to skip and anchoring on the first
    // CSS_ENTERED would drop the ops that precede it (the handshake PIN_RNG
    // and friends). Such a viewer is served from the start of the session
    // instead, and can never be told to hold.
    const bool deep_join_eligible =
        pinned_kind != SPEC_ACK_KIND_BATTLE && HaveBoundedAnchor();
    char addr_buf[48] = {};
    FormatAddr(from, addr_buf, sizeof(addr_buf));

    // Helper: if there's a live GekkoNet session on this node (player slot),
    // add the joining spectator as a GekkoSpectator actor so confirmed-input
    // events and (battle) Save/Load events reach them natively. Both CSS
    // and BATTLE sessions get spectators added -- CSS doesn't emit Save/Load
    // (lockstep suppresses them) but it does emit GekkoAdvanceEvent per
    // confirmed frame, and that's the source of truth that drives the
    // spectator's local sim 1:1 with the host.
    auto AddSpectatorToSession = [](const sockaddr_in& spec_from) {
        const NetplaySessionKind k = Netplay_GetSessionKind();
        if (k != NetplaySessionKind::CSS && k != NetplaySessionKind::BATTLE) {
            return;
        }
        char ip_str[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, (void*)&spec_from.sin_addr, ip_str, sizeof(ip_str));
        char addr_str[64];
        snprintf(addr_str, sizeof(addr_str), "%s:%u",
                 ip_str, ntohs(spec_from.sin_port));
        GekkoSession* sess = Netplay_GetActiveSession();
        if (!sess) return;

        // Dedup against this-session's previously-added spec addrs. Without
        // this, a spec stuck in a 5s retry loop (e.g. TCP punch failing on
        // symmetric NAT) re-fires SPEC_JOIN_REQ every cycle and we'd
        // gekko_add_actor on each, leaking one actor per retry. GekkoNet
        // has no remove_actor counterpart, so dedup-on-add is the only
        // bound. Set is cleared at session boundaries from netplay.cpp.
        const std::string addr_key(addr_str);
        if (g_gekko_spectator_addrs.count(addr_key)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: GekkoSpectator already on %s session for %s -- skipping re-add",
                k == NetplaySessionKind::CSS ? "CSS" : "BATTLE",
                addr_str);
            return;
        }
        // Hard bound on native GekkoSpectator actors per session. GekkoNet has
        // no remove_actor, so a NAT-remapped reconnect (new ip:port each time)
        // would otherwise add a PERMANENT actor per churn cycle -> O(N)/tick
        // event fan-out that crushes the host frame budget to single-digit FPS.
        // Well above any real spectator count; a joiner past the cap still gets
        // the full stream over the SpectatorNode TCP/RC path (this native actor
        // is a secondary confirmed-input delivery, not the primary transport).
        constexpr size_t kMaxGekkoSpectators = 64;
        if (g_gekko_spectator_addrs.size() >= kMaxGekkoSpectators) {
            static uint32_t s_cap_log = 0;
            if ((s_cap_log++ & 0x1F) == 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: GekkoSpectator actor cap %zu reached (%zu addrs this "
                    "session) -- %s served via SpectatorNode stream only (no native actor)",
                    kMaxGekkoSpectators, g_gekko_spectator_addrs.size(), addr_str);
            }
            return;
        }
        GekkoNetAddress addr = {};
        addr.data = (void*)addr_str;
        addr.size = (int)strlen(addr_str);
        gekko_add_actor(sess, GekkoSpectator, &addr);
        g_gekko_spectator_addrs.insert(addr_key);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: Late-joiner added as GekkoSpectator on %s session -> %s",
            k == NetplaySessionKind::CSS ? "CSS" : "BATTLE",
            addr_str);
    };

    // Already subscribed? Reset the slot's TCP-bound state so the new
    // JOIN_REQ re-fires the bind + backfill path. Without this, a previous
    // spectator session whose TCP read-errored leaves the slot with
    // bound=true; the next JOIN_REQ from same UDP source treats it as
    // a duplicate and never re-ships snapshot/backfill -- symptom is the
    // spectator's silence-failover triggering every 5s in a reconnect
    // loop with the host accepting TCP but never sending data.
    //
    // Also bumps last_seen_ms so the host's own subscriber-expiry sweep
    // doesn't cull this slot mid-rebind.
    for (auto& sub : g_state.subscribers) {
        if (AddrEqual(sub.addr, from)) {
            // Surgical gap-fill: a still-live viewer (TCP conn up) declared
            // a resume cursor BEHIND our live edge. This is the mid-match
            // snapshot-join gap -- the viewer's RC live stream started past
            // the snapshot anchor (RC endpoint-up delay under loss) and the
            // frames between its TCP backfill's end and the RC live start
            // reached it over neither channel. Re-ship exactly
            // [resume_frame .. live-cursor) over the EXISTING reliable conn
            // (SendSessionBackfillFromFrame) without
            // resetting bind state, tearing the path down, or re-sending the
            // snapshot. The viewer's positional dedup drops any overlap
            // with the RC stream, so the gap heals in one round trip. The
            // old recovery (SPEC_LEAVE + full re-JOIN + re-snapshot) looped
            // forever under sustained loss.
            // "Live path" for the surgical/dup branches: the RC endpoint to
            // sub.addr IS the reliable connection, so a bound sub has one.
            const bool reliable_path_up = sub.bound;
            //
            // HOST-SIDE FLOOR + WINDOW (candidate A1). This branch had
            // NEITHER, and it sits ABOVE the 3s destructive-reset floor
            // below, so it was the one re-ship path whose rate was set
            // entirely by the viewer (250ms stall / 500ms throttle) and whose
            // cost was O(session_events) -- a vector that is never trimmed
            // and grows one INPUT per confirmed frame. Worse, the trigger IS
            // a stuck cursor, so resume_frame does not advance while the live
            // edge does: every pull re-encoded a LONGER range than the last,
            // on the MM-timer worker thread holding g_poll_mutex, which the
            // game main thread takes blocking on every gekko receive. That is
            // an episode that gets worse the longer the session has run.
            //
            // The floor is 1s against the viewer's 500ms pull cadence, so at
            // worst the host answers every other pull. It stays well under
            // the starve-escalation rungs (4s), so a viewer whose pulls are
            // genuinely not helping still escalates to the full re-JOIN on
            // schedule.
            if (resume_frame > 0 && sub.bound &&
                !g_state.spec_transport_relay &&
                reliable_path_up) {
                const uint64_t gf_now = GetTickCount64();
                if (sub.last_gapfill_ship_ms != 0 &&
                    gf_now - sub.last_gapfill_ship_ms <
                        SPECTATOR_GAP_FILL_HOST_FLOOR_MS) {
                    // Suppressed. Still ACK -- the viewer must not read this
                    // as an unreachable host and escalate early -- but ship
                    // nothing and log nothing. The count rides out on the
                    // next real ship instead (synchronous logging: a line
                    // here would fire twice a second per stalled viewer).
                    ++sub.gapfill_suppressed;
                    sub.last_seen_ms = gf_now;
                    CtrlPacket ack = BuildJoinAckPacket();  // live: no re-pin
                    ControlChannel_SendTo(ack, sub.addr);
                    return;
                }
                sub.last_seen_ms = gf_now;
                CtrlPacket ack = BuildJoinAckPacket();  // live: no re-pin
                ControlChannel_SendTo(ack, sub.addr);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: gap-fill for %s from INPUT-frame=%u "
                    "(live conn -- reliable re-ship, no reset; window<=%zu "
                    "events, %u pull(s) suppressed by the %llums floor since "
                    "the last ship)",
                    addr_buf, resume_frame, SPECTATOR_GAP_FILL_MAX_EVENTS,
                    sub.gapfill_suppressed,
                    (unsigned long long)SPECTATOR_GAP_FILL_HOST_FLOOR_MS);
                sub.last_gapfill_ship_ms = gf_now;
                sub.gapfill_suppressed   = 0;
                SendSessionBackfillFromFrame(sub.addr, resume_frame,
                                             SPECTATOR_GAP_FILL_MAX_EVENTS);
                return;
            }
            // Destructive-reset rate limit (task #55): floor full
            // bind-reset + re-backfill to one per 3s per sub. A JOIN
            // retry storm (starved viewer, 500ms period) burns at most
            // one full backfill per window; a genuinely restarted viewer
            // still gets its fresh backfill within 3s. NOTE: this is the
            // ONLY reset suppression, and it is deliberately TIME-based
            // rather than state-based. The TCP transport used to add an
            // unconditional "sub is bound, so the stream is flowing --
            // re-ACK and change nothing" dup-suppress on top of it, keyed
            // on a live TCP connection. That branch DIED WITH THE
            // TRANSPORT, and it must not come back keyed on sub.bound
            // instead: an RC sub binds the instant its JOIN_REQ lands, so
            // the suppress would fire on the very first retry and answer
            // it with a LIVE REFRESH, which cannot subscribe anyone. A
            // viewer whose accept ACK was lost would then re-JOIN forever
            // and never boot (measured 2026-08-18, one gate run, viewer
            // stuck at q=0 for the whole session). With no connection to
            // observe there is no liveness signal saying the sub actually
            // received its one-shot backfill, so a viewer that lost it
            // MUST be able to force a re-ship -- the 3s floor bounds the
            // cost of letting it.
            {
                const uint64_t now_ms = GetTickCount64();
                if (sub.last_reset_ms != 0 &&
                    now_ms - sub.last_reset_ms < 3000) {
                    sub.last_seen_ms = now_ms;
                    // Re-STATE the existing grant rather than live state: this
                    // is the branch a first-time viewer whose accept ACK was
                    // lost lands on, and a refresh here could not subscribe it.
                    // Nothing was re-pinned, so pinned_ack_kind is still right.
                    CtrlPacket ack = BuildJoinAckPacketFor(sub);
                    ControlChannel_SendTo(ack, sub.addr);
                    return;
                }
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: JOIN_REQ from existing subscriber %s -- "
                        "resetting bind state for fresh backfill",
                        addr_buf);
            sub.bound    = false;
            sub.ack_frame    = 0;
            // Deliberate re-pin: the grant is being recomputed, so the kind
            // advertised from here on must move with it, never independently.
            sub.pinned_ack_kind = pinned_kind;
            // ...and so must the deep-join grant. This is the branch a
            // deep-join escalation re-JOIN lands on (it suppresses
            // SPEC_JOIN_RESUME precisely to reach here): with the host now in
            // battle, pinned_kind is BATTLE, eligibility drops to false, and
            // the bind's ordinary use_snapshot path ships the blob the held
            // viewer is waiting for. The viewer does NOT clear its hold on a
            // grant that lacks the bit -- that snapshot is what releases it.
            sub.deep_join_eligible = deep_join_eligible;
            sub.deep_join_pending  = false;
            sub.resume_frame = resume_frame;
            sub.last_seen_ms = GetTickCount64();
            sub.last_reset_ms = sub.last_seen_ms;
            // DELIBERATELY NOT the RC endpoint. "Reset bind state" resets
            // everything the HOST owns, and it is worth writing down that the
            // reliable transport is not on that list and must not be:
            //
            // RC state lives in an address-keyed registry
            // (reliable_channel_net.cpp) that survives every app-level
            // re-JOIN, which is why a viewer whose ordered spec stream had
            // head-of-line-blocked could re-JOIN forever into the SAME wedged
            // endpoint and starve while this host was fully reachable
            // (2026-08-07). The repair is real, but it belongs on the VIEWER:
            // the wedge is a pinned RECEIVE cursor, and the viewer resets its
            // own endpoint before sending this JOIN_REQ (SpecForceFullReJoin,
            // spec_join_viewer.cpp). A fresh receiver anchors on whatever
            // msg_seq we send next and the stream is whole again.
            //
            // Resetting the HOST side too would be actively HARMFUL. Our
            // msg_seq would restart at 0 while the viewer's cursor sits at N,
            // and DeliverOrdered only reads a back-jump as "sender endpoint
            // restarted" when it exceeds RC_RESTART_BACKJUMP (256) -- below
            // that it is indistinguishable from an ordinary retransmit and is
            // dropped as a duplicate. RC_CHAN_SPEC_SNAPSHOT typically carries
            // only a few dozen messages per join, so a host-side reset would
            // silently EAT the first N messages of the very backfill this
            // branch exists to re-ship.
            //
            // What the host must do is exactly what it already does: drop the
            // bind so the loop re-ships. See ReliableChannel_ResetPeer's
            // header comment ("resetting ONE side is already correct").
            // Phase 2c: late-arriving spec_user_id backfill. If the
            // first JOIN_REQ raced past our spec_incoming poll (common
            // on loopback where UDP RTT is microseconds), sub.spec_user_id
            // stayed empty -- relay-mode SendTo can't address it. The
            // launcher refreshes the punch dict on every WS event for
            // this addr, so a retry JOIN_REQ should find the user_id
            // by now. Pop on success.
            if (sub.spec_user_id.empty()) {
                auto it = g_state.pending_spec_user_ids.find(addr_buf);
                if (it != g_state.pending_spec_user_ids.end()) {
                    sub.spec_user_id = it->second;
                    g_state.pending_spec_user_ids.erase(it);
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: backfilled spec_user_id=%s on existing "
                        "sub %s (raced past first JOIN_REQ)",
                        sub.spec_user_id.c_str(), addr_buf);
                }
            }
            CtrlPacket ack = BuildJoinAckPacketFor(sub);
            ControlChannel_SendTo(ack, from);
            return;
        }
    }

    if (g_state.subscribers.size() < g_state.capacity) {
        Subscriber sub = {};
        sub.addr         = from;
        sub.last_seen_ms = GetTickCount64();
        sub.ack_frame    = 0;
        sub.bound    = false;
        sub.pinned_ack_kind = pinned_kind;
        sub.deep_join_eligible = deep_join_eligible;
        sub.resume_frame = resume_frame;
        // Phase 2c: pop the cached spec_user_id (if any) for this addr.
        // Punch-target poll wrote it earlier when the hub's
        // spec_incoming forwarded the sub's user_id. Used by relay-mode
        // SendTo to address binary frames; ignored on the direct path.
        {
            char ip_str[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, (void*)&from.sin_addr, ip_str, sizeof(ip_str));
            char addr_key[64];
            std::snprintf(addr_key, sizeof(addr_key), "%s:%u",
                          ip_str, ntohs(from.sin_port));
            auto it = g_state.pending_spec_user_ids.find(addr_key);
            if (it != g_state.pending_spec_user_ids.end()) {
                sub.spec_user_id = it->second;
                g_state.pending_spec_user_ids.erase(it);
            } else {
                // Race fallback: ControlChannel_Poll runs RawReceive
                // (which dispatched this JOIN_REQ) BEFORE
                // TickHostMaintenance's punch-target poll updates our
                // dict. If launcher published the user_id just before
                // this tick, the dict won't have it yet on this call.
                // Read directly from shared mem as the
                // single-source-of-truth fallback. If the shm punch
                // target matches our `from` addr, use its user_id.
                FM2KSharedMemData* shm = GetSharedMemory();
                if (shm && shm->magic == FM2K_SHARED_MEM_MAGIC &&
                    shm->spectator_punch_ip_be == from.sin_addr.s_addr &&
                    shm->spectator_punch_port  == ntohs(from.sin_port) &&
                    shm->spectator_punch_user_id[0]) {
                    sub.spec_user_id = std::string(
                        shm->spectator_punch_user_id,
                        strnlen(shm->spectator_punch_user_id,
                                sizeof(shm->spectator_punch_user_id)));
                }
            }
        }
        g_state.subscribers.push_back(sub);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: Accepted subscriber %s (%zu/%zu, "
                    "transport=%s, user_id=%s)",
                    addr_buf, g_state.subscribers.size(), g_state.capacity,
                    g_state.spec_transport_relay ? "RELAY" : "RC",
                    sub.spec_user_id.empty() ? "(none)" : sub.spec_user_id.c_str());

        CtrlPacket ack = BuildJoinAckPacketFor(sub);
        ControlChannel_SendTo(ack, from);

        // If we already have a live GekkoNet session, add this late joiner
        // as a GekkoSpectator actor so the input stream reaches them.
        AddSpectatorToSession(from);

        // INITIAL_MATCH + SendSessionBackfillFromStart are sent by TickHealth's
        // TryBindPendingTCP path the first time the spectator's accepted
        // TCP connection gets paired with this subscriber slot.
        return;
    }

    // At capacity -- random redirect à la CCCaster.
    if (!g_state.subscribers.empty()) {
        const size_t i = static_cast<size_t>(std::rand()) % g_state.subscribers.size();
        const sockaddr_in& target = g_state.subscribers[i].addr;

        CtrlPacket redir = {};
        redir.header.type = CtrlMsg::SPEC_JOIN_REDIRECT;
        redir.data.spec_redirect.redirect_ip   = target.sin_addr.s_addr;
        redir.data.spec_redirect.redirect_port = ntohs(target.sin_port);
        ControlChannel_SendTo(redir, from);

        char target_buf[48] = {};
        FormatAddr(target, target_buf, sizeof(target_buf));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: At capacity, redirecting %s -> %s",
                    addr_buf, target_buf);
        return;
    }

    // Capacity=0, no subscribers -- reject with null redirect. Viewer gives up.
    CtrlPacket redir = {};
    redir.header.type = CtrlMsg::SPEC_JOIN_REDIRECT;
    redir.data.spec_redirect.redirect_ip   = 0;
    redir.data.spec_redirect.redirect_port = 0;
    ControlChannel_SendTo(redir, from);
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Rejected JOIN_REQ from %s (capacity=0)", addr_buf);
}

void SpectatorNode_HandleLeave(const sockaddr_in& from) {
    // First check: was this from our upstream telling us to leave (it's
    // shutting down)? If so, immediately fail over to root rather than
    // waiting out the silence timer.
    if (g_state.subscribed_upstream && AddrEqual(g_state.upstream_addr, from)) {
        char buf[48] = {}; FormatAddr(from, buf, sizeof(buf));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: upstream %s sent SPEC_LEAVE -- failing over to root",
                    buf);
        g_state.subscribed_upstream = false;
        // TickHealth will pick up the disconnected state and trigger
        // RequestJoin(root) on its next call (rate-limited).
        return;
    }
    // Otherwise, treat as a downstream subscriber leaving us.
    for (auto it = g_state.subscribers.begin(); it != g_state.subscribers.end(); ++it) {
        if (AddrEqual(it->addr, from)) {
            char buf[48] = {}; FormatAddr(from, buf, sizeof(buf));
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: Subscriber %s left", buf);
            g_state.subscribers.erase(it);
            return;
        }
    }
}

// Host side: tell every subscriber the session is ending cleanly (player quit /
// left). Without this the viewer sees only the dropped stream and treats it as a
// glitch -- storm-reconnecting to a dead host for seconds. Sent a few times
// because UDP is lossy and we tear down right after. Mirrors the expiry sweep's
// SPEC_LEAVE send (spec_health.cpp) but with do-not-reconnect semantics.
void SpectatorNode_BroadcastSessionEnd() {
    if (g_state.subscribers.empty()) return;
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::SPEC_SESSION_END;
    for (int rep = 0; rep < 3; ++rep) {
        for (const auto& sub : g_state.subscribers) {
            ControlChannel_SendTo(pkt, sub.addr);
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: broadcast SPEC_SESSION_END to %zu subscriber(s)",
                g_state.subscribers.size());
}


void SpectatorNode_HandleHeartbeat(const sockaddr_in& from) {
    // Viewer side: an echo from the upstream is the gameplay-independent
    // liveness proof. The session-derived datagram flow stops whenever
    // the host is between sessions (CSS sync under loss can take many
    // seconds) and the silence failover then read a healthy-but-quiet
    // host as dead -- the CSS "disconnect/re-subscribe" the user kept
    // seeing (2026-06-11 14:32).
    if (g_state.subscribed_upstream &&
        AddrEqual(from, g_state.upstream_addr)) {
        g_state.last_upstream_packet_ms = GetTickCount64();
        return;
    }
    for (auto& sub : g_state.subscribers) {
        if (AddrEqual(sub.addr, from)) {
            sub.last_seen_ms = GetTickCount64();
            // Echo so the viewer's liveness clock ticks even when no
            // session is confirming frames (1Hz, 16 bytes).
            CtrlPacket hb = {};
            hb.header.type = CtrlMsg::SPEC_HEARTBEAT;
            ControlChannel_SendTo(hb, sub.addr);
            return;
        }
    }
}

// -----------------------------------------------------------------------------
// STATUS
// -----------------------------------------------------------------------------

size_t SpectatorNode_GetSubscriberCount() { return g_state.subscribers.size(); }
bool   SpectatorNode_IsBroadcasting()     { return g_state.broadcasting;      }

std::vector<sockaddr_in> SpectatorNode_GetSubscriberAddrs() {
    std::vector<sockaddr_in> out;
    out.reserve(g_state.subscribers.size());
    for (const auto& sub : g_state.subscribers) {
        out.push_back(sub.addr);
    }
    return out;
}

