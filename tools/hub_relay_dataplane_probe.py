#!/usr/bin/env python3
"""Verify the REAL hub's 0xCF match-relay data plane (started with
--test-relay-session), which relay_stub.py's docstring asks be re-checked
against hub.py at least once where `websockets` is installed."""
import socket, sys

TOKEN = "aabbccddeeff00112233445566778899"
SID = bytes.fromhex(TOKEN)
RELAY = ("127.0.0.1", 7712)


def mk():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    s.settimeout(3.0)
    return s


def wrap(p):
    return bytes([0xCF, 0x01]) + SID + p


a, b = mk(), mk()
print(f"peerA={a.getsockname()} peerB={b.getsockname()} relay={RELAY}")

# A -> relay claims slot 1; B -> relay claims slot 2 and its packet routes to A.
a.sendto(wrap(b"HELLO_FROM_A"), RELAY)
b.sendto(wrap(b"HELLO_FROM_B"), RELAY)
data, src = a.recvfrom(2048)
assert data[:2] == bytes([0xCF, 0x01]) and data[2:18] == SID, f"bad envelope {data[:18].hex()}"
assert data[18:] == b"HELLO_FROM_B", data[18:]
print(f"PASS 1: B->A routed, envelope intact, payload={data[18:]!r} from {src}")

# Reverse direction.
a.sendto(wrap(b"SECOND_FROM_A"), RELAY)
data, src = b.recvfrom(2048)
assert data[18:] == b"SECOND_FROM_A", data[18:]
print(f"PASS 2: A->B routed, payload={data[18:]!r}")

# Third source for the same session must be dropped.
c = mk()
c.sendto(wrap(b"INTRUDER"), RELAY)
try:
    d, _ = a.recvfrom(2048)
    print(f"FAIL 3: third-source packet leaked: {d[18:]!r}")
    sys.exit(1)
except socket.timeout:
    print("PASS 3: third unique source dropped (2-slot routing enforced)")

# 0xCD UDP STUN reflection on the lobby port.
s = mk()
s.sendto(bytes([0xCD, 0x01]) + b"probeuser", ("127.0.0.1", 7711))
d, _ = s.recvfrom(2048)
assert d[0] == 0xCD and d[1] == 0x02, d[:2].hex()
ip = ".".join(str(x) for x in d[2:6])
port = int.from_bytes(d[6:8], "big")
print(f"PASS 4: 0xCD STUN reflected {ip}:{port} (mine={s.getsockname()[1]})")
print("ALL PASS -- real hub 0xCF data plane agrees with relay_stub.py")
