"""Shared helpers for the /v1/report tests: two linked players (client seeds 0x02 slot 0, 0x04 slot 1, as in the C++
vectors), a game table with a no-worker row and a worker row, receipt builders and the signed call."""
from __future__ import annotations

import base64
import functools
import hashlib
import json
import os
import secrets

import games as G
import receipts as R
import tickets as T
from conftest import NOW, b64

CA, CB, CX = bytes([2]) * 32, bytes([4]) * 32, bytes([9]) * 32
GAME, BUILD, WBUILD = "wonderfulworld", "943e826d7b675e52", "cdc1f90561600ea9"
SECRET = "0f" * 32
TSV = ("game_id\tname\tengine\tbuild_id\trounds_to_win_legal\tfps\tmin_round_frames\tdouble_ko\tinput_width\t"
       "max_match_frames\tworker\tconfirmed\n"
       f"{GAME}\tWonderful World\tFM2K\t{BUILD}\t1,2,3\t100\t300\t0\t2\t180000\tno\tno\n"
       f"{GAME}\tWonderful World\tFM2K\t{WBUILD}\t1,2,3\t100\t300\t0\t2\t180000\tyes\tno\n")


def setup(h, tmp_path):
    h.cfg.stats_id_secret = SECRET
    h.cfg.export_dir = str(tmp_path / "export")
    G.import_rows(h.community.db, G.parse_tsv(TSV), NOW)
    h.app.state.discord.users["third-code"] = "222222222222222222"
    a = h.link(CA, name="Alice", code="good-code").json()
    b = h.link(CB, name="Bob", code="other-code").json()
    return base64.b64decode(a["ticket"]), base64.b64decode(b["ticket"]), a, b


def key(n=0):
    return R.session_key(bytes([0x11 + n % 200]) * 16, b"\x22" * 16, b"\xa0" * 32, b"\xb0" * 32)


@functools.lru_cache(maxsize=None)
def chains(k, mi, frame):
    c = R.InputChains(k, mi)
    for f in range(frame + 1):
        c.feed(f, (f * 7) & 0x7FF, (f * 13 + 5) & 0x7FF)
    return c.joint, (c.slot[0], c.slot[1])


def mk(kind, signer, frame, w=(0, 0), winner=0, salt=0, n=0, mi=3, build=BUILD, started=NOW + 60, zero=False):
    k = key(n)
    joint, slots = chains(k, mi, frame)
    sh = bytes(32) if zero else hashlib.sha256(b"SIM" + T.le(frame, 4) + T.le(salt, 4)).digest()
    r = R.Receipt(k, mi, GAME.encode(), build.encode(), kind, signer, started, 2, frame, sh, joint, slots, tuple(w),
                  winner)
    return R.sign_receipt(r, (CA, CB)[signer])


def signed_as(seed, receipt_bytes):
    """Re-sign a receipt's fields under another key (self-play, anonymous peer)."""
    _, _, at = R.parse_receipt(receipt_bytes)
    f = receipt_bytes[:at]
    return f + T.sign(seed, T.TAG_RESULT, f)


def body(*rs):
    why, v = R.make_list(list(rs))
    assert why == R.OK, why
    return R.encode_list(v)


def report(h, seed, ticket, b, path="/v1/report"):
    nonce = secrets.token_bytes(16)
    msg = T.call_message(b"2dfm.org", b"POST", ("/community" + path).encode(), T.sha256(b), h.clock.t, nonce,
                         T.ticket_id(ticket))
    return h.post(path, content=b, headers={"X-PC-Ticket": b64(ticket), "X-PC-Time": str(h.clock.t),
                                            "X-PC-Nonce": b64(nonce), "X-PC-Sig": b64(T.sign(seed, b"", msg)),
                                            "Content-Type": "application/octet-stream"})


def started_pair(**kw):
    return [mk(R.STARTED, 0, 120, **kw), mk(R.STARTED, 1, 120, **kw)]


def export_lines(tmp_path):
    p = tmp_path / "export" / "results.jsonl"
    if not os.path.exists(p):
        return []
    return [json.loads(x) for x in p.read_text().splitlines()]


def latest(tmp_path):
    out = {}
    for row in export_lines(tmp_path):
        out[row["match_id"]] = row
    return out
