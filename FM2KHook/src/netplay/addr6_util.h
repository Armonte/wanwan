#pragma once
// IPv4/IPv6 address helpers -- the SINGLE owner of the dual-stack-socket
// concern. The rest of the netcode now holds peer/source addresses as
// sockaddr_storage and formats / sends / compares them through here, so no
// family logic leaks into the callers.
//
// The shared UDP socket is opened AF_INET6 dual-stack (IPV6_V6ONLY=0) when the
// platform allows it; v4 peers ride v4-mapped (::ffff:a.b.c.d) on that SAME
// handle. On hosts where IPv6 is disabled we soft-fall-back to a legacy
// AF_INET socket. Which one won is recorded here (Addr_SetSocketV6).
//
// LOAD-BEARING INVARIANT: GekkoNet drops any packet whose source string is not
// a byte-exact match of the actor string. Addr_ActorString is the ONE formatter
// used both to register the actor and to stamp inbound packets; its v4 output is
// byte-identical to the legacy inet_ntoa("%s:%d") form, so v4 sync is preserved.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>

namespace fm2k {

// Record whether the shared socket is AF_INET6 dual-stack (true) or a legacy
// AF_INET fallback (false). Called once from NetSocket_Init before any send or
// receive runs -- write-once, then read concurrently (safe).
void Addr_SetSocketV6(bool is_v6);

// Low-level v4-mapped helpers (pure).
bool Addr_IsV4Mapped(const sockaddr_in6& a6);
sockaddr_in Addr_Unmap4(const sockaddr_in6& a6);   // precondition: v4-mapped
sockaddr_in6 Addr_Map4(const sockaddr_in& a4);     // -> ::ffff:<a4>

// Normalize a recvfrom source (received into a sockaddr_in6) to a real-family
// sockaddr_storage: v4-mapped -> AF_INET; native v6 -> AF_INET6; on the v4-
// fallback socket the kernel wrote a sockaddr_in. Always succeeds (v6 peers
// are supported now).
void Addr_NormalizeRecv(const sockaddr_in6& from6, sockaddr_storage& out);

// The GekkoNet actor string for a peer/source: v4 -> "a.b.c.d:port" (BYTE-
// IDENTICAL to the legacy inet_ntoa form), v6 -> "[2xxx::...]:port". The ONE
// formatter used at every actor-register + recv-stamp site.
std::string Addr_ActorString(const sockaddr_storage& sa);

// Family-aware equality (family + address bytes + port). Used by peer-learning.
bool Addr_Equal(const sockaddr_storage& a, const sockaddr_storage& b);

// Family + address bytes only (IGNORES port). Used by the carrier-NAT
// mid-session port-rebind adoption: same IP + new port = the peer's NAT
// remapped the flow, not a different host.
bool Addr_SameIp(const sockaddr_storage& a, const sockaddr_storage& b);

// True if sa is a usable (non-empty) peer address (port != 0).
bool Addr_HasPort(const sockaddr_storage& sa);

// sendto a v4 destination on socket s: bare on the v4-fallback socket, or
// v4-mapped on the dual-stack socket. Returns the raw sendto() result; logs
// only the family-mismatch errors that would otherwise be silent.
int Sendto4or6(SOCKET s, const char* buf, int len, const sockaddr_in& dst4);

// Test-only (FM2K_NET_DELAY_MS) periodic flush of the impairment delay queue.
// MUST be called from a periodic worker (the MM-timer) so a CSS/battle-entry
// stall where both peers wait on each other's delayed packet can't wedge. No-op
// when impairment is off.
void NetImpair_Pump();

// sendto an arbitrary-family destination (sockaddr_storage): native v6 for an
// AF_INET6 dst, else routed through Sendto4or6 (v4-mapped / bare). Used by the
// direct gameplay egress now that the peer can be v6.
int SendtoStorage(SOCKET s, const char* buf, int len, const sockaddr_storage& dst);

} // namespace fm2k
