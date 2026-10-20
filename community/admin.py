"""Admin CLI for the community service (run on the server, from community/, with the service's env).

    python3 admin.py gen-ticket-key --gen 1              # the ONLINE key; prints its public key for the KEYSET
    python3 admin.py keyset-body --serial 1 --root a --ticket-key 1:<pubhex> --out keyset_1.body
    python3 admin.py verify-keyset keyset_1.pckeys       # check a signed KEYSET against the two roots
    python3 admin.py install-keyset keyset_1.pckeys      # verify, then copy into COMMUNITY_KEYSET_DIR
    python3 admin.py ban "Name#1234" --reason "..."      # account bans ride the deny list at once
    python3 admin.py unban "Name#1234"
    python3 admin.py revoke-ticket --gen 1 --serial 42   # deny one ticket serial until it expires
    python3 admin.py show "Name#1234"                    # or a 32-hex account id
    python3 admin.py stats

The KEYSET body is UNSIGNED: carry it to the machine that holds the root keys and run sign_keyset.py there. This
tool never reads a root private key.
"""
from __future__ import annotations

import argparse
import json
import os
import secrets
import shutil
import sys
import time

import tickets as T
from config import Config
from service import Community, ServiceError, write_ticket_key

DAY = 86400


def _entry(spec: str, role: int, issued: int, valid_until: int) -> T.KeysetEntry:
    """GEN:PUBHEX[:NOT_BEFORE:NOT_AFTER] for a ticket key, GEN:PUBHEX:NOT_BEFORE:NOT_AFTER:game1,game2 for a worker."""
    parts = spec.split(":", 4)  # a game_id may itself hold ":"
    try:
        gen, pub = int(parts[0]), bytes.fromhex(parts[1])
        nb = int(parts[2]) if len(parts) > 2 and parts[2] else issued
        na = int(parts[3]) if len(parts) > 3 and parts[3] else valid_until + 30 * DAY
    except (IndexError, ValueError):
        sys.exit(f"bad key spec {spec!r}")
    scope = sorted(set(parts[4].split(","))) if role == T.ROLE_WORKER and len(parts) > 4 else []
    if role == T.ROLE_WORKER and not scope:
        sys.exit(f"worker key {spec!r} needs a game scope (GEN:PUB:NB:NA:game1,game2)")
    if len(pub) != 32:
        sys.exit(f"{spec!r}: the public key is 32 bytes (64 hex digits)")
    return T.KeysetEntry(gen, pub, nb, na, role, scope)


def build_keyset_body(a) -> bytes:
    issued = int(time.time()) if a.issued == "now" else int(a.issued)
    valid_until = issued + int(a.valid_days * DAY)
    entries = [_entry(s, T.ROLE_TICKET, issued, valid_until) for s in a.ticket_key or []]
    entries += [_entry(s, T.ROLE_WORKER, issued, valid_until) for s in a.worker_key or []]
    if not entries:
        sys.exit("a KEYSET needs at least one --ticket-key")
    entries.sort(key=lambda e: e.key_gen)
    rr = {"none": 0xFF, "a": 0, "b": 1}[a.revoke_root]
    k = T.Keyset(a.serial, issued, valid_until, a.min_key_gen, {"a": 0, "b": 1}[a.root], entries,
                 sorted(set(a.revoke or [])), rr)
    body = T.keyset_body(k)
    # Dry run: sign with throwaway roots and parse, so a body the client parser refuses never reaches the owner.
    tmp = (secrets.token_bytes(32), secrets.token_bytes(32))
    r, _ = T.parse_keyset(body + T.sign(tmp[k.root_id], T.TAG_KEYSET, body),
                          (T.seed_public(tmp[0]), T.seed_public(tmp[1])), 0, 0)
    if r != T.OK:
        sys.exit(f"this KEYSET would be refused by clients: {r}")
    for e in entries:
        if e.pub in (T.ROOT_A_PUB, T.ROOT_B_PUB):
            sys.exit(f"key gen {e.key_gen} is a root public key")
    return body


def describe(ks: T.Keyset) -> str:
    when = lambda t: time.strftime("%Y-%m-%d %H:%M:%SZ", time.gmtime(t))  # noqa: E731
    lines = [f"KEYSET serial {ks.serial}, signed by root {'ab'[ks.root_id]}, issued {when(ks.issued)}, "
             f"valid until {when(ks.valid_until)}, min_key_gen {ks.min_key_gen}"]
    for e in ks.keys:
        role = "ticket" if e.role == T.ROLE_TICKET else "worker " + ",".join(e.scope)
        lines.append(f"  gen {e.key_gen} {role} {e.pub.hex()} {when(e.not_before)} .. {when(e.not_after)}")
    if ks.revoked:
        lines.append("  revoked gens: " + ", ".join(map(str, ks.revoked)))
    if ks.revoked_root != 0xFF:
        lines.append(f"  REVOKES root {'ab'[ks.revoked_root]}")
    return "\n".join(lines)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen-ticket-key", help="generate the online ticket key into a 0600 file")
    g.add_argument("--gen", type=int, required=True, help="the key generation (1 for the first key)")
    g.add_argument("--out", help="default: COMMUNITY_TICKET_KEY")
    kb = sub.add_parser("keyset-body", help="build an UNSIGNED KEYSET body for sign_keyset.py")
    kb.add_argument("--serial", type=int, required=True)
    kb.add_argument("--issued", default="now", help="unix seconds or 'now'")
    kb.add_argument("--valid-days", type=float, default=45)
    kb.add_argument("--min-key-gen", type=int, default=0)
    kb.add_argument("--root", choices=("a", "b"), required=True, help="the root that will sign it")
    kb.add_argument("--ticket-key", action="append", help="GEN:PUBHEX[:NOT_BEFORE:NOT_AFTER] (repeatable)")
    kb.add_argument("--worker-key", action="append", help="GEN:PUBHEX:NOT_BEFORE:NOT_AFTER:game1,game2")
    kb.add_argument("--revoke", type=int, action="append", help="a revoked key generation (repeatable)")
    kb.add_argument("--revoke-root", choices=("none", "a", "b"), default="none")
    kb.add_argument("--out", required=True)
    for name in ("verify-keyset", "install-keyset"):
        s = sub.add_parser(name)
        s.add_argument("file")
    for name in ("ban", "unban", "show"):
        s = sub.add_parser(name)
        s.add_argument("who", help="Name#1234 or a 32-hex account id")
        if name == "ban":
            s.add_argument("--reason", default="")
    rt = sub.add_parser("revoke-ticket")
    rt.add_argument("--gen", type=int, required=True)
    rt.add_argument("--serial", type=int, required=True)
    sub.add_parser("stats")
    sub.add_parser("deny", help="print the current deny list summary")
    a = p.parse_args(argv)
    cfg = Config.from_env()

    if a.cmd == "gen-ticket-key":
        out = a.out or cfg.ticket_key_path
        seed = secrets.token_bytes(32)
        try:
            write_ticket_key(out, a.gen, seed)
        except FileExistsError:
            sys.exit(f"{out} exists: a rotation writes a new file (pass --out) and a new KEYSET")
        pub = T.seed_public(seed).hex()
        print(f"wrote {out} (0600). Online ticket key gen {a.gen}, public key:\n{pub}\n")
        print("Put it in the next KEYSET body:\n"
              f"  python3 admin.py keyset-body --serial N --root a --ticket-key {a.gen}:{pub} --out keyset_N.body")
        return 0
    if a.cmd == "keyset-body":
        body = build_keyset_body(a)
        with open(a.out, "wb") as f:
            f.write(body)
        stem = a.out[:-5] if a.out.endswith(".body") else a.out
        print(f"wrote {a.out} ({len(body)} bytes, UNSIGNED). Sign it offline:\n"
              f"  python3 sign_keyset.py {a.out} root_a.private.pem root_b.private.pem {stem}.pckeys")
        return 0
    if a.cmd in ("verify-keyset", "install-keyset"):
        with open(a.file, "rb") as f:
            b = f.read()
        r, ks = T.parse_keyset(b, cfg.root_pubs, 0, int(time.time()), T.SERVICE_KEYSET_SKEW)
        if r == "not_yet_valid":  # a pre-signed KEYSET: check everything but the date
            r, ks = T.parse_keyset(b, cfg.root_pubs, 0, 0)
            if r == T.OK:
                print("(issued in the future: the service publishes it when it comes due)")
        if r != T.OK:
            print(f"REFUSED: {r}")
            return 1
        print(describe(ks))
        if a.cmd == "install-keyset":
            os.makedirs(cfg.keyset_dir, exist_ok=True)
            dst = os.path.join(cfg.keyset_dir, f"keyset_{ks.serial:06d}.pckeys")
            shutil.copyfile(a.file, dst)
            print(f"installed {dst}")
        return 0

    c = Community(cfg)
    if a.cmd in ("ban", "unban", "show"):
        acc = c.find_account(a.who)
        if acc is None:
            sys.exit(f"no account {a.who!r}")
        if a.cmd == "show":
            print(json.dumps({"account_id": acc["account_id"].hex(), "name": acc["name"], "tag": acc["tag"],
                              "flagged": bool(acc["flagged"]), "banned": bool(acc["banned"]),
                              "ban_reason": acc["ban_reason"], "linked": acc["linked"],
                              "last_refresh": acc["last_refresh"]}, indent=2, ensure_ascii=False))
        else:
            c.ban(acc["id"], getattr(a, "reason", ""), a.cmd == "ban")
            print(f"{a.cmd} {acc['name']}#{(acc['tag'] or 0):04d}: live on the next deny-list fetch")
        return 0
    if a.cmd == "revoke-ticket":
        print("revoked" if c.revoke_serial(a.gen, a.serial) else "no such ticket (recorded in the audit log anyway)")
        return 0
    if a.cmd == "stats":
        print(json.dumps(c.stats(), indent=2))
        return 0
    if a.cmd == "deny":
        try:
            r, d = T.parse_deny(c.deny_list(), c.ticket_key().pub)
        except ServiceError as e:
            sys.exit(f"{e.code}: {e.detail}")
        print(json.dumps({"key_gen": d["key_gen"], "list_serial": d["list_serial"], "serials": d["serials"],
                          "accounts": [x.hex() for x in d["accounts"]]}, indent=2))
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
