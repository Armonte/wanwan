#!/usr/bin/env python3
"""Summarize a tools/delay_sweep.sh run, one row per (profile, arm).

Reads each run's live_FM2K_P{1,2}_Debug.log:
  * the battle-start "Netplay: RTT ... local_delay=N" line -> chosen delay
  * [BEAT] lines -> brake %, split STARTUP (first beat) vs STEADY (the rest),
    because braking here is a match-start transient and averaging it over the
    run makes the number depend on run length (see the 2026-09-10 notes)
  * rb_avg / rb_max from [BEAT]
  * GekkoNet "DESYNC #" lines -- the only desync verdict that counts

Usage: delay_sweep_report.py <sweep_dir>
"""
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

BEAT_BRAKE = re.compile(r"brake=(\d+)/(\d+)")
KV = re.compile(r"(\w+)=([-+]?[\d.]+)")
DELAY = re.compile(r"Netplay: RTT .*?local_delay=(\d+)")
P95 = re.compile(r"Netplay: RTT .*?p9[05]=(\d+)ms")


def parse_log(path):
    beats, delays, p95s, desyncs = [], [], [], 0
    for ln in open(path, errors="replace"):
        if "[BEAT] " in ln:
            m = BEAT_BRAKE.search(ln)
            kv = dict(KV.findall(ln))
            if m and int(m.group(2)):
                beats.append((100.0 * int(m.group(1)) / int(m.group(2)),
                              float(kv.get("rb_avg", 0)),
                              int(float(kv.get("rb_max", 0)))))
        elif "Netplay: RTT " in ln:
            m = DELAY.search(ln)
            if m:
                delays.append(int(m.group(1)))
            m = P95.search(ln)
            if m:
                p95s.append(int(m.group(1)))
        if "DESYNC #" in ln:
            desyncs += 1
    return beats, delays, p95s, desyncs


def main():
    root = Path(sys.argv[1])
    rows = defaultdict(lambda: {"runs": 0, "start": [], "steady": [],
                                "rb_avg": [], "rb_max": 0, "delay": [],
                                "p95": [], "desync": 0, "nobeat": 0})
    for run in sorted(p for p in root.iterdir() if p.is_dir()):
        m = re.match(r"(ow\d+_j\d+_l[\d.]+)_(\w+)_r\d+$", run.name)
        if not m:
            continue
        key = (m.group(1), m.group(2))
        row = rows[key]
        row["runs"] += 1
        got = False
        for log in sorted(run.glob("live_FM2K_P*_Debug.log")):
            beats, delays, p95s, desyncs = parse_log(log)
            row["desync"] += desyncs
            row["delay"] += delays
            row["p95"] += p95s
            if beats:
                got = True
                row["start"].append(beats[0][0])
                row["steady"] += [b[0] for b in beats[1:]]
                row["rb_avg"] += [b[1] for b in beats]
                row["rb_max"] = max(row["rb_max"], max(b[2] for b in beats))
        if not got:
            row["nobeat"] += 1

    def mean(xs):
        return f"{statistics.mean(xs):5.1f}" if xs else "  n/a"

    print(f"{'profile':<18} {'arm':<5} {'runs':>4} {'delay':>9} {'p90':>5} "
          f"{'start%':>7} {'steady%':>8} {'rb_avg':>6} {'rb_max':>6} "
          f"{'desync':>6}")
    for (prof, arm), r in sorted(rows.items()):
        delays = sorted(set(r["delay"]))
        dstr = ",".join(map(str, delays)) if delays else "?"
        p95 = f"{statistics.median(r['p95']):.0f}" if r["p95"] else "-"
        note = f"  ({r['nobeat']} run(s) without [BEAT])" if r["nobeat"] else ""
        print(f"{prof:<18} {arm:<5} {r['runs']:>4} {dstr:>9} {p95:>5} "
              f"{mean(r['start']):>7} {mean(r['steady']):>8} "
              f"{mean(r['rb_avg']):>6} {r['rb_max']:>6} {r['desync']:>6}{note}")


if __name__ == "__main__":
    main()
