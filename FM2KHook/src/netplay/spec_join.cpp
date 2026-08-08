// Spectator join protocol, HOST side: JOIN_REQ accept/redirect, the
// GekkoSpectator dedup set, SESSION_END broadcast, and JOIN_ACK construction.
// The viewer half (request/ack/redirect/kick) lives in the sibling
// spec_join_viewer.cpp; they share spec_join_internal.h. Public API decls in
// spectator_node.h; reaches specnode helpers via using.
#include "spectator_node.h"
#include "spectator_node_internal.h"  // shared State model + g_state (split for sibling TUs)
#include "spec_join_internal.h"       // shared with spec_join_viewer.cpp
#include "spectator_tcp.h"        // TCP transport for INPUT_BATCH stream
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
// wants the chars, and a FULL_SESSION grant advertises CSS/NONE and must carry
// NO chars at all -- a /F-boot seed handed to a from-frame-0 mirror is exactly
// the bug this split of the builder exists to prevent.
static CtrlPacket BuildJoinAckWithKind(uint8_t advertised_kind, bool with_chars) {
    CtrlPacket ack = {};
    ack.header.type = CtrlMsg::SPEC_JOIN_ACK;
    ack.data.spec_join_ack.host_session_kind = advertised_kind;
    // Tell the spectator which TCP port to dial for the INPUT_BATCH
    // stream. Zero would mean the listener failed at startup, in which
    // case the spectator refuses the subscription.
    ack.data.spec_join_ack.host_tcp_port = SpectatorTCP::GetListenPort();
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
// pinned alongside its join_mode -- decided once, from one
// Netplay_GetSessionKind() read, and re-decided only when the host
// deliberately re-pins the mode (the destructive-reset branch, which updates
// both in the same block).
CtrlPacket BuildJoinAckPacketFor(const Subscriber& sub) {
    // No SPEC_ACK_LIVE_REFRESH bit: this IS the grant. Chars only for a BATTLE
    // grant -- handing /F-boot seeds to a from-frame-0 mirror is the defect.
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

void SpectatorNode_HandleJoinReq(const sockaddr_in& from, SpecJoinMode mode,
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
    const bool udp_ok = SpecUdpEnabled() && (caps & SPEC_JOIN_UDP_OK) != 0;
    if ((caps & SPEC_JOIN_RESUME) == 0) resume_frame = 0;
    // Pin the mode NOW, from the host's state at this instant -- the
    // same instant the ACK's kind is computed from, so the viewer's
    // natural-boot/battle-boot decision and the host's delivery path
    // can never diverge. (The bind used to decide from its own LATER
    // state: a CSS-time joiner whose bind fired after battle started
    // got a battle snapshot against a title-screen engine = deadlock.)
    //
    // ONE read, reused for both halves of the decision. That comment above
    // was aspirational until now: the mode came from this read, but the ACK's
    // kind came from a SECOND Netplay_GetSessionKind() inside the builder --
    // and the battle-entry re-broadcast took a THIRD. A host crossing into
    // battle between them handed a FULL_SESSION-granted viewer kind=BATTLE,
    // which is the desync this fixes. pinned_ack_kind rides on the Subscriber
    // so every later re-ACK repeats the same answer.
    const NetplaySessionKind kind_at_pin = Netplay_GetSessionKind();
    // Design 2: the CURRENT_MATCH -> FULL_SESSION downgrade that used to live
    // here is GONE. It fired whenever the host was not in battle, which is the
    // common case for a hub "spectate" click, and FULL_SESSION means the bind
    // ships session_events from index 0 -- every prior char-select and every
    // prior battle of an ongoing set. Battle frames replay cheaply, but each
    // mirrored CSS cursor move can trigger a cold .player load, so a viewer
    // joining a long set watched ~a minute of fast-forward before reaching
    // live. The mode now survives, and the bind serves a bounded backfill
    // anchored at the current char-select instead (spec_health.cpp).
    //
    // FULL_SESSION is still honoured when the VIEWER asks for it
    // (FM2K_SPECTATE_MODE=full, netplay.cpp) -- that is the streamer/archivist
    // opt-in and the escape hatch if a bounded join ever misbehaves.
    //
    // Nothing about the viewer's boot posture changes: pinned_kind below is
    // still CSS/NONE for a non-battle join, so the viewer natural-boots and
    // mirrors char-select exactly as it does today. Only the host's choice of
    // where the stream STARTS moves.
    if (mode == SpecJoinMode::FULL_SESSION) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: viewer explicitly requested FULL_SESSION -- "
            "from-frame-0 stream (deep-join bound opted out)");
    }
    // The grant advertised to this subscriber, from the same read. BATTLE
    // means and ONLY means "snapshot join: /F-boot into battle, a snapshot is
    // coming". A FULL_SESSION grant therefore never advertises BATTLE even
    // when the host is in battle -- that viewer gets the from-frame-0 stream,
    // which starts at CSS, so CSS is the posture it must boot into. Getting
    // this backwards is the whole defect: a FULL_SESSION viewer told BATTLE
    // /F-boots and then feeds itself CSS inputs as battle inputs.
    const uint8_t pinned_kind =
        (mode == SpecJoinMode::CURRENT_MATCH &&
         kind_at_pin == NetplaySessionKind::BATTLE)
            ? SPEC_ACK_KIND_BATTLE
            : (kind_at_pin == NetplaySessionKind::BATTLE
                   ? SPEC_ACK_KIND_CSS
                   : static_cast<uint8_t>(kind_at_pin));
    // Wave 4: the bounded deep-join decision, taken HERE, in the same block and
    // from the same single Netplay_GetSessionKind() read as the mode pin and
    // the grant kind. The bind used to re-derive it a few ticks later against
    // moved state, so the grant a viewer booted on and the payload it was
    // actually shipped could disagree; keying the bind on this field removes
    // that class entirely (it is the same lesson as pinned_ack_kind).
    //
    // Requires all four: the feature gate, a CURRENT_MATCH request, a NON-battle
    // grant (a BATTLE grant already means "a snapshot is coming" through the
    // ordinary bind path), and an actual CSS anchor -- HaveBoundedAnchor() is
    // false for a fresh session-start join, which therefore keeps taking the
    // from-frame-0 path byte for byte and can never be told to hold.
    const bool deep_join_eligible =
        DeepJoinEnabled() &&
        mode == SpecJoinMode::CURRENT_MATCH &&
        pinned_kind != SPEC_ACK_KIND_BATTLE &&
        HaveBoundedAnchor();
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
    // tcp_bound=true; the next JOIN_REQ from same UDP source treats it as
    // a duplicate and never re-ships snapshot/backfill -- symptom is the
    // spectator's silence-failover triggering every 5s in a reconnect
    // loop with the host accepting TCP but never sending data.
    //
    // Also refreshes join_mode in case the spectator switched modes (e.g.
    // CURRENT_MATCH on first connect, FULL_SESSION on retry after fallback)
    // and bumps last_seen_ms so the host's own subscriber-expiry sweep
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
            // (SendSessionBackfillFromFrame -> TCP by default) without
            // resetting bind state, dropping the conn, or re-sending the
            // snapshot. The viewer's positional dedup drops any overlap
            // with the RC stream, so the gap heals in one round trip. The
            // old recovery (SPEC_LEAVE + full re-JOIN + re-snapshot) looped
            // forever under sustained loss.
            // "Live conn" for the surgical/dup branches: a healthy TCP
            // conn, OR RC full-transport mode where the RC endpoint to
            // sub.addr IS the reliable conn (task #55: HasLiveConnFor is
            // always false with no TCP dial, which made every gap-fill
            // pull fall through to the destructive full-reset path --
            // 500ms full-re-backfill storm whenever a viewer starved).
            const bool reliable_path_up =
                SpectatorTCP::HasLiveConnFor(sub.addr) ||
                (SpecRcSnapshotEnabled() && sub.tcp_bound);
            if (resume_frame > 0 && sub.tcp_bound &&
                !g_state.spec_transport_relay &&
                reliable_path_up) {
                sub.last_seen_ms = GetTickCount64();
                sub.udp_ok       = udp_ok;
                CtrlPacket ack = BuildJoinAckPacket();  // live: no re-pin
                ControlChannel_SendTo(ack, sub.addr);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: gap-fill for %s from INPUT-frame=%u "
                    "(live conn -- reliable re-ship, no reset)",
                    addr_buf, resume_frame);
                SendSessionBackfillFromFrame(sub.addr, resume_frame);
                return;
            }
            // Destructive-reset rate limit (task #55): floor full
            // bind-reset + re-backfill to one per 3s per sub. A JOIN
            // retry storm (starved viewer, 500ms period) burns at most
            // one full backfill per window; a genuinely restarted viewer
            // still gets its fresh backfill within 3s. NOTE: this is the
            // ONLY reset suppression in RC mode -- an unconditional
            // dup-suppress (like the TCP branch below) was tried and
            // broke self-heal: with no TCP conn there is no liveness
            // signal to know the sub actually got its one-shot backfill,
            // so a viewer that lost it must be able to force a re-ship.
            {
                const uint64_t now_ms = GetTickCount64();
                if (sub.last_reset_ms != 0 &&
                    now_ms - sub.last_reset_ms < 3000) {
                    sub.last_seen_ms = now_ms;
                    sub.udp_ok       = udp_ok;
                    // Re-STATE the existing grant rather than live state: this
                    // is the branch a first-time viewer whose accept ACK was
                    // lost lands on, and a refresh here could not subscribe it.
                    // Nothing was re-pinned, so pinned_ack_kind is still right.
                    CtrlPacket ack = BuildJoinAckPacketFor(sub);
                    ControlChannel_SendTo(ack, sub.addr);
                    return;
                }
            }
            if (sub.tcp_bound && !g_state.spec_transport_relay &&
                SpectatorTCP::HasLiveConnFor(sub.addr)) {
                // Live stream already flowing: this JOIN_REQ is a dup or
                // an over-eager retry. Re-ACK and change NOTHING -- the
                // old reset dropped the conn the previous JOIN opened,
                // and the viewer's heal retried 500ms later: an infinite
                // join storm that DoS'ed this host's main loop and
                // starved its own netplay sends (one-directional 30s
                // blackout -> P1/P2 barrier wedge).
                sub.last_seen_ms = GetTickCount64();
                sub.udp_ok       = udp_ok;
                CtrlPacket ack = BuildJoinAckPacket();  // live: no re-pin
                ControlChannel_SendTo(ack, sub.addr);
                return;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: JOIN_REQ from existing subscriber %s -- "
                        "resetting bind state for fresh backfill (mode=%s)",
                        addr_buf,
                        mode == SpecJoinMode::CURRENT_MATCH ? "CURRENT_MATCH"
                                                            : "FULL_SESSION");
            sub.tcp_bound    = false;
            sub.ack_frame    = 0;
            sub.join_mode    = mode;
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
            sub.udp_ok       = udp_ok;
            sub.resume_frame = resume_frame;
            sub.last_seen_ms = GetTickCount64();
            sub.last_reset_ms = sub.last_seen_ms;
            // Drop the old TCP conn + any stale pending clients from this
            // IP so the bind path pairs the spectator's FRESH dial instead
            // of an abandoned one (deep-join reconnect-loop fix).
            SpectatorTCP::DropConnectionsFromAddr(sub.addr);
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
        sub.tcp_bound    = false;
        sub.join_mode    = mode;
        sub.pinned_ack_kind = pinned_kind;
        sub.deep_join_eligible = deep_join_eligible;
        sub.udp_ok       = udp_ok;
        sub.resume_frame = resume_frame;
        // Phase 2c: pop the cached spec_user_id (if any) for this addr.
        // Punch-target poll wrote it earlier when the hub's
        // spec_incoming forwarded the sub's user_id. Used by relay-mode
        // SendTo to address binary frames; ignored in TCP mode.
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
                    "SpectatorNode: Accepted subscriber %s (%zu/%zu, mode=%s, "
                    "transport=%s, user_id=%s)",
                    addr_buf, g_state.subscribers.size(), g_state.capacity,
                    mode == SpecJoinMode::CURRENT_MATCH ? "CURRENT_MATCH"
                                                       : "FULL_SESSION",
                    g_state.spec_transport_relay ? "RELAY" : "TCP",
                    sub.spec_user_id.empty() ? "(none)" : sub.spec_user_id.c_str());

        CtrlPacket ack = BuildJoinAckPacketFor(sub);
        ControlChannel_SendTo(ack, from);

        // If we already have a live GekkoNet session, add this late joiner
        // as a GekkoSpectator actor so the input stream reaches them.
        AddSpectatorToSession(from);

        // INITIAL_MATCH + SendSessionBackfillTo are sent by TickHealth's
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
            SpectatorTCP::DisconnectSubscriber(it->addr);
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
        g_state.last_udp_recv_ms = GetTickCount64();
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

