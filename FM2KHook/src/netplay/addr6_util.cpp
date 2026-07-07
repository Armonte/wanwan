#include "addr6_util.h"
#include <windows.h>
#include <SDL3/SDL_log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <deque>
#include <mutex>
#include <vector>

namespace fm2k {

// Set once at init (NetSocket_Init) before any send/recv -- write-once, then
// read concurrently from the poll + punch threads, which is safe.
static bool s_socket_is_v6 = false;

void Addr_SetSocketV6(bool is_v6) { s_socket_is_v6 = is_v6; }

bool Addr_IsV4Mapped(const sockaddr_in6& a6) {
    return a6.sin6_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&a6.sin6_addr);
}

sockaddr_in Addr_Unmap4(const sockaddr_in6& a6) {
    sockaddr_in a4{};
    a4.sin_family = AF_INET;
    std::memcpy(&a4.sin_addr, &a6.sin6_addr.s6_addr[12], 4);
    a4.sin_port = a6.sin6_port;
    return a4;
}

sockaddr_in6 Addr_Map4(const sockaddr_in& a4) {
    sockaddr_in6 a6{};
    a6.sin6_family = AF_INET6;
    a6.sin6_addr.s6_addr[10] = 0xFF;  // ::ffff: prefix
    a6.sin6_addr.s6_addr[11] = 0xFF;
    std::memcpy(&a6.sin6_addr.s6_addr[12], &a4.sin_addr, 4);
    a6.sin6_port = a4.sin_port;
    return a6;
}

void Addr_NormalizeRecv(const sockaddr_in6& from6, sockaddr_storage& out) {
    std::memset(&out, 0, sizeof(out));
    if (!s_socket_is_v6) {
        // v4-fallback socket: the kernel wrote a sockaddr_in into the buffer.
        std::memcpy(&out, &from6, sizeof(sockaddr_in));
        return;
    }
    if (Addr_IsV4Mapped(from6)) {
        sockaddr_in a4 = Addr_Unmap4(from6);
        std::memcpy(&out, &a4, sizeof(a4));
    } else {
        // native IPv6 source
        std::memcpy(&out, &from6, sizeof(sockaddr_in6));
    }
}

std::string Addr_ActorString(const sockaddr_storage& sa) {
    char ip[INET6_ADDRSTRLEN] = {};
    char buf[80] = {};
    if (sa.ss_family == AF_INET6) {
        const sockaddr_in6* a6 = reinterpret_cast<const sockaddr_in6*>(&sa);
        inet_ntop(AF_INET6, const_cast<in6_addr*>(&a6->sin6_addr), ip, sizeof(ip));
        std::snprintf(buf, sizeof(buf), "[%s]:%d", ip, (int)ntohs(a6->sin6_port));
    } else {
        const sockaddr_in* a4 = reinterpret_cast<const sockaddr_in*>(&sa);
        // BYTE-IDENTICAL to the legacy v4 format (inet_ntoa -> "%s:%d"); using
        // inet_ntop(AF_INET) here is reentrant and yields the same dotted quad.
        inet_ntop(AF_INET, const_cast<in_addr*>(&a4->sin_addr), ip, sizeof(ip));
        std::snprintf(buf, sizeof(buf), "%s:%d", ip, (int)ntohs(a4->sin_port));
    }
    return buf;
}

bool Addr_Equal(const sockaddr_storage& a, const sockaddr_storage& b) {
    if (a.ss_family != b.ss_family) return false;
    if (a.ss_family == AF_INET6) {
        const sockaddr_in6* x = reinterpret_cast<const sockaddr_in6*>(&a);
        const sockaddr_in6* y = reinterpret_cast<const sockaddr_in6*>(&b);
        return x->sin6_port == y->sin6_port &&
               std::memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(in6_addr)) == 0;
    }
    const sockaddr_in* x = reinterpret_cast<const sockaddr_in*>(&a);
    const sockaddr_in* y = reinterpret_cast<const sockaddr_in*>(&b);
    return x->sin_port == y->sin_port &&
           x->sin_addr.s_addr == y->sin_addr.s_addr;
}

bool Addr_HasPort(const sockaddr_storage& sa) {
    if (sa.ss_family == AF_INET6)
        return reinterpret_cast<const sockaddr_in6*>(&sa)->sin6_port != 0;
    return reinterpret_cast<const sockaddr_in*>(&sa)->sin_port != 0;
}

static void LogFamilyErr(int r, bool v6) {
    if (r == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        if (err == WSAEAFNOSUPPORT || err == WSAEINVAL) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "addr6: send family error %d (is_v6=%d) -- dual-stack misroute",
                err, (int)v6);
        }
    }
}

// Raw family-aware send (the real sendto). Kept separate so the test-only
// network-impairment intercept can re-emit delayed packets without re-impairing.
static int Sendto4or6_Raw(SOCKET s, const char* buf, int len, const sockaddr_in& dst4) {
    int r;
    if (s_socket_is_v6) {
        sockaddr_in6 d6 = Addr_Map4(dst4);
        r = sendto(s, buf, len, 0,
                   reinterpret_cast<const sockaddr*>(&d6), sizeof(d6));
    } else {
        r = sendto(s, buf, len, 0,
                   reinterpret_cast<const sockaddr*>(&dst4), sizeof(dst4));
    }
    LogFamilyErr(r, s_socket_is_v6);
    return r;
}

// ---- Test-only network impairment on the whole UDP link (control + gekko) ---
// FM2K_NET_DELAY_MS (one-way ms), FM2K_NET_JITTER_MS (random +/-), FM2K_NET_LOSS
// (drop prob 0..1), FM2K_NET_REORDER (0..1: chance a packet is released out of
// order, modeling clumsy's ood), FM2K_NET_DUP (0..1: chance a packet is duplicated,
// modeling clumsy's duplicate), FM2K_NET_SEED. RTT ~= 2*delay (each peer impairs its
// own outbound). Reproduces a real link IN-PROCESS -- jitter/reorder/dup like the
// clumsy WinDivert tool (jagt/clumsy) -- without needing an external conditioner.
// The delay queue is drained in due-time order so reordered due-times manifest.
namespace {
struct DelayedSend { std::vector<char> buf; sockaddr_in dst; SOCKET s; uint64_t due_ms; };
std::mutex              g_ni_mutex;
std::deque<DelayedSend> g_ni_queue;
int      g_ni_delay   = -1;     // one-way ms; -1 = uninit
int      g_ni_jitter  = 0;
double   g_ni_loss    = 0.0;
double   g_ni_reorder = 0.0;    // clumsy ood: chance a packet's release time is shuffled
double   g_ni_dup     = 0.0;    // clumsy duplicate: chance a packet is sent twice
uint64_t g_ni_rng     = 0;
void NetImpairInit() {
    if (g_ni_delay >= 0) return;
    const char* j  = std::getenv("FM2K_NET_JITTER_MS");
    const char* l  = std::getenv("FM2K_NET_LOSS");
    const char* ro = std::getenv("FM2K_NET_REORDER");
    const char* du = std::getenv("FM2K_NET_DUP");
    const char* sd = std::getenv("FM2K_NET_SEED");
    const char* d  = std::getenv("FM2K_NET_DELAY_MS");
    g_ni_jitter  = j ? std::atoi(j) : 0;
    g_ni_loss    = l ? std::atof(l) : 0.0;
    g_ni_reorder = ro ? std::atof(ro) : 0.0;
    g_ni_dup     = du ? std::atof(du) : 0.0;
    g_ni_rng     = sd ? std::strtoull(sd, nullptr, 10) : 0x9E3779B97F4A7C15ULL;
    g_ni_delay   = d ? std::atoi(d) : 0;          // set LAST: arms the fast-path gate
    if (g_ni_delay > 0 || g_ni_loss > 0.0 || g_ni_reorder > 0.0 || g_ni_dup > 0.0)
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[NET-IMPAIR] one-way delay=%dms jitter=%dms loss=%.3f reorder=%.3f dup=%.3f "
            "(RTT~%dms) -- TEST ONLY",
            g_ni_delay, g_ni_jitter, g_ni_loss, g_ni_reorder, g_ni_dup, g_ni_delay * 2);
}
bool NiImpaired() {
    return g_ni_delay > 0 || g_ni_loss > 0.0 || g_ni_reorder > 0.0 || g_ni_dup > 0.0;
}
uint64_t NiRng() {
    uint64_t x = g_ni_rng; x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    g_ni_rng = x; return x * 0x2545F4914F6CDD1DULL;
}
}  // namespace

// Re-emit delayed packets whose time has come (raw -> no re-impair). Caller
// must hold g_ni_mutex.
static void NetImpair_PumpLocked(uint64_t now) {
    // Send every due packet in DUE order (not queue order) so a reordered/held
    // packet with a later due leaves after the successors that overtook it -- this
    // is what makes FM2K_NET_REORDER actually manifest as out-of-order delivery.
    std::vector<DelayedSend*> due;
    for (auto& p : g_ni_queue) if (p.due_ms <= now) due.push_back(&p);
    std::sort(due.begin(), due.end(),
              [](const DelayedSend* a, const DelayedSend* b) { return a->due_ms < b->due_ms; });
    for (auto* p : due) Sendto4or6_Raw(p->s, p->buf.data(), (int)p->buf.size(), p->dst);
    for (auto it = g_ni_queue.begin(); it != g_ni_queue.end(); )
        it = (it->due_ms <= now) ? g_ni_queue.erase(it) : std::next(it);
}

// Periodic pump -- called from the MM-timer worker (control_channel) so the
// delay queue flushes every few ms REGARDLESS of whether this peer is sending.
// Without it the queue drained only on send, so a CSS-sync / battle-entry stall
// where BOTH peers wait on each other's delayed packet wedged: the awaited
// packet sat un-flushed because neither peer was sending (the deadlock the
// player saw under RTT). The MM-timer fires independent of the stalled main loop.
void NetImpair_Pump() {
    // Only delay + reorder HOLD packets in the queue; loss/dup don't linger.
    if (g_ni_delay <= 0 && g_ni_reorder <= 0.0) return;
    std::lock_guard<std::mutex> lock(g_ni_mutex);
    NetImpair_PumpLocked(GetTickCount64());
}

// [0,1) uniform roll from the seeded RNG.
static double NiRoll() { return (double)(NiRng() >> 11) * (1.0 / 9007199254740992.0); }

int Sendto4or6(SOCKET s, const char* buf, int len, const sockaddr_in& dst4) {
    NetImpairInit();
    if (!NiImpaired()) {
        return Sendto4or6_Raw(s, buf, len, dst4);   // fast path: impairment off
    }
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_ni_mutex);
    NetImpair_PumpLocked(now);
    if (g_ni_loss > 0.0 && NiRoll() < g_ni_loss) {
        return len;   // clumsy drop: report success to caller
    }
    // Delay (+ jitter). Reorder = clumsy ood: hold this packet an extra ~10-100ms so
    // it falls behind its successors (drain is due-ordered => out-of-order delivery).
    int jit = g_ni_jitter > 0
        ? (int)(NiRng() % (2u * (unsigned)g_ni_jitter + 1)) - g_ni_jitter : 0;
    int d = g_ni_delay + jit; if (d < 0) d = 0;
    if (g_ni_reorder > 0.0 && NiRoll() < g_ni_reorder) {
        d += 10 + (int)(NiRng() % 90);   // ~1-10 packet-slots of hold
    }
    // Duplicate = clumsy duplicate: emit the packet twice.
    int copies = (g_ni_dup > 0.0 && NiRoll() < g_ni_dup) ? 2 : 1;
    if (d == 0 && copies == 1) {
        return Sendto4or6_Raw(s, buf, len, dst4);   // nothing to hold/dup -> send now
    }
    for (int i = 0; i < copies; ++i) {
        g_ni_queue.push_back(DelayedSend{
            std::vector<char>(buf, buf + len), dst4, s, now + (uint64_t)d });
    }
    return len;   // queued -- report success to caller
}

int SendtoStorage(SOCKET s, const char* buf, int len, const sockaddr_storage& dst) {
    if (dst.ss_family == AF_INET6) {
        // Native v6 -- send directly (NOT via the v4-mapped wrapper).
        int r = sendto(s, buf, len, 0,
                       reinterpret_cast<const sockaddr*>(&dst), sizeof(sockaddr_in6));
        LogFamilyErr(r, true);
        return r;
    }
    // v4 family: reuse the v4-mapped/bare logic.
    return Sendto4or6(s, buf, len, *reinterpret_cast<const sockaddr_in*>(&dst));
}

} // namespace fm2k
