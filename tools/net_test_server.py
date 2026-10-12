#!/usr/bin/env python3
"""Local TLS test server for the launcher's HTTPS / WebSocket client.

Exercises the server-side behaviours the real hub and GitHub rarely show on
demand: chunked bodies, redirects, a WebSocket server that pings, fragments a
message, sends binary, and closes with a code. Standard library only.

Usage (from WSL; the Windows selftest reaches it via localhost forwarding):
    python3 -I tools/net_test_server.py --dir <scratch dir> [--port 8443] [--plain]
It writes <dir>/ca.pem (a throwaway CA) and serves TLS with a leaf cert for
"localhost" signed by it. Point the client at it with FM2K_CA_FILE=<ca.pem>:
    FM2K_CA_FILE=... fm2k_net_selftest.exe get https://localhost:8443/redirect
    FM2K_CA_FILE=... fm2k_net_selftest.exe ws  wss://localhost:8443/ws hello

Endpoints:
    /cl        Content-Length body "content-length-ok"
    /chunked   chunked body with an extension and a trailer
    /redirect  302 -> /chunked
    /loop      302 -> /loop (redirect limit)
    /ws        WebSocket: ping, fragmented text, binary, echo; "close-me" -> close 4000
"""
import argparse
import asyncio
import base64
import hashlib
import os
import ssl
import struct
import subprocess

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def make_certs(d):
    ca_key, ca = os.path.join(d, "ca.key"), os.path.join(d, "ca.pem")
    key, csr, crt = (os.path.join(d, n) for n in ("leaf.key", "leaf.csr", "leaf.pem"))
    ext = os.path.join(d, "leaf.ext")
    run = lambda *a: subprocess.run(a, check=True, capture_output=True)
    run("openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
        "-nodes", "-keyout", ca_key, "-out", ca, "-days", "2", "-subj", "/CN=FM2K test CA",
        "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign")
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", csr,
        "-subj", "/CN=localhost")
    with open(ext, "w") as f:
        f.write("subjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\n")
    run("openssl", "x509", "-req", "-in", csr, "-CA", ca, "-CAkey", ca_key, "-CAcreateserial",
        "-out", crt, "-days", "2", "-extfile", ext)
    return crt, key, ca


def frame(opcode, payload=b"", fin=True):
    b0 = (0x80 if fin else 0) | opcode
    n = len(payload)
    if n < 126:
        hdr = struct.pack("!BB", b0, n)
    elif n < 65536:
        hdr = struct.pack("!BBH", b0, 126, n)
    else:
        hdr = struct.pack("!BBQ", b0, 127, n)
    return hdr + payload


async def read_frame(reader):
    b0, b1 = await reader.readexactly(2)
    n = b1 & 0x7F
    if n == 126:
        (n,) = struct.unpack("!H", await reader.readexactly(2))
    elif n == 127:
        (n,) = struct.unpack("!Q", await reader.readexactly(8))
    if not (b1 & 0x80):
        raise ValueError("client frame not masked")
    key = await reader.readexactly(4)
    data = bytearray(await reader.readexactly(n))
    for i in range(n):
        data[i] ^= key[i & 3]
    return b0 & 0x80, b0 & 0x0F, bytes(data)


async def websocket(reader, writer, headers):
    key = headers.get("sec-websocket-key", "")
    accept = base64.b64encode(hashlib.sha1(key.encode() + GUID).digest()).decode()
    writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                  "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
    # Frames right behind the 101, in the same write: ping, fragmented text, binary.
    writer.write(frame(0x9, b"srv-ping") + frame(0x1, b"frag", fin=False) +
                 frame(0x9, b"mid") + frame(0x0, b"mented") + frame(0x2, bytes(range(200))))
    await writer.drain()
    pongs = 0
    while True:
        fin, op, data = await read_frame(reader)
        if op == 0xA:
            pongs += 1
            print("ws: pong %d (%r)" % (pongs, data))
        elif op == 0x9:
            writer.write(frame(0xA, data))
        elif op == 0x8:
            print("ws: client close", data[:2].hex() if data else "")
            writer.write(frame(0x8, data[:2] if data else b""))
            await writer.drain()
            return
        elif op in (0x1, 0x2):
            print("ws: message %r" % data[:80])
            if data == b"close-me":
                writer.write(frame(0x8, struct.pack("!H", 4000) + b"bye"))
                await writer.drain()
                await asyncio.sleep(0.5)
                return
            writer.write(frame(op, b"echo:" + data))
        await writer.drain()


async def handle(reader, writer):
    try:
        head = await reader.readuntil(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        method, path, _ = lines[0].split(" ", 2)
        headers = {}
        for l in lines[1:]:
            if ":" in l:
                k, v = l.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        print("http:", method, path)
        if path == "/ws" and headers.get("upgrade", "").lower() == "websocket":
            await websocket(reader, writer, headers)
        elif path == "/cl":
            body = b"content-length-ok"
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
        elif path == "/chunked":
            writer.write(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                         b"7;ext=1\r\nchunked\r\n4\r\n-ok-\r\n0\r\nX-T: 1\r\n\r\n")
        elif path == "/redirect":
            writer.write(b"HTTP/1.1 302 Found\r\nLocation: /chunked\r\nContent-Length: 0\r\n\r\n")
        elif path == "/loop":
            writer.write(b"HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n")
        else:
            writer.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
        await writer.drain()
    except Exception as e:  # noqa: BLE001 - test server, log and drop
        print("conn error:", repr(e))
    finally:
        writer.close()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--plain", action="store_true", help="serve http/ws without TLS")
    a = ap.parse_args()
    os.makedirs(a.dir, exist_ok=True)
    crt, key, ca = make_certs(a.dir)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(crt, key)
    srv = await asyncio.start_server(handle, "0.0.0.0", a.port, ssl=None if a.plain else ctx)
    print("serving %s on :%d, CA at %s" % ("http/ws" if a.plain else "https/wss", a.port, ca),
          flush=True)
    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
