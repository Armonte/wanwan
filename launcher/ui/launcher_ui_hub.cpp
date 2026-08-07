// launcher_ui_hub.cpp -- LauncherUI hub/lobby panel + match polling + hub helpers. Split from FM2K_LauncherUI.cpp. NOTE: RenderHubPanel is large; flagged for follow-up factoring.
#include "FM2K_Integration.h"
#include "launcher_ui_hub_internal.h"
#include "launcher_ui_hubstate.h"  // LauncherUI::HubState full def
#include "launcher_ui_internal.h"  // shared persistence helpers (namespace lui)
#include "FM2K_HubClient.h"
#include "FM2K_PortMapper.h"  // UPnP port mapper member of LauncherUI (Phase 1)
#include "FM2K_DiscordAuth.h"
#include "FM2K_Locale.h"
#include "FM2K_Updater.h"
#include "version_local.h"
#include "auto_upload_secret.h"
#include "FM2K_UploadQueue.h"
#include "FM2KHook/src/ui/input_binder.h"
#include "FM2KHook/src/ui/shared_mem.h"
#include "FM2KHook/src/util/pii_scrub.h"
#include "FM2K_GameIni.h"
#include "FM2K_DDrawRedirect.h"
#include "FM2K_CncDDraw.h"
#include "FM2K_Utf8Path.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wininet.h>
#include <shellapi.h>  // Shell_NotifyIcon for challenge toast
#include <shobjidl.h>  // IFileOpenDialog (modern native folder picker)
#include <iostream>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>
#include <array>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>
#include "vendored/imgui/imgui.h"
#include "imgui_internal.h"
#include "vendored/GekkoNet/GekkoLib/include/gekkonet.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <memory>
#include <unordered_map>
#include <unordered_set>

using namespace lui;  // shared persistence helpers (launcher_ui_internal.h)



bool HubPreflightPunch(uint16_t local_port,
                              const std::string& peer_ip,
                              uint16_t peer_port,
                              const std::string& match_token_hex,
                              int timeout_ms)
{
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return false;

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in laddr{};
    laddr.sin_family = AF_INET;
    laddr.sin_port   = htons(local_port);
    laddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, reinterpret_cast<sockaddr*>(&laddr), sizeof(laddr)) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Preflight: bind() to %d failed err=%d", (int)local_port,
            WSAGetLastError());
        closesocket(s);
        return false;
    }

    DWORD recv_timeout = 100;  // 100ms recvfrom poll
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&recv_timeout), sizeof(recv_timeout));

    sockaddr_in paddr{};
    paddr.sin_family = AF_INET;
    paddr.sin_port   = htons(peer_port);
    if (inet_pton(AF_INET, peer_ip.c_str(), &paddr.sin_addr) != 1) {
        closesocket(s);
        return false;
    }

    // Decode 32-hex match token to 16 binary bytes (matches hub.py + nat_traversal).
    uint8_t token[16] = {};
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
    };
    for (size_t i = 0; i + 1 < match_token_hex.size() && i / 2 < 16; i += 2) {
        int hi = nib(match_token_hex[i]);
        int lo = nib(match_token_hex[i + 1]);
        if (hi < 0 || lo < 0) break;
        token[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
    }

    // CTRL_PUNCH packet: 0xCD 0x10 [16-byte token] -- matches
    // FM2KHook/src/netplay/nat_traversal.cpp wire format.
    uint8_t pkt[2 + 16];
    pkt[0] = 0xCD;
    pkt[1] = 0x10;
    std::memcpy(pkt + 2, token, 16);

    const auto start    = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeout_ms);
    auto next_send      = start;
    int  sends_done     = 0;
    bool peer_seen      = false;

    while (std::chrono::steady_clock::now() < deadline) {
        auto now = std::chrono::steady_clock::now();
        if (!peer_seen && now >= next_send && sends_done < 30) {
            sendto(s, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
                   reinterpret_cast<const sockaddr*>(&paddr), sizeof(paddr));
            sends_done++;
            next_send = now + std::chrono::milliseconds(10);
        }

        uint8_t buf[1024];
        sockaddr_in from{};
        int from_len = sizeof(from);
        int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n >= 18 && buf[0] == 0xCD && buf[1] == 0x10 &&
            std::memcmp(buf + 2, token, 16) == 0) {
            peer_seen = true;
            // Send a few more punches so the peer also confirms us
            // before we drop the socket. NAT mapping persists through
            // close on cone NATs, but the peer needs one good packet
            // arriving from us to flip its own preflight to "done".
            for (int i = 0; i < 3; ++i) {
                sendto(s, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
                       reinterpret_cast<const sockaddr*>(&paddr), sizeof(paddr));
                Sleep(5);
            }
            break;
        }
        // recvfrom returned WSAETIMEDOUT or other err -- loop and try
        // again until deadline.
    }

    closesocket(s);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Preflight: %s in %lldms (%d sends, peer %s:%u, port=%d)",
        peer_seen ? "PEER REACHED" : "TIMED OUT",
        (long long)elapsed, sends_done, peer_ip.c_str(),
        (unsigned)peer_port, (int)local_port);
    return peer_seen;
}

// Primary LAN IPv4 of this machine (see FM2K_Integration.h). UDP-connect a
// throwaway socket toward a public addr -- connect() on a datagram socket sends
// nothing, it just makes the OS pick the source interface it would route
// through -- then getsockname() reads that source back. RFC1918-gated so we
// only ever advertise a real private LAN address as the same-LAN candidate.
std::string fm2k::LocalLanIp() {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "";
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(53);
    ::inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);  // route hint only; no traffic
    std::string out;
    if (::connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == 0) {
        sockaddr_in local{};
        int len = static_cast<int>(sizeof(local));
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            uint32_t h = ntohl(local.sin_addr.s_addr);
            uint8_t a = (h >> 24) & 0xFF, b = (h >> 16) & 0xFF;
            bool priv = (a == 10) || (a == 172 && b >= 16 && b <= 31) ||
                        (a == 192 && b == 168);
            if (priv) {
                char ip[INET_ADDRSTRLEN] = {};
                ::inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
                out = ip;
            }
        }
    }
    ::closesocket(s);
    return out;
}

// IPv6 counterpart of LocalLanIp: discover the GLOBAL-unicast v6 address the OS
// would source for outbound to a global dest (the connect()+getsockname() trick
// auto-handles privacy/temporary address selection). A native global v6 IS the
// routable address -- no STUN/NAT involved -- so this is our cleanest CGNAT
// bypass candidate when both peers have v6. Filter to 2000::/3 (which already
// excludes fe80:: link-local, fc00:: ULA, ::1 loopback and :: unspecified) and
// drop the flaky transition prefixes (Teredo 2001:0::/32, 6to4 2002::/16).
// Returns a bracketless "2xxx::..." or empty. NOTE: mingw's <ws2ipdef.h> has no
// IN6_IS_ADDR_GLOBAL macro -- the by-hand prefix test below is the right call.
std::string fm2k::LocalLanIpV6() {
    SOCKET s = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "";
    sockaddr_in6 dst{};
    dst.sin6_family = AF_INET6;
    dst.sin6_port   = htons(53);
    ::inet_pton(AF_INET6, "2001:4860:4860::8888", &dst.sin6_addr);  // route hint only
    std::string out;
    if (::connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == 0) {
        sockaddr_in6 local{};
        int len = static_cast<int>(sizeof(local));
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            const uint8_t* b = local.sin6_addr.s6_addr;
            bool global = (b[0] & 0xE0) == 0x20;  // 2000::/3 native global unicast
            bool teredo = (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00);
            bool sixto4 = (b[0] == 0x20 && b[1] == 0x02);
            if (global && !teredo && !sixto4) {
                char ip[INET6_ADDRSTRLEN] = {};
                ::inet_ntop(AF_INET6, &local.sin6_addr, ip, sizeof(ip));
                out = ip;
            }
        }
    }
    ::closesocket(s);
    return out;
}

// Pre-match launcher STUN + NAT classification (Phase 2a). Binds the UDP
// port the game will reuse, then sends TWO 0xCD 0x01 probes from the SAME
// socket:
//   (A) to hub:hub_udp_port  (the primary STUN port, 7711) -- the hub
//       records this user's *external* NAT mapping (user.udp_addr) BEFORE
//       any match_start fires. Without this, hub's peer_dict() ships the
//       launcher-reported local port -- which only matches reality on
//       port-preserving NATs, and silently breaks for everyone else
//       (Comcast CGNAT, Spanish ISPs that randomize ports, etc.).
//   (B) to hub:hub_udp_port+3 (the classification port, 7714) -- a PURE
//       REFLECTOR on the hub that echoes the observed source but never
//       touches user state.
// Both probes go to the SAME hub IP but DIFFERENT ports. Comparing the two
// reflected external ports gives the RFC-4787 mapping behavior:
//   port_a == port_b -> endpoint/port-independent mapping  -> "cone".
//   port_a != port_b -> a new external port per destination -> "symmetric".
//   no acks          -> "blocked" ; only one ack            -> "unknown".
//
// KNOWN LIMITATION (D7, accepted): same-IP-different-port catches
// PORT-dependent symmetric NATs but NOT a purely address-dependent one
// (which assigns a new port only per destination *IP*, not per port). That
// needs two different hub IPs, which we don't have. The miss is benign:
// such a NAT classifies as "cone" here, Phase 3 may try direct first, and
// punch + peer-learning + relay still back it up.
//
// The two probes are sent BACK-TO-BACK and both acks are collected under a
// SINGLE ~1s deadline (not two serial 800ms waits) to keep the UI hitch
// small. Closes the socket on return -- the game's hook re-binds the same
// local port shortly after, and most cone NATs preserve the
// (local_port -> ext_port) mapping for >=30s of inactivity.
//
// Returns the classification (also surfaced to the hub as udp_addr.nat_type).
fm2k::NatClassifyResult fm2k::LauncherStunClassify(uint16_t local_port,
                                                   const std::string& hub_host,
                                                   uint16_t hub_udp_port,
                                                   const std::string& user_id)
{
    fm2k::NatClassifyResult out;  // defaults to "unknown", ports 0
    if (user_id.empty()) return out;
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return out;
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in laddr{};
    laddr.sin_family = AF_INET;
    laddr.sin_port   = htons(local_port);
    laddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, reinterpret_cast<sockaddr*>(&laddr), sizeof(laddr)) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "STUN: bind() to %u failed err=%d (skipping pre-match STUN)",
            (unsigned)local_port, WSAGetLastError());
        closesocket(s);
        return out;
    }

    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(hub_host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "STUN: getaddrinfo('%s') failed", hub_host.c_str());
        closesocket(s);
        return out;
    }
    sockaddr_in haddr_a = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    freeaddrinfo(res);
    sockaddr_in haddr_b = haddr_a;            // same IP...
    haddr_a.sin_port = htons(hub_udp_port);    // ...port 7711 (primary STUN)
    haddr_b.sin_port = htons(hub_udp_port + 3);// ...port 7714 (classification)

    constexpr size_t kUserIdLen = 24;
    uint8_t pkt[2 + kUserIdLen] = { 0xCD, 0x01 };
    size_t n = user_id.size();
    if (n > kUserIdLen) n = kUserIdLen;
    std::memcpy(pkt + 2, user_id.data(), n);

    // Fire both probes back-to-back so the hub answers them in parallel and
    // we wait once, not twice. The primary (A) is the one whose ack the hub
    // also uses to register user.udp_addr.
    int sent_a = sendto(s, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
                        reinterpret_cast<const sockaddr*>(&haddr_a), sizeof(haddr_a));
    int sent_b = sendto(s, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
                        reinterpret_cast<const sockaddr*>(&haddr_b), sizeof(haddr_b));
    if (sent_a != (int)sizeof(pkt) || sent_b != (int)sizeof(pkt)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "STUN: sendto failed err=%d (a=%d b=%d)", WSAGetLastError(),
            sent_a, sent_b);
    }

    // Retransmit-and-collect for BOTH acks. We can't tell which ack is which
    // by the packet alone (both are 0xCD 0x02 ip port from the hub), so we
    // disambiguate by the recvfrom *source port*: an ack from hub_udp_port
    // answers probe A, one from hub_udp_port+3 answers probe B.
    //
    // A single send + a single recv timeout used to mis-classify lossy,
    // high-RTT links (e.g. Latin-American / mobile carrier NAT to a US hub)
    // as "blocked": one dropped probe or ack, or one recvfrom slice timeout,
    // ended the whole wait. Now we re-send both probes on a cadence and keep
    // collecting until the overall (adaptive) deadline -- a recvfrom slice
    // timeout is a RETRY, never a stop. "blocked" therefore means zero acks
    // across the full retransmit window, not a single unlucky drop.
    constexpr uint32_t kBaseBudgetMs     = 2500;  // overall budget before first ack
    constexpr uint32_t kResendMs         = 250;   // re-send both probes this often
    constexpr DWORD    kRecvSliceMs      = 200;   // per-recvfrom wake so resends fire
    constexpr uint32_t kSecondAckFloorMs = 600;   // min window for the 2nd ack
    bool have_a = false, have_b = false;
    uint16_t ext_a = 0, ext_b = 0;
    char ip_a_str[INET_ADDRSTRLEN] = {}, ip_b_str[INET_ADDRSTRLEN] = {};
    const uint32_t start  = static_cast<uint32_t>(SDL_GetTicks());
    uint32_t deadline     = start + kBaseBudgetMs;   // extended on first ack
    uint32_t next_send_ms = start + kResendMs;       // initial send already fired
    uint32_t first_ack_ms = 0;
    int      resend_passes = 0;
    while (!(have_a && have_b)) {
        uint32_t now = static_cast<uint32_t>(SDL_GetTicks());
        if (now >= deadline) break;
        // Re-send both probes on the cadence (cheap: 2 tiny packets) so a
        // dropped probe/ack gets another chance within the budget.
        if (now >= next_send_ms) {
            sendto(s, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
                   reinterpret_cast<const sockaddr*>(&haddr_a), sizeof(haddr_a));
            sendto(s, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
                   reinterpret_cast<const sockaddr*>(&haddr_b), sizeof(haddr_b));
            next_send_ms = now + kResendMs;
            ++resend_passes;
        }
        uint32_t remaining = deadline - now;
        DWORD recv_to = (remaining < kRecvSliceMs) ? (DWORD)remaining : kRecvSliceMs;
        if (recv_to == 0) recv_to = 1;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&recv_to), sizeof(recv_to));
        uint8_t buf[64];
        sockaddr_in from{};
        int from_len = sizeof(from);
        int got = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                           reinterpret_cast<sockaddr*>(&from), &from_len);
        if (got < 8 || buf[0] != 0xCD || buf[1] != 0x02) {
            // Slice timeout (got <= 0) or junk: do NOT stop -- keep
            // retransmitting and collecting until the overall deadline. This
            // is the crux of the false-"blocked" fix.
            continue;
        }
        bool none_before = !have_a && !have_b;
        uint32_t ip_be = 0;
        std::memcpy(&ip_be, buf + 2, 4);
        uint16_t port_be = 0;
        std::memcpy(&port_be, buf + 6, 2);
        uint16_t reflected = ntohs(port_be);
        in_addr ia{}; ia.s_addr = ip_be;
        char ip_str[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &ia, ip_str, sizeof(ip_str));
        uint16_t src_port = ntohs(from.sin_port);
        if (src_port == hub_udp_port && !have_a) {
            have_a = true; ext_a = reflected;
            std::memcpy(ip_a_str, ip_str, sizeof(ip_a_str));
        } else if (src_port == (uint16_t)(hub_udp_port + 3) && !have_b) {
            have_b = true; ext_b = reflected;
            std::memcpy(ip_b_str, ip_str, sizeof(ip_b_str));
        }
        // Any other source port: ignore (not one of our two probes).
        // On the FIRST ack, give the second ack a fair window on a lossy /
        // high-RTT link: extend the deadline to 3x the measured round-trip,
        // floored at kSecondAckFloorMs. UDP demonstrably works at this point,
        // so we must not snap to "unknown" just because the 2nd ack is slow.
        if (none_before && (have_a || have_b)) {
            first_ack_ms = now;
            uint32_t rtt   = now - start;
            uint32_t extra = rtt * 3;
            if (extra < kSecondAckFloorMs) extra = kSecondAckFloorMs;
            uint32_t ext_deadline = now + extra;
            if (ext_deadline > deadline) deadline = ext_deadline;
        }
    }

    out.port_a = ext_a;
    out.port_b = ext_b;
    if (have_a && have_b) {
        out.nat_type = (ext_a == ext_b) ? "cone" : "symmetric";
    } else if (!have_a && !have_b) {
        out.nat_type = "blocked";  // zero acks across the full retransmit window
    } else {
        out.nat_type = "unknown";  // only one side answered -- inconclusive
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "NAT classify: %s (port_a=%u port_b=%u, refl_ip=%s, local=%u, "
        "resends=%d, first_ack=%ums, user=%s)",
        out.nat_type.c_str(), (unsigned)ext_a, (unsigned)ext_b,
        have_a ? ip_a_str : (have_b ? ip_b_str : "?"),
        (unsigned)local_port, resend_passes,
        first_ack_ms ? (unsigned)(first_ack_ms - start) : 0u,
        user_id.c_str());
    if (!have_a) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "STUN: no ack from primary %s:%u across the retransmit window "
            "(%d resends, hub records mapping anyway)",
            hub_host.c_str(), (unsigned)hub_udp_port, resend_passes);
    }

    closesocket(s);
    return out;
}

// Read the spawned game's hook log and extract the most recent
// "GameHash: manifest" block. Used by the hash-mismatch popup so the
// user sees their per-file inventory (filename | size | content_hash)
// without having to dig through the full hook log themselves.
//
// The hook log normally lives at <game_dir>/logs/FM2K_P{idx}_Debug.log.
// When the game is installed under C:\Program Files\ and the hook
// process isn't elevated, Windows VirtualStore redirects the write
// to %LOCALAPPDATA%\VirtualStore\<original_path>. We probe both
// locations and pick whichever has the freshest "manifest" content.
// Returns a friendly error string (still safe to display) if neither
// path contains a usable log.
std::string ExtractGameHashManifest(const std::filesystem::path& exe_path,
                                           int player_index) {
    if (exe_path.empty()) return "(no game selected)";
    const std::string base =
        std::string("FM2K_P") + std::to_string(player_index + 1) + "_Debug.log";
    const std::filesystem::path canonical_log =
        exe_path.parent_path() / "logs" / base;

    auto read_log = [](const std::filesystem::path& p) -> std::string {
        FILE* f = _wfopen(p.wstring().c_str(), L"rb");
        if (!f) return {};
        std::vector<char> buf;
        char chunk[8192];
        while (size_t n = std::fread(chunk, 1, sizeof(chunk), f)) {
            buf.insert(buf.end(), chunk, chunk + n);
        }
        std::fclose(f);
        return std::string(buf.begin(), buf.end());
    };

    // VirtualStore mirror path: %LOCALAPPDATA%\VirtualStore\<rest>
    // where <rest> is the original full path WITHOUT the drive letter.
    // Win32 paints LocalAppData\VirtualStore over the install dir
    // for unprivileged writes to Program Files / Windows / etc.
    std::filesystem::path virtualstore_log;
    {
        wchar_t lad[MAX_PATH] = {0};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH) > 0) {
            // Strip drive prefix ("C:\foo\bar" -> "foo\bar"). On a
            // standard Windows install drive letter is always 2 chars
            // ("C:") followed by a separator; relative_path() drops both.
            std::filesystem::path rel = canonical_log.relative_path();
            // relative_path() of "C:\Program Files\..." gives "Program
            // Files\..." which is exactly what VirtualStore wants.
            virtualstore_log =
                std::filesystem::path(lad) / "VirtualStore" / rel;
        }
    }

    // Read both candidates; prefer the one that actually contains a
    // manifest marker (in case both exist but only one was written
    // this session -- e.g. an old non-virtualized log lingering).
    std::string text = read_log(canonical_log);
    if (text.empty() ||
        (text.find("local manifest follows") == std::string::npos &&
         text.find("GameHash: manifest") == std::string::npos))
    {
        if (!virtualstore_log.empty()) {
            std::string vs = read_log(virtualstore_log);
            if (!vs.empty()) {
                text = std::move(vs);
            }
        }
    }
    if (text.empty()) {
        return std::string("(couldn't open ") +
               fm2k::utf8path::FilenameUtf8(canonical_log) +
               " from either the game folder or VirtualStore -- "
               "is the launcher running with the wrong privileges?)";
    }

    // Find the LAST manifest dump. Two possible markers, in order of
    // preference:
    //   1. "local manifest follows"  -- emitted by netplay.cpp on the
    //      actual hash-mismatch path. Always present when this popup
    //      fires from a current build, and includes the offending
    //      hashes both peers traded inline above the dump.
    //   2. "GameHash: manifest"      -- emitted by game_hash.cpp once
    //      at boot, INFO level. Fallback for cases where we open the
    //      log on a session that didn't actually hit a mismatch yet,
    //      or older hook builds before the per-mismatch dump landed.
    size_t pos = text.rfind("local manifest follows");
    if (pos == std::string::npos) {
        pos = text.rfind("GameHash: manifest");
    }
    if (pos == std::string::npos) {
        return "(log present, but no manifest dump found -- older hook "
               "build, or the log was rotated. Open the log in a text "
               "editor and search for 'manifest'.)";
    }
    // Walk back to start of the line.
    size_t line_start = pos;
    while (line_start > 0 && text[line_start - 1] != '\n') --line_start;
    // Take the next ~120 lines max.
    std::string out;
    size_t cursor = line_start;
    int lines = 0;
    while (cursor < text.size() && lines < 120) {
        size_t nl = text.find('\n', cursor);
        if (nl == std::string::npos) nl = text.size();
        out.append(text, cursor, nl - cursor);
        out.push_back('\n');
        cursor = nl + 1;
        ++lines;
    }
    return out;
}

int FindInstalledGameForRoom(const std::vector<FM2K::FM2KGameInfo>& games,
                                    const std::string& room_id) {
    if (room_id.empty()) return -1;
    auto lower = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    const std::string target = lower(room_id);
    for (size_t i = 0; i < games.size(); ++i) {
        // Wide-construct so JP-named exes give us their actual UTF-8
        // bytes back from stem(), not CP1252 mangle. Otherwise rooms
        // never match games with non-ASCII filenames.
        std::filesystem::path exe(fm2k::utf8path::Utf8ToWide(games[i].exe_path));
        const std::string stem = lower(fm2k::utf8path::StemUtf8(exe));
        if (stem == target) return (int)i;
        if (stem.size() > target.size() && stem.compare(0, target.size(), target) == 0) {
            unsigned char next = static_cast<unsigned char>(stem[target.size()]);
            if (!std::isalpha(next)) return (int)i;
        }
    }
    return -1;
}

// Settings → Hub Server… window. Lets the user point the launcher at a
// custom hub (their own hub.py instance, a friend's box, etc.) without
// cluttering the main Hub panel for casual users who just want the
// default hub.2dfm.org.
void LauncherUI::RenderHubServerBody() {
    if (!hub_host_initialized_) {
        hub_host_initialized_ = true;
        const char* env_h = std::getenv("FM2K_HUB_HOST");
        const char* def   = (env_h && env_h[0]) ? env_h : "hub.2dfm.org";
        std::snprintf(hub_host_, sizeof(hub_host_), "%s", def);
    }
    ImGui::PushItemWidth(280);
    ImGui::InputText(T("netcfg_host"), hub_host_, sizeof(hub_host_));
    ImGui::PopItemWidth();
    ImGui::TextWrapped(
        "Hub server hostname or IP. Default hub.2dfm.org for public play. "
        "Use 127.0.0.1 (or localhost) when running your own hub.py on the same "
        "machine -- NAT routers rarely hairpin so the public DNS won't loop back. "
        "Takes effect on next Connect.");
    // Update Channel toggle was moved to the top-level Settings menu --
    // 2 clicks instead of 4 to flip. See RenderMenuBar.
}

void LauncherUI::RenderHubServerWindow() {
    if (!ImGui::Begin("Hub Server", &show_hub_server_, ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }
    RenderHubServerBody();
    ImGui::End();
}

// Settings → Recent Matches… (#49). Read-only scoreboard fed by
// HubClient::RequestRecentMatches → K::RecentMatchesReceived. Refreshed
// on Connected and after every match_result so the just-finished match
// shows up at the top. The Refresh button forces an out-of-band fetch
// for users who left the launcher open between hub deployments.
void LauncherUI::RenderRecentMatchesBody() {
    auto& hs = *hub_state_;

    // Unique-ID suffix -- when the Recent Matches and Live Matches
    // panels are both visible the bare T("btn_refresh") collides on
    // ImGui's hashed-by-label widget IDs and clicking one fires the
    // other's callback (FlippySpatula's bug). PushID isolates them.
    ImGui::PushID("recent_matches_refresh");
    if (ImGui::Button(T("btn_refresh"))) {
        if (hs.client.IsConnected()) hs.client.RequestRecentMatches(50);
    }
    ImGui::PopID();
    ImGui::SameLine();
    ImGui::TextDisabled("%u %s", (unsigned)hs.recent_matches.size(),
                        T("label_matches"));
    ImGui::Separator();

    if (!hs.recent_matches_loaded) {
        ImGui::TextDisabled("%s", T("status_loading"));
    } else if (hs.recent_matches.empty()) {
        ImGui::TextDisabled("%s", T("status_no_matches"));
    } else if (ImGui::BeginTable("##recent_matches", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 360))) {
        ImGui::TableSetupColumn(T("col_when"),    ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn(T("col_game"),    ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn(T("col_p1"));
        ImGui::TableSetupColumn(T("col_p2"));
        ImGui::TableSetupColumn(T("col_stage"),   ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn(T("col_winner"),  ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        const ImVec4 kWin (0.4f, 0.95f, 0.4f, 1.0f);
        const ImVec4 kLoss(0.95f, 0.4f, 0.4f, 1.0f);
        const ImVec4 kDraw(0.7f, 0.7f, 0.7f, 1.0f);

        for (const auto& m : hs.recent_matches) {
            ImGui::TableNextRow();

            // Localised time. finished_at is unix seconds; format with
            // strftime so it respects the system locale's date separator.
            ImGui::TableSetColumnIndex(0);
            char tbuf[64] = "";
            if (m.finished_at > 0.0) {
                std::time_t tt = static_cast<std::time_t>(m.finished_at);
                std::tm lt{};
                if (localtime_s(&lt, &tt) == 0) {
                    std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", &lt);
                }
            }
            ImGui::TextUnformatted(tbuf[0] ? tbuf : "--");

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(m.game_id.empty() ? "--" : m.game_id.c_str());

            // P1 / P2 nick + char. Char fallback chain: wire-baked name
            // (from MatchResult) wins; else local KGT lookup; else
            // id-only stub. -1 char_id renders just the nick (the row
            // simply lacks the char info -- happens for older matches
            // committed before stage/char baking landed).
            auto draw_player = [&](const std::string& id, const std::string& nick,
                                   int char_id, const std::string& char_name) {
                std::string suffix;
                if (!char_name.empty()) {
                    suffix = " (" + char_name + ")";
                } else if (char_id >= 0 && on_resolve_char_name) {
                    std::string n = on_resolve_char_name(m.game_id,
                                                         (uint32_t)char_id);
                    if (!n.empty()) suffix = " (" + n + ")";
                    else            suffix = " (#" + std::to_string(char_id) + ")";
                }
                if (!hs.my_id.empty() && id == hs.my_id) {
                    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.3f, 1.0f),
                                       "%s%s", nick.c_str(), suffix.c_str());
                } else {
                    ImGui::Text("%s%s", nick.c_str(), suffix.c_str());
                }
            };
            ImGui::TableSetColumnIndex(2);
            draw_player(m.p1_id, m.p1_nick, m.p1_char_id, m.p1_char_name);
            ImGui::TableSetColumnIndex(3);
            draw_player(m.p2_id, m.p2_nick, m.p2_char_id, m.p2_char_name);

            // Stage column. Same fallback chain as the live-matches
            // panel: wire-baked > local KGT lookup > id-only > "--".
            ImGui::TableSetColumnIndex(4);
            if (!m.stage_name.empty()) {
                ImGui::TextUnformatted(m.stage_name.c_str());
            } else if (m.stage_id >= 0) {
                std::string n;
                if (on_resolve_stage_name) {
                    n = on_resolve_stage_name(m.game_id,
                                              (uint32_t)m.stage_id);
                }
                if (!n.empty()) ImGui::TextUnformatted(n.c_str());
                else            ImGui::Text("Stage #%d", m.stage_id);
            } else {
                ImGui::TextDisabled("--");
            }

            // Winner column: from local user's perspective when they're
            // in the match (Win / Loss); pure side label otherwise.
            ImGui::TableSetColumnIndex(5);
            if (m.winner_id.empty()) {
                ImGui::TextColored(kDraw, "%s", T("outcome_draw"));
            } else if (!hs.my_id.empty() && (hs.my_id == m.p1_id || hs.my_id == m.p2_id)) {
                const bool i_won = (m.winner_id == hs.my_id);
                ImGui::TextColored(i_won ? kWin : kLoss, "%s",
                                   i_won ? T("outcome_win") : T("outcome_loss"));
            } else {
                const bool p1_won = (m.winner_id == m.p1_id);
                ImGui::TextUnformatted(p1_won ? "P1" : "P2");
            }
        }
        ImGui::EndTable();
    }
}

void LauncherUI::RenderRecentMatchesWindow() {
    if (!ImGui::Begin(T("menu_recent_matches"), &show_recent_matches_)) {
        ImGui::End();
        return;
    }
    RenderRecentMatchesBody();
    ImGui::End();
}

// Live-matches lobby panel. Fed by MatchInProgressStarted/Updated/Ended
// broadcasts + the CurrentMatchesReceived snapshot at connect time. Char
// + stage cells call fm2k::FormatCharLabel / FormatStageLabel which fall
// back to "Char #N" / "Stage #N" when the local launcher doesn't have
// the game installed AND the sender didn't bake the name into the
// payload (older client).
void LauncherUI::RenderInProgressMatchesBody() {
    auto& hs = *hub_state_;

    if (ImGui::CollapsingHeader(T("hub_live_matches_header"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("live_matches_refresh");
        if (ImGui::SmallButton(T("btn_refresh"))) {
            if (hs.client.IsConnected()) hs.client.RequestCurrentMatches();
        }
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::TextDisabled("%u %s", (unsigned)hs.current_matches.size(),
                            T("label_matches"));

        if (!hs.current_matches_loaded) {
            ImGui::TextDisabled("%s", T("status_loading"));
            return;
        }
        if (hs.current_matches.empty()) {
            ImGui::TextDisabled("%s", T("status_no_matches"));
            return;
        }

        // Sort newest-first on every render -- list size stays small
        // (handful of in-flight matches max) so an in-place sort is
        // cheaper than maintaining a sorted insert in the dispatcher.
        std::sort(hs.current_matches.begin(), hs.current_matches.end(),
                  [](const auto& a, const auto& b) {
                      return a.started_at > b.started_at;
                  });

        if (ImGui::BeginTable("##live_matches", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                ImVec2(0, 200))) {
            ImGui::TableSetupColumn(T("col_game"),
                ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn(T("col_p1"));
            ImGui::TableSetupColumn(T("col_p2"));
            ImGui::TableSetupColumn(T("col_stage"),
                ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn(T("col_duration"),
                ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            const double now = (double)std::time(nullptr);
            for (const auto& m : hs.current_matches) {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(m.game_id.empty() ? "--" : m.game_id.c_str());

                auto fmt_char = [&](const std::string& nick,
                                    const std::string& id,
                                    int char_id,
                                    const std::string& char_name) {
                    // Fallback chain:
                    //   1. wire-baked name   "瑞希君 (#3)"  (sender had it)
                    //   2. local KGT lookup   ("game installed locally")
                    //   3. id-only stub      "Char #3"
                    //   4. unreported (CSS)  "(selecting…)"  dim italic
                    if (char_id < 0 && char_name.empty()) {
                        // CSS state: no chars locked in yet. Render the
                        // nick normally (so the player is still visible)
                        // and tag the char slot dim so the row reads as
                        // "in CSS" at a glance.
                        const ImVec4 self(0.95f, 0.85f, 0.3f, 1.0f);
                        const bool is_self = (!hs.my_id.empty() && id == hs.my_id);
                        if (is_self) {
                            ImGui::TextColored(self, "%s", nick.c_str());
                        } else {
                            ImGui::TextUnformatted(nick.c_str());
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("-- %s", T("status_in_css"));
                        return;
                    }
                    std::string label;
                    if (!char_name.empty()) {
                        label = char_name + " (#" +
                                std::to_string(char_id) + ")";
                    } else if (on_resolve_char_name) {
                        std::string n = on_resolve_char_name(
                            m.game_id, (uint32_t)char_id);
                        label = n.empty()
                            ? ("Char #" + std::to_string(char_id))
                            : (n + " (#" + std::to_string(char_id) + ")");
                    } else {
                        label = "Char #" + std::to_string(char_id);
                    }
                    if (!hs.my_id.empty() && id == hs.my_id) {
                        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.3f, 1.0f),
                                           "%s -- %s", nick.c_str(),
                                           label.c_str());
                    } else {
                        ImGui::Text("%s -- %s", nick.c_str(), label.c_str());
                    }
                };

                ImGui::TableSetColumnIndex(1);
                fmt_char(m.p1_nick, m.p1_id, m.p1_char_id, m.p1_char_name);
                ImGui::TableSetColumnIndex(2);
                fmt_char(m.p2_nick, m.p2_id, m.p2_char_id, m.p2_char_name);

                ImGui::TableSetColumnIndex(3);
                if (!m.stage_name.empty()) {
                    ImGui::Text("%s (#%d)", m.stage_name.c_str(),
                                m.stage_id);
                } else if (m.stage_id >= 0) {
                    // Best-effort local resolve via the launcher's
                    // FindKgtByGameId callback. Empty string means the
                    // game isn't installed locally -- fall back to
                    // id-only stub.
                    std::string n;
                    if (on_resolve_stage_name) {
                        n = on_resolve_stage_name(m.game_id,
                                                  (uint32_t)m.stage_id);
                    }
                    if (!n.empty()) {
                        ImGui::Text("%s (#%d)", n.c_str(), m.stage_id);
                    } else {
                        ImGui::Text("Stage #%d", m.stage_id);
                    }
                } else {
                    // No stage reported yet -- same CSS-state visualization
                    // as the char cells.
                    ImGui::TextDisabled("%s", T("status_in_css"));
                }

                ImGui::TableSetColumnIndex(4);
                if (m.started_at > 0.0 && now > m.started_at) {
                    int secs = (int)(now - m.started_at);
                    int mins = secs / 60;
                    secs %= 60;
                    ImGui::Text("%d:%02d", mins, secs);
                } else {
                    ImGui::TextDisabled("--");
                }
            }
            ImGui::EndTable();
        }
    }
}
