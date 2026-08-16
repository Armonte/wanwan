// reliable_channel_liveness.cpp -- the S0-S3 liveness machinery: the
// FM2K_RC_LIVENESS kill switch, the holding-advertisement codec (build + parse
// of the ack carrier's trailing section), and the transport predicate the
// spectator starve ladder consults before it destroys an endpoint.
//
// Split out of reliable_channel.cpp purely for the 1000-line cap, but the seam
// is a real one: nothing here touches framing, ordering, pacing or retransmit.
// It is policy ("is this peer alive, and is it still holding what we are
// missing?") plus one wire codec. The state model both halves share is
// reliable_channel_internal.h.
//
// The wire format and the reasoning behind every field live next to
// RC_CARRIER_EXT_MAGIC in reliable_channel_internal.h -- read that first.
#include "reliable_channel_internal.h"

#include <SDL3/SDL_log.h>
#include <cctype>
#include <cstdlib>   // std::getenv -- FM2K_RC_LIVENESS gate
#include <cstring>
#include <string>

namespace fm2k {
namespace rc {

// FM2K_RC_LIVENESS -- DEFAULT ON, kill-switch semantics and STRICT parse,
// deliberately the same shape as PoolSync_Enabled() / SeamGuard's lever. The
// only load-bearing direction is OFF: it restores the pre-fix transport
// contract byte-for-byte (no carrier keepalive, flat unconditional 7s retire,
// no holding advertisement emitted or honoured, IsRepairingOrdered() always
// false), which is what makes "does this still happen with the liveness fix
// off?" a real triage question and what the causality control run is taken
// against. An unrecognised value fails SAFE to OFF and says so.
//
// Read once and cached: this is on the per-message retire path, and the hook's
// logging is SYNCHRONOUS (quill is hardcoded off in FM2KHook) so neither the
// getenv nor a log line may run per message.
bool LivenessEnabled() {
    static int s_enabled = -1;
    if (s_enabled >= 0) return s_enabled == 1;

    const char* raw = std::getenv("FM2K_RC_LIVENESS");
    std::string v = raw ? raw : "";
    const size_t b = v.find_first_not_of(" \t\r\n");
    const size_t e = v.find_last_not_of(" \t\r\n");
    v = (b == std::string::npos) ? std::string() : v.substr(b, e - b + 1);
    for (char& c : v) c = (char)std::tolower((unsigned char)c);

    const char* why = nullptr;
    if (v.empty()) {
        s_enabled = 1;
        why = "default; FM2K_RC_LIVENESS unset";
    } else if (v == "0" || v == "false" || v == "off" || v == "no" ||
               v == "disabled") {
        s_enabled = 0;
        why = "FM2K_RC_LIVENESS kill-switch";
    } else if (v == "1" || v == "true" || v == "on" || v == "yes" ||
               v == "enabled") {
        s_enabled = 1;
        why = "FM2K_RC_LIVENESS set explicitly";
    } else {
        s_enabled = 0;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[RC-LIVENESS] liveness-conditional retirement disabled -- "
            "FM2K_RC_LIVENESS=\"%s\" is not a recognised value (accepted: "
            "1/true/on/yes/enabled, 0/false/off/no/disabled). Failing SAFE to "
            "the pre-fix flat-TTL behaviour; UNSET the variable to get the "
            "default (ON)", raw ? raw : "");
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[RC-LIVENESS] ordered-channel liveness %s (%s)",
        s_enabled ? "ENABLED -- the sender retires an unacked message at 7s "
                    "only when the peer is SILENT (or at the 25s ceiling / 2MB "
                    "cap), the ack carrier keepalives at 0.5s so a one-way "
                    "outage cannot fake a dead peer, and each side advertises "
                    "the oldest seq it still holds so the receiver un-anchors "
                    "on a fact instead of a 3.5s timer"
                  : "disabled -- flat unconditional 7s retire, no keepalive, "
                    "no holding advertisement (pre-fix behaviour)",
        why);
    return s_enabled == 1;
}

// S3: is any ordered rx channel of this endpoint parked at a hole the peer
// says it is still holding? The latch is set/cleared only by the stall sweep,
// so this is a cheap read of already-decided state -- no timers, no policy of
// its own, and nothing for a caller to get subtly out of step with.
bool IsRepairingOrdered(Endpoint* ep) {
    if (!ep) return false;
    for (auto& kv : ep->rx) if (kv.second.holding_repair) return true;
    return false;
}

// Append the holding section to an ack-carrier body already carrying its
// cumulative block, and return the new offset. Emits nothing (and returns `off`
// unchanged) when this endpoint has no ORDERED tx channel, which is what keeps
// the section absent -- rather than empty -- on a peer that never sends ordered
// traffic.
size_t CarrierAppendHoldSection(Endpoint* ep, uint8_t* body, size_t off) {
    // Purely additive: a pre-fix peer stops after `cnt` entries and never looks
    // at these bytes. See the format note in reliable_channel_internal.h.
    const size_t hdr_off = off;
    uint8_t hcnt = 0;
    size_t hoff = off + 2;
    for (auto& kv : ep->tx) {
        if (!kv.second.cls_known || !kv.second.cls_ordered) continue;
        if (hcnt >= RC_CARRIER_MAX_ENTRIES) break;
        const TxChannel& tc = kv.second;
        // The lowest msg_seq still live: unacked first, then merely QUEUED (a
        // message with a seq but not yet pumped is held too, and advertising
        // next_msg_seq over it would tell the receiver to skip something we
        // are about to send), else next_msg_seq with holding=0.
        const bool holding = !tc.unacked.empty() || tc.queued_count > 0;
        uint64_t oldest = !tc.unacked.empty() ? tc.unacked.begin()->first
                        : (tc.queued_count > 0 ? tc.queued_lowest
                                               : tc.next_msg_seq);
        body[hoff]     = kv.first;
        body[hoff + 1] = holding ? 1 : 0;
        std::memcpy(body + hoff + 2, &oldest, 8);
        hoff += RC_CARRIER_HOLD_STRIDE; hcnt++;
    }
    if (hcnt == 0) return off;
    body[hdr_off]     = RC_CARRIER_EXT_MAGIC;
    body[hdr_off + 1] = hcnt;
    ep->st_hold_adverts_sent++;
    return hoff;
}

// Read a peer's holding section, if present, from the bytes remaining after the
// cumulative ack block. `p` is where that block stopped and `end` bounds the
// carrier payload.
//
// THIS IS ATTACKER-CONTROLLED DATA from an unauthenticated UDP source: every
// read is bounds-checked, `hcount` is clamped, and entries naming a channel we
// have no ordered rx cursor for are ignored. A pre-fix peer's carrier simply
// ends at `p`, so nothing below runs and the receiver keeps today's behaviour --
// that is the mixed-version path, and it degrades to current behaviour rather
// than to something worse.
void CarrierParseHoldSection(Endpoint* ep, const uint8_t* p, const uint8_t* end) {
    if (!LivenessEnabled()) return;
    if (p + 2 > end || p[0] != RC_CARRIER_EXT_MAGIC) return;
    uint8_t hcount = p[1];
    if (hcount > RC_CARRIER_MAX_ENTRIES) hcount = RC_CARRIER_MAX_ENTRIES;
    p += 2;
    for (uint8_t i = 0; i < hcount && p + RC_CARRIER_HOLD_STRIDE <= end; i++) {
        uint8_t  hchan   = p[0];
        bool     holding = p[1] != 0;
        uint64_t oldest; std::memcpy(&oldest, p + 2, 8);
        p += RC_CARRIER_HOLD_STRIDE;
        auto rit = ep->rx.find(hchan);
        if (rit == ep->rx.end() || !rit->second.cls_ordered) continue;
        rit->second.peer_hold_oldest = oldest;
        rit->second.peer_hold_active = holding;
        rit->second.peer_hold_known  = true;
        rit->second.peer_hold_time   = ep->now_time;
        // DISARM on a positive claim. An un-anchor armed while we had no
        // information about the sender adopts whatever arrives first, and after
        // a link recovery that is an ARBITRARY member of the retained backlog
        // (retransmits are phase-staggered, not synchronised), which silently
        // discards its head. The moment the sender says it is still holding our
        // hole, that arm is provably wrong.
        //
        // ...UNLESS the arm came from the HARD BACKSTOP. That one is the
        // receiver's guarantee that no peer behaviour can pin it past
        // RC_ORDERED_STALL_HARD_SEC, and honouring the disarm there defeated it
        // completely: the peer's next carrier (33ms receive-driven, 0.5s on the
        // S0 keepalive) cleared the arm before any message could adopt it, so
        // the ladder deferred 3.5s -> 9s, armed, disarmed, and cycled forever.
        // The bound was 25s against an honest peer and unbounded against a
        // hostile one, while the comments claimed 9s. See
        // RxChannel::stall_resync_backstop. Deferral is a courtesy to a peer
        // that says it is still trying; the backstop is not negotiable.
        if (holding && oldest <= rit->second.next_deliver &&
            !rit->second.stall_resync_backstop)
            rit->second.stall_resync = false;
    }
}

}  // namespace rc
}  // namespace fm2k
