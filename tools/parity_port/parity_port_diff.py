#!/usr/bin/env python3
"""Compare two FM2K parity dumps (FM2KPAR1, written by the port's src/parity_core.c and by the
original-side fm2k_parity.dll) tick by tick.

    python3 -I parity_port_diff.py A.fm2kpar B.fm2kpar [--port-dir DIR] [--max-bytes N]
                                   [--summary-only] [--tick T]

Reports: the timeline of each dump (game state changes), the first tick whose region hashes differ
(plus input / game-state / alloc-count mismatches, which usually explain it), a per-region summary
(first differing tick, number of differing ticks), and, when both dumps hold raw bytes for the
first differing tick (FM2K_PARITY_DUMP_FRAMES), the differing bytes with their original address and
the decomp's name: global (include/game_bss.h of the port, else docs data_name_map.tsv) and struct
member (include/kgt_types.h) for object slots, character slots, after-image trails, kgtGameState.
Exit status: 0 identical over the common ticks, 1 divergent, 2 usage/format error.
"""
import argparse
import bisect
import os
import re
import struct
import sys

MAGIC = b"FM2KPAR1"
TICK = 0x4B434954
DEFAULT_PORT = "/mnt/d/dev/fm2k-port-parity"
DOCS_MAP = "/mnt/d/dev/wanwan/docs/dev/2dfm_decomp_triage/data_name_map.tsv"


class Dump:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        h = self.f.read(8 + 8 + 16)
        if h[:8] != MAGIC:
            raise SystemExit(f"{path}: not an FM2KPAR1 dump")
        self.version, n = struct.unpack_from("<II", h, 8)
        self.side = h[16:32].split(b"\0")[0].decode()
        self.regions = []
        for _ in range(n):
            addr, size, name = struct.unpack("<II32s", self.f.read(40))
            self.regions.append((addr, size, name.split(b"\0")[0].decode()))
        self.raw_size = sum(r[1] for r in self.regions)
        self.ofs = []
        o = 0
        for r in self.regions:
            self.ofs.append(o)
            o += r[1]
        self.ticks = {}   # tick -> dict
        self.order = []
        rec = struct.Struct("<IIIII2HII")
        while True:
            b = self.f.read(rec.size)
            if len(b) < rec.size:
                break
            magic, tick, gs, ipos, flags, in0, in1, calls, aseq = rec.unpack(b)
            if magic != TICK:
                raise SystemExit(f"{path}: bad record at {self.f.tell() - rec.size}")
            hb = self.f.read(8 * n)
            if len(hb) < 8 * n:
                break
            hashes = struct.unpack(f"<{n}Q", hb)
            raw_at = None
            if flags & 1:
                raw_at = self.f.tell()
                self.f.seek(self.raw_size, 1)
                if self.f.tell() > os.path.getsize(path):
                    break
            self.ticks[tick] = dict(gs=gs, ipos=ipos, inp=(in0, in1), calls=calls, aseq=aseq,
                                    hashes=hashes, raw_at=raw_at)
            self.order.append(tick)

    def raw(self, tick, r):
        t = self.ticks[tick]
        if t["raw_at"] is None:
            return None
        self.f.seek(t["raw_at"] + self.ofs[r])
        return self.f.read(self.regions[r][1])


# ---------------------------------------------------------------- names
class Names:
    def __init__(self, port_dir):
        self.globals = []   # (addr, size, name)
        self.structs = {}   # struct -> [(ofs, name)]
        bss = os.path.join(port_dir, "include", "game_bss.h")
        if os.path.exists(bss):
            for line in open(bss, encoding="utf-8", errors="replace"):
                m = re.match(r"\s*unsigned char (\w+)\[(\d+)\];\s*/\* 0x([0-9a-f]+)", line)
                if m:
                    self.globals.append((int(m.group(3), 16), int(m.group(2)), m.group(1)))
            base = 0x4213C0
            fields = []
            for line in open(bss, encoding="utf-8", errors="replace"):
                m = re.match(r"#define BSS_OFS_(\w+)\s+0x([0-9a-f]+)\s+/\* (\w+) \+ 0x[0-9a-f]+, (\d+) bytes", line)
                if m:
                    fields.append((base + int(m.group(2), 16), int(m.group(4)), m.group(1)))
            self.fields = sorted(fields)
        else:
            self.fields = []
        self.globals.sort()
        self.gaddrs = [g[0] for g in self.globals]
        # rand seed lives in .data
        self.extra = {0x41FB1C: "guRandSeed (LIBC holdrand)"}
        self.docs = {}
        if os.path.exists(DOCS_MAP):
            for line in open(DOCS_MAP, encoding="utf-8", errors="replace"):
                p = line.rstrip("\n").split("\t")
                if len(p) >= 3 and re.fullmatch(r"[0-9a-f]{8}", p[0]):
                    nm = p[2] or p[1]
                    if nm:
                        self.docs[int(p[0], 16)] = nm
        types = os.path.join(port_dir, "include", "kgt_types.h")
        if os.path.exists(types):
            cur = None
            for line in open(types, encoding="utf-8", errors="replace"):
                m = re.match(r"struct (\w+) \{", line)
                if m:
                    cur = m.group(1)
                    self.structs[cur] = []
                    continue
                if cur and line.startswith("};"):
                    cur = None
                    continue
                if cur:
                    m = re.match(r"\s*(.+?)\s*;\s*/\* 0x([0-9a-f]{4}) \*/", line)
                    if m:
                        decl = m.group(1)
                        nm = re.findall(r"(\w+)\s*(\[[^\]]*\])?\s*$", decl)
                        name = nm[0][0] + (nm[0][1] or "") if nm else decl
                        self.structs[cur].append((int(m.group(2), 16), name))
            for k in self.structs:
                self.structs[k].sort()

    def member(self, struct_name, ofs):
        mem = self.structs.get(struct_name, [])
        best = None
        for o, n in mem:
            if o <= ofs:
                best = (o, n)
            else:
                break
        if not best:
            return f"+0x{ofs:x}"
        return f"{best[1]}+0x{ofs - best[0]:x}" if ofs != best[0] else best[1]

    def name(self, addr):
        if addr in self.extra:
            return self.extra[addr]
        if 0x4701E0 <= addr < 0x4701E0 + 1024 * 0x17E:
            i, o = divmod(addr - 0x4701E0, 0x17E)
            return f"gkgtEngineObjects[{i}].{self.member('kgtEngineObject', o)}"
        if 0x4D1D80 <= addr < 0x4D1D80 + 8 * 0xE03F:
            i, o = divmod(addr - 0x4D1D80, 0xE03F)
            if o < 0x2234:
                return f"gkgtLoadedCharacter[{i}].kgtCore.{self.member('kgt_core', o)}"
            return f"gkgtLoadedCharacter[{i}].{self.member('kgt_character_struct', o)}"
        if 0x447F80 <= addr < 0x447F80 + 100 * 0x650:
            i, o = divmod(addr - 0x447F80, 0x650)
            hdr = ["bInUse", "iPos", "pStep", "iTimer"]
            if o < 16:
                return f"gAfterImageTrails[{i}].{hdr[o // 4]}+{o % 4}"
            n, k = divmod(o - 16, 16)
            return f"gAfterImageTrails[{i}].frame[{n}].{['x', 'y', 'flags', 'pImageStep'][k // 4]}+{k % 4}"
        if 0x470020 <= addr < 0x470020 + 0x1AC:
            return f"gkgtGameState.{self.member('kgtGameState', addr - 0x470020)}"
        if 0x4CFA20 <= addr < 0x4CFA20 + 8192:
            i, o = divmod(addr - 0x4CFA20, 8)
            return f"gkgtDrawNodes[{i}].{'pObj' if o < 4 else 'pNext'}+{o % 4}"
        if 0x430240 <= addr < 0x430240 + 1024:
            i, o = divmod(addr - 0x430240, 8)
            return f"gkgtDrawLayers[{i}].{'head' if o < 4 else 'tail'}+{o % 4}"
        if 0x4280E0 <= addr < 0x4280E0 + 32768:
            p, o = divmod(addr - 0x4280E0, 4096)
            return f"giInputBuffer[{p}][{o // 4}]+{o % 4}"
        for fa, fs, fn in self.fields:
            if fa <= addr < fa + fs:
                return f"{fn}+0x{addr - fa:x}" if addr != fa else fn
        i = bisect.bisect_right(self.gaddrs, addr) - 1
        if i >= 0:
            ga, gs, gn = self.globals[i]
            if addr < ga + gs:
                s = f"{gn}+0x{addr - ga:x}" if addr != ga else gn
                d = self.docs.get(addr)
                return s + (f" ({d})" if d and d not in s else "")
        return self.docs.get(addr, "?")


def runs(diff_idx):
    """group sorted byte indices into [start, end) runs"""
    out = []
    for i in diff_idx:
        if out and i == out[-1][1]:
            out[-1][1] = i + 1
        else:
            out.append([i, i + 1])
    return out


def timeline(d):
    out = []
    last = None
    for t in d.order:
        gs = d.ticks[t]["gs"]
        if gs != last:
            out.append(f"{t}:{gs}")
            last = gs
    return " ".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--port-dir", default=DEFAULT_PORT)
    ap.add_argument("--max-bytes", type=int, default=40, help="byte runs listed per region")
    ap.add_argument("--summary-only", action="store_true")
    ap.add_argument("--tick", type=int, help="byte-diff this tick instead of the first divergent one")
    args = ap.parse_args()
    A, B = Dump(args.a), Dump(args.b)
    if [r[:2] for r in A.regions] != [r[:2] for r in B.regions]:
        print("region tables differ")
        return 2
    names = Names(args.port_dir)
    common = sorted(set(A.ticks) & set(B.ticks))
    print(f"A: {args.a} side={A.side} ticks={len(A.order)}")
    print(f"B: {args.b} side={B.side} ticks={len(B.order)}")
    print(f"A timeline (tick:game_state): {timeline(A)}")
    print(f"B timeline (tick:game_state): {timeline(B)}")
    n = len(A.regions)
    first = None
    first_meta = None
    per = [[None, 0] for _ in range(n)]
    for t in common:
        a, b = A.ticks[t], B.ticks[t]
        if first_meta is None and (a["inp"] != b["inp"] or a["gs"] != b["gs"] or a["calls"] != b["calls"]
                                   or a["aseq"] != b["aseq"]):
            first_meta = t
        bad = False
        for r in range(n):
            if a["hashes"][r] != b["hashes"][r]:
                bad = True
                if per[r][0] is None:
                    per[r][0] = t
                per[r][1] += 1
        if bad and first is None:
            first = t
    if first is None:
        print(f"\nRESULT: identical for {len(common)} ticks (0..{common[-1] if common else -1}); "
              f"all {n} regions match every tick")
        if first_meta is not None:
            print(f"  (but input/meta first differ at tick {first_meta})")
        return 0
    print(f"\nRESULT: first divergence at tick {first} "
          f"(game_state A={A.ticks[first]['gs']} B={B.ticks[first]['gs']})")
    if first_meta is not None:
        t = first_meta
        print(f"  first input/meta difference at tick {t}: "
              + " ".join(f"{k}: A={A.ticks[t][k]} B={B.ticks[t][k]}" for k in ("gs", "ipos", "inp", "calls", "aseq")
                         if A.ticks[t][k] != B.ticks[t][k]))
    print("\nper region: first differing tick, differing ticks / common ticks")
    for r in range(n):
        addr, size, nm = A.regions[r]
        f, c = per[r]
        print(f"  {nm:26s} 0x{addr:06x} {size:7d}  " + (f"first={f:<7d} {c}/{len(common)}" if f is not None else "identical"))
    if args.summary_only:
        return 1
    tick = args.tick if args.tick is not None else first
    print(f"\nbyte diff at tick {tick}:")
    if tick not in A.ticks or tick not in B.ticks or A.ticks[tick]["raw_at"] is None or B.ticks[tick]["raw_at"] is None:
        raw_a = [t for t in A.order if A.ticks[t]["raw_at"] is not None]
        print(f"  no raw bytes for tick {tick} in both dumps (A raw ticks: "
              f"{(raw_a[0], raw_a[-1]) if raw_a else 'none'}); rerun both sides with "
              f"FM2K_PARITY_DUMP_FRAMES={max(0, tick - 2)}-{tick + 2}")
        return 1
    for r in range(n):
        if A.ticks[tick]["hashes"][r] == B.ticks[tick]["hashes"][r]:
            continue
        addr, size, nm = A.regions[r]
        ra, rb = A.raw(tick, r), B.raw(tick, r)
        idx = [i for i in range(size) if ra[i] != rb[i]]
        rr = runs(idx)
        print(f"  [{nm}] {len(idx)} bytes differ in {len(rr)} runs")
        for s, e in rr[: args.max_bytes]:
            ea = addr + s
            print(f"    0x{ea:06x} +{e - s:<4d} {names.name(ea):60s} A={ra[s:e][:16].hex()} B={rb[s:e][:16].hex()}")
        if len(rr) > args.max_bytes:
            print(f"    ... {len(rr) - args.max_bytes} more runs")
    return 1


if __name__ == "__main__":
    sys.exit(main())
