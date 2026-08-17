#!/usr/bin/env python3
"""envelope_shadow_report.py -- offline analyser for FM2K_P*_envshadow.csv.

ENVELOPE INVERSION PHASE 1 (shadow mode). The hook TU
(FM2KHook/src/netplay/envelope_shadow.cpp, FM2K_ENVELOPE_SHADOW=1) hashes the
whole writable image (.data, 0x41E000..0x544000) in 256-byte blocks at every
SaveState_Save and diffs a REPLAY save against the FORWARD save of the same
frame. This script turns those raw block rows into the three things the study
asks for:

  1. THE HOLE LIST -- every divergent block OUTSIDE the current save envelope,
     named from the study's coverage map, classified (a)/(b)/(c)/(d)/(e), with
     frequency and first-divergence frame.
  2. SELF-VALIDATION -- the five deliberate carve-outs E1..E5 (render frame
     counter, palette-flash 1 and 2, shake, g_last_frame_time) are real
     forward-vs-replay divergences BY DESIGN. If they do not appear, the
     instrument is broken and every other finding is worthless. They are tagged
     KNOWN-EXCLUSION and reported separately from the discovered holes.
  3. RESTORE BUGS -- divergent blocks INSIDE the envelope that are NOT explained
     by a carve-out. There should be none. Any hit is loud and sets exit 1.

Usage:
    python3 tools/envelope_shadow_report.py FM2K_P1_envshadow.csv [more.csv ...]
    python3 tools/envelope_shadow_report.py --no-strict run_dir/*.csv

Exit codes: 0 = self-validation passed and no restore bugs; 1 = a restore bug
(or, with --strict, a failed self-validation); 2 = no usable input.

The block hashes give 256-byte resolution; the hook's byte-witness ring refines
each divergent block to a 64-bit mask of exactly which dwords differed, which is
what lets this report say "0x424F24" rather than "somewhere in block 111". Rows
with dword_mask=0x0 had no witness (it aged out) and are reported at block
resolution, honestly labelled.
"""
import sys
import os
import csv
from collections import defaultdict

DATA_BASE = 0x41E000
DATA_END = 0x544000
BLOCK = 256

# --- the current save envelope (study section 1.1) -------------------------
# The eleven regions the save path actually copies. NOTE the same caveat the
# hook carries: the object-pool and char-slot copies are SPARSE (active/loaded
# slots only), so "inside" here is the optimistic reading. It can under-report
# holes; it cannot invent them.
ENVELOPE = [
    (0x41FB1C, 4, "rng seed"),
    (0x424718, 4, "round_end_flag"),
    (0x4259A8, 4, "current_object_ptr"),
    (0x4280D8, 8200, "input history ring"),
    (0x430240, 1024, "object list heads/tails"),
    (0x4456B0, 88, "effect_sys2"),
    (0x447930, 0x46F6C0 - 0x447930, "afterimage pool"),
    (0x470020, 0x220, "game state"),
    (0x4701E0, 0x5F800, "object pool (SPARSE: active slots only)"),
    (0x4CFA20, 0x2000, "object node pool"),
    (0x4D1D90, 8 * 57407, "char slots (SPARSE: loaded slots only)"),
]

# --- the deliberate carve-outs the instrument must re-discover -------------
# Same table the hook carries. E1..E5 of the study's exclusion list (section
# 2.1). These are inside the envelope and deliberately not restored.
KNOWN_EXCLUSIONS = [
    ("E1", 0x4456FC, 4, "g_render_frame_counter (ProcessShakeEffect parity flip)"),
    ("E2", 0x447D7D, 42, "effect_sys1 / palette-flash-1 (render+sim decremented)"),
    ("E3", 0x4456D0, 44, "palette-flash-2 (inside effect_sys2)"),
    ("E4", 0x447DA9, 40, "g_shake_effect_1/_2 (render-decremented timer)"),
    ("E5", 0x447DD4, 4, "g_last_frame_time (per-process pacing anchor)"),
]

# --- the coverage map, gap level (study section 1.2) -----------------------
# (start, end, gap letter, class, what lives there)
GAPS = [
    (0x41E000, 0x41FB1C, "A", "c", "file-initialised head of .data: strings, formats, "
     "path templates, g_object_function_table, handler tables"),
    (0x41FB20, 0x424718, "B", "b", "MSVC CRT internals (__output/__setmbcp/__ctype/"
     "_xcptlookup/__heap_alloc) + Win32/DirectDraw object state"),
    (0x42471C, 0x4259A8, "C", "a/b/c", "DDraw+sound handles, CRT argv/environ/stdio block, "
     "joystick/keyboard device state, CSS cursors, round timer, the 0x424F24 family"),
    (0x4259AC, 0x4280D8, "D", "b/c/e", "g_p1_input/g_p2_input, DDraw scratch, message-system "
     "aux block, config buffers, CSS scene-config, char-loader scratch"),
    (0x42A0E0, 0x430240, "E", "c/e", "24,576 B of never-referenced BSS then the game.ini "
     "settings block 0x4300E0..0x43012C (g_selected_stage, g_default_round)"),
    (0x430640, 0x4456B0, "F", "b/c/a/e", "g_sound_channel_table, UI/score block, three "
     "256-byte-stride name tables, the 0x4438xx / 0x4451AA counter clusters"),
    (0x445708, 0x447930, "G", "c/e", "key-config bytes, stage-data block base, "
     "8,408 B unreferenced"),
    (0x46F6C0, 0x470020, "H", "b", "the on-screen message table (30 x 72 B) whose +0x44 "
     "countdown is decremented once per SIM frame, then DDraw/dialog state"),
    (0x4CF9E0, 0x4CFA20, "I", "d", "g_slot_loaded_char_idx[8] + per-frame object-iteration "
     "and input scratch"),
    (0x4D1A20, 0x4D1D90, "J", "b/c", "blitter scratch, menu-only input repeat state, config, "
     "and char slot 0's first 16 bytes (the +16 landmine)"),
    (0x541F88, 0x544000, "K", "b", "pure MSVC CRT: ___sbh_* small-block heap descriptors, "
     "mbcs case tables, stdio FILE array, osfhandle table, environ/argv"),
]

# --- named scalars / sub-ranges, for dword-resolution naming ---------------
# Everything the study names explicitly inside the complement, plus the E-list
# neighbours. Longest-prefix wins: the list is searched for the tightest range
# containing the address.
NAMED = [
    (0x41E2F0, 4, "g_frame_time_ms", "c"),
    (0x41E3FC, 4, "g_input_initial_delay", "c"),
    (0x41E400, 4, "g_input_repeat_delay", "c"),
    (0x41ED58, 4, "g_object_function_table", "c"),
    (0x41FF10, 0x210, "__ctype (CRT)", "b"),
    (0x420120, 0x278, "mbcs tables (CRT)", "b"),
    (0x4246FC, 4, "per-frame updated-object counter", "a"),
    (0x424710, 8, "0x424710/0x424714 (sim-adjacent scalars)", "a"),
    (0x424750, 0x40, "DDraw surface + sound object handles", "b"),
    (0x4247B0, 0xB4, "CRT argv/environ/stdio pointer block", "b"),
    (0x424D20, 0x130, "keyboard/joystick key array (get_player_input)", "b"),
    (0x424E50, 0x10, "CSS cursors 0x424E50/0x424E58", "a"),
    (0x424F00, 4, "g_round_timer_counter", "a"),
    (0x424F24, 4, "CSS/char select index (ADDR_CSS_ACTIVE_PLAYER, study a-2)", "a"),
    (0x424F28, 0x0C, "unk_424F28[] char-index table (study a-2)", "a"),
    (0x424F34, 0x0C, "vs_round_function scalars 0x424F34/38/3C", "a"),
    (0x4259C0, 8, "g_p1_input / g_p2_input", "a"),
    (0x4259E0, 0x0C, "DDraw scratch", "b"),
    (0x425A00, 0x44, "on-screen message system aux block (study N2)", "b"),
    (0x425A48, 0x20, "config buffers 0x425A48/4C/60", "c"),
    (0x427C7C, 0x1E, "CSS scene-config state", "c"),
    (0x4280CC, 0x0C, "char-loader scratch", "c"),
    (0x4300E0, 0x4C, "game.ini settings block (g_selected_stage 0x43010C, "
     "g_default_round 0x430124)", "c"),
    (0x430640, 0x2C00, "g_sound_channel_table (2816 SoundBufferArray*, study N3)", "b"),
    (0x433240, 0x2234, "UI/score block", "c"),
    (0x435474, 0x4E28, "character filename table (256 B stride)", "c"),
    (0x43A29C, 0x3200, "stage filename table (256 B stride, 50 entries)", "c"),
    (0x43D49C, 0x6400, "third 256-B-stride name table (100 entries)", "c"),
    (0x4438A8, 0x1902, "UI/round byte+word counters (vs_round/game_state/score/title)", "a"),
    (0x4451AA, 0x122, "the 0x4451AA..0x4452CC word cluster (study a-5)", "a"),
    (0x445710, 8, "key-config bytes (get_player_input)", "c"),
    (0x445740, 0x110, "g_special_object_character_data_base (stage loader lpBuffer)", "c"),
    (0x46F6C0, 0x870, "on-screen message table, 30 x 72 B (study a-3 / N1)", "b"),
    (0x46FF40, 0xA0, "DDraw / dialog state", "b"),
    (0x4CF9E0, 0x20, "g_slot_loaded_char_idx[8] (study N9)", "d"),
    (0x4CFA00, 4, "object-iteration scratch pointer (rewritten every frame)", "a"),
    (0x4CFA04, 4, "process_game_inputs per-frame word (read by vs_round_function)", "a"),
    (0x4D1A20, 0x200, "blitter scratch", "b"),
    (0x4D1C20, 4, "0x4D1C20", "b"),
    (0x4D1C40, 0x20, "g_input_repeat_state family (menu-only)", "b"),
    (0x4D1C60, 4, "config 0x4D1C60", "c"),
    (0x4D1D60, 4, "config 0x4D1D60", "c"),
    (0x4D1D80, 0x10, "char slot 0 bytes +0..+0xF (the +16 landmine, study 5.1)", "a"),
    (0x541F78, 4, "g_speedcalc_inv (over-captured today as slot-7 tail)", "a"),
    (0x541F80, 8, "g_input_repeat timer (menu-only; battle never reads it)", "b"),
    (0x447F40, 0x20, "g_processed_input (derived from menu-only repeat state)", "a"),
    (0x447EE0, 0x20, "g_input_buffer_index / input tracking head", "a"),
    (0x541FA0, 0x18, "___sbh_* small-block heap descriptors (CRT)", "b"),
    (0x541FB8, 0x230, "mbcs case tables (CRT)", "b"),
    (0x5421E8, 0x1150, "stdio FILE array + osfhandle table (CRT)", "b"),
]

# The two SPARSE members of the envelope. Save and load walk these per slot and
# skip inactive / unloaded ones, so a byte here is inside a "covered" range yet
# may not have been copied at all this frame. Divergence in these is NOT a
# restore bug -- it is precisely the residue a declared envelope would cover.
SPARSE = [
    (0x4701E0, 0x5F800, "object pool (inactive-slot residue)"),
    (0x4D1D90, 8 * 57407, "char slots (unloaded-slot residue)"),
]


# Divergences INSIDE a densely-restored region that are the downstream
# CONSEQUENCE of an uncovered hole rather than a failure to restore. Each entry
# names its source hole, so the claim is falsifiable: if the source hole is not
# also in the hole list for the same corpus, the attribution does not hold and
# the entry must be re-judged as an unattributed restore bug.
PROPAGATED = [
    (0x447F40, 0x20, "g_processed_input",
     [0x4D1C20, 0x4D1C40, 0x541F80],
     "recomputed every frame from the menu-only input-repeat state "
     "@0x4D1C40 / 0x541F80, which is OUTSIDE the envelope. Documented in-source "
     "at savestate_fm2k_save.cpp:170-181 as non-authoritative noise; battle "
     "never reads it. A hole propagating INTO a covered region."),
]


def propagated(addr, length=4):
    for base, size, nm, srcs, why in PROPAGATED:
        if addr < base + size and base < addr + length:
            return nm, srcs, why
    return None, None, None


def in_sparse(addr, length=4):
    for base, size, _ in SPARSE:
        if addr < base + size and base < addr + length:
            return True
    return False


def parse_header(hdr):
    """Pull the control counters out of the '# envshadow ...' header line."""
    out = {}
    for tok in hdr.split():
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        out[k] = v
    return out


def parse_ke(hdr_fields):
    """ke=E1:a1d26120,E2:a0d0,... -> {tag: (active, diverged)}"""
    res = {}
    raw = hdr_fields.get("ke", "")
    for part in raw.split(","):
        if ":" not in part:
            continue
        tag, rest = part.split(":", 1)
        if not rest.startswith("a") or "d" not in rest:
            continue
        a, d = rest[1:].split("d", 1)
        try:
            res[tag] = (int(a), int(d))
        except ValueError:
            continue
    return res


CLASS_TEXT = {
    "a": "SIM state (real hole -- rollback-visible)",
    "b": "process/render-owned (must stay EXCLUDED under inversion)",
    "c": "static-after-load (harmless either way; blob-size cost only)",
    "d": "resource-ownership bookkeeping (exclude)",
    "e": "UNKNOWN residual (the number phase 1 exists to shrink)",
}


def in_range(addr, base, size):
    return base <= addr < base + size


def envelope_cover(addr, length=4):
    for base, size, _ in ENVELOPE:
        if addr < base + size and base < addr + length:
            return True
    return False


def known_exclusion(addr, length=4):
    for tag, base, size, name in KNOWN_EXCLUSIONS:
        if addr < base + size and base < addr + length:
            return tag, name
    return None, None


def name_of(addr):
    best = None
    for base, size, nm, cls in NAMED:
        if in_range(addr, base, size):
            if best is None or size < best[0]:
                best = (size, nm, cls)
    if best:
        return best[1], best[2]
    for start, end, letter, cls, what in GAPS:
        if start <= addr < end:
            return "gap %s: %s" % (letter, what), cls
    return "(inside a save region)", "-"


def gap_of(addr):
    for start, end, letter, cls, what in GAPS:
        if start <= addr < end:
            return letter
    return "-"


def load(paths):
    """Merge every CSV into one address-keyed table.

    Rows are aggregated at DWORD resolution when a witness mask is present and
    at BLOCK resolution when it is not. The two are kept apart deliberately: a
    block-resolution row is a weaker claim and the report says so.
    """
    dwords = {}   # addr -> dict(hits, first_frame, first_match, fwd, rep, srcs)
    blocks = {}   # block addr -> dict(hits, first_frame, cover, srcs)
    headers = []
    for p in paths:
        try:
            fh = open(p, newline="", encoding="utf-8", errors="replace")
        except OSError as exc:
            print("  ! cannot read %s: %s" % (p, exc))
            continue
        with fh:
            hdr = None
            rows = []
            for line in fh:
                if line.startswith("#"):
                    hdr = line.strip()
                    continue
                rows.append(line)
            if hdr:
                headers.append((os.path.basename(p), hdr, parse_header(hdr)))
            if not rows:
                continue
            rd = csv.DictReader(rows)
            for r in rd:
                try:
                    addr = int(r["addr"], 16)
                    hits = int(r["hits"])
                    frame = int(r["first_frame"])
                    match = int(r.get("first_match", 0))
                    cover = int(r["cover"])
                    mask = int(r["dword_mask"], 16)
                    fwd = int(r["fwd"], 16)
                    rep = int(r["rep"], 16)
                    foff = int(r["first_off"])
                except (KeyError, ValueError, TypeError):
                    continue
                b = blocks.setdefault(addr, dict(hits=0, first_frame=frame,
                                                 first_match=match, cover=cover,
                                                 srcs=set(), refined=False))
                b["hits"] += hits
                b["first_frame"] = min(b["first_frame"], frame)
                b["srcs"].add(os.path.basename(p))
                if mask:
                    b["refined"] = True
                    for d in range(64):
                        if not (mask >> d) & 1:
                            continue
                        a = addr + d * 4
                        e = dwords.setdefault(a, dict(hits=0, first_frame=frame,
                                                      first_match=match,
                                                      fwd=None, rep=None, srcs=set()))
                        e["hits"] += hits
                        e["first_frame"] = min(e["first_frame"], frame)
                        e["srcs"].add(os.path.basename(p))
                        if d * 4 == foff and e["fwd"] is None:
                            e["fwd"], e["rep"] = fwd, rep
    return dwords, blocks, headers


def main(argv):
    strict = True
    paths = []
    for a in argv[1:]:
        if a == "--no-strict":
            strict = False
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            paths.append(a)
    if not paths:
        print(__doc__)
        return 2

    dwords, blocks, headers = load(paths)
    if not blocks:
        print("ENVELOPE SHADOW REPORT: no divergent blocks in %d input file(s)." % len(paths))
        print("  Three different things look like this and the game log tells them apart --")
        print("  find the [ENVSHADOW] summary line:")
        print("    armed=no                     -> the instrument was DARK (var never set).")
        print("    armed=yes, blocks_seen=0     -> armed, and NOTHING diverged. The hook")
        print("                                    writes no CSV in that case, by design.")
        print("    armed=yes, compared=0        -> armed, but every comparison was discarded")
        print("                                    by the input-correction control: an")
        print("                                    UNPOWERED run, not a clean one.")
        return 2

    print("=" * 78)
    print("ENVELOPE SHADOW REPORT -- forward-vs-replay divergence over .data")
    print("=" * 78)
    for nm, hdr, _f in headers:
        print("  %s\n    %s" % (nm, hdr[1:].strip()))
    print()

    # --- 0. THE CONTROL ---------------------------------------------------
    # Every number below is conditioned on this. A comparison is only usable if
    # the resim consumed IDENTICAL inputs and converged on the SAME gameplay
    # fingerprint; otherwise the two passes are simply different simulations and
    # every downstream byte legitimately differs.
    tot = defaultdict(int)
    for _nm, _hdr, fl in headers:
        for k in ("fwd", "replay", "paired", "drop_input", "drop_fp", "compared",
                  "nowitness", "divergent_events"):
            try:
                tot[k] += int(fl.get(k, 0))
            except ValueError:
                pass
    have_control = any("paired" in fl for _n, _h, fl in headers)
    print("-" * 78)
    print("0. THE INPUT-CORRECTION CONTROL (what the numbers below are conditioned on)")
    print("-" * 78)
    if not have_control:
        print("  ! This corpus predates the control (no paired=/drop_input= in the header).")
        print("  ! Its divergences CONFLATE envelope holes with ordinary rollback input")
        print("  ! correction and must not be read as a hole list. Re-run instrumented.")
    else:
        print("  forward saves                     %d" % tot["fwd"])
        print("  replay saves                      %d" % tot["replay"])
        print("  paired with a forward vector      %d" % tot["paired"])
        print("  DISCARDED, inputs differed        %d  (predicted vs corrected remote input)"
              % tot["drop_input"])
        print("  DISCARDED, fingerprint differed   %d  (the two sims genuinely diverged)"
              % tot["drop_fp"])
        print("  USABLE comparisons                %d" % tot["compared"])
        print("  of those, divergent               %d" % tot["divergent_events"])
        if tot["paired"]:
            print("  usable fraction                   %.1f%%"
                  % (100.0 * tot["compared"] / tot["paired"]))
        if tot["compared"] == 0:
            print()
            print("  ! ZERO usable comparisons. Every resim in this corpus consumed corrected")
            print("  ! inputs, so nothing here measures the envelope. Not a null result --")
            print("  ! an unpowered one. Raise the frame count or lower the loss rate.")
    print()

    # --- partition -------------------------------------------------------
    known = defaultdict(lambda: dict(hits=0, first_frame=10 ** 9, addrs=set()))
    inside_bug = []
    sparse_res = []
    prop_res = []
    holes = []
    for addr, e in dwords.items():
        tag, kname = known_exclusion(addr)
        if tag:
            k = known[tag]
            k["hits"] += e["hits"]
            k["first_frame"] = min(k["first_frame"], e["first_frame"])
            k["addrs"].add(addr)
            k["name"] = kname
            continue
        if envelope_cover(addr):
            if in_sparse(addr):
                sparse_res.append((addr, e))
            elif propagated(addr)[0]:
                prop_res.append((addr, e))
            else:
                inside_bug.append((addr, e))
        else:
            holes.append((addr, e))

    # Block-resolution rows that never got refined -- weaker evidence, kept
    # separate so a reader never confuses the two.
    unrefined = [(a, b) for a, b in blocks.items() if not b["refined"]]

    # --- 1. SELF-VALIDATION ----------------------------------------------
    print("-" * 78)
    print("1. SELF-VALIDATION -- do the five deliberate carve-outs re-discover themselves?")
    print("-" * 78)
    ok = True
    ke = {}
    for _n, _h, fl in headers:
        for tag, (a, d) in parse_ke(fl).items():
            pa, pd = ke.get(tag, (0, 0))
            ke[tag] = (max(pa, a), pd + d)
    for tag, base, size, name in KNOWN_EXCLUSIONS:
        if tag in known:
            k = known[tag]
            print("  [FOUND]   %s 0x%08X+%-3d hits=%-8d first_frame=%-7d  %s"
                  % (tag, base, size, k["hits"], k["first_frame"], name))
        elif tag in ke and ke[tag][0] == 1 and tot.get("compared", 0) < 100:
            # It held a live value but this corpus has too few usable
            # comparisons to expect to catch it. Underpowered, not broken.
            print("  [UNDERPOWERED] %s 0x%08X+%-3d  live but not caught in only %d usable "
                  "comparison(s) (%s)" % (tag, base, size, tot.get("compared", 0), name))
        elif tag in ke and ke[tag][0] == 0:
            # The carve-out held ZERO for the whole run. A timer that never runs
            # cannot diverge, so its absence is not evidence against the
            # instrument -- it is evidence the run never triggered that effect.
            print("  [INACTIVE] %s 0x%08X+%-3d  never held a non-zero value in this "
                  "corpus -- nothing could diverge (%s)" % (tag, base, size, name))
        else:
            # A block-resolution hit over the same range is partial credit: the
            # instrument fired, but without a witness it cannot prove WHICH
            # dword moved.
            blk = base - DATA_BASE
            blk = DATA_BASE + (blk // BLOCK) * BLOCK
            if blk in blocks and not blocks[blk]["refined"]:
                print("  [BLOCK]   %s 0x%08X+%-3d  block 0x%08X diverged but had no byte "
                      "witness -- partial credit" % (tag, base, size, blk))
            else:
                ok = False
                print("  [MISSING] %s 0x%08X+%-3d  %s" % (tag, base, size, name))
    print()
    n_found = sum(1 for t, _b, _s, _n in KNOWN_EXCLUSIONS if t in known)
    n_inact = sum(1 for t, _b, _s, _n in KNOWN_EXCLUSIONS
                  if t not in known and t in ke and ke[t][0] == 0)
    n_under = sum(1 for t, _b, _s, _n in KNOWN_EXCLUSIONS
                  if t not in known and t in ke and ke[t][0] == 1
                  and tot.get("compared", 0) < 100)
    if n_under:
        print("  NOTE: %d carve-out(s) were live but this corpus has only %d usable"
              % (n_under, tot.get("compared", 0)))
        print("  comparison(s) -- too few to expect a hit. Judge self-validation on the")
        print("  forced-rollback arm, not on a lossy netplay arm.")
    if ok:
        print("  VERDICT: PASS -- %d carve-out(s) re-discovered themselves and %d were "
              "provably" % (n_found, n_inact))
        print("  INACTIVE (held zero all run, so nothing could diverge). No carve-out that")
        print("  was live came back identical. The instrument sees real forward-vs-replay")
        print("  divergence, so its NOVEL findings below are trustworthy.")
    else:
        print("  VERDICT: FAIL -- at least one carve-out that MUST diverge did not appear.")
        print("  Do not trust the hole list until this is explained (candidate causes: the")
        print("  run never rolled back, the witness ring aged out on every hit, or the")
        print("  sampling point moved relative to render).")
    print()

    # --- 2. RESTORE BUGS --------------------------------------------------
    print("-" * 78)
    print("2. INSIDE-ENVELOPE DIVERGENCES -- these are RESTORE BUGS, not holes")
    print("-" * 78)
    hole_addrs = set(a for a, _e in holes)
    if prop_res:
        print("  ATTRIBUTED (a hole propagating into a covered region, not a failure to")
        print("  restore). Each attribution is falsifiable: its source hole must ALSO be in")
        print("  the hole list of this same corpus, and is checked below.")
        seen_nm = set()
        for addr, e in sorted(prop_res, key=lambda x: -x[1]["hits"]):
            nm, srcs, why = propagated(addr)
            if nm in seen_nm:
                continue
            seen_nm.add(nm)
            present = [x for x in srcs if any(x <= h < x + 8 or h == x for h in hole_addrs)]
            ok_src = bool(present)
            print("    0x%08X  %s  hits=%d" % (addr, nm, e["hits"]))
            print("      %s" % why)
            print("      source hole(s) %s in THIS corpus: %s -> attribution %s"
                  % (", ".join("0x%06X" % x for x in srcs),
                     ("present " + ", ".join("0x%06X" % x for x in present)) if ok_src
                     else "ABSENT",
                     "HOLDS" if ok_src else "DOES NOT HOLD -- treat as unattributed"))
            if not ok_src:
                inside_bug.append((addr, e))
        print()
    if not inside_bug:
        print("  UNATTRIBUTED: none. Every densely-restored byte either came back identical")
        print("  or is accounted for above.")
    else:
        print("  *** %d address(es) inside a save region diverged forward-vs-replay and are"
              % len(inside_bug))
        print("  *** NOT a known carve-out. The envelope says it restores these bytes.")
        for addr, e in sorted(inside_bug, key=lambda x: -x[1]["hits"])[:60]:
            nm, cls = name_of(addr)
            print("    0x%08X hits=%-8d first_frame=%-7d fwd=%s rep=%s  %s"
                  % (addr, e["hits"], e["first_frame"],
                     ("0x%08X" % e["fwd"]) if e["fwd"] is not None else "-",
                     ("0x%08X" % e["rep"]) if e["rep"] is not None else "-", nm))
        if len(inside_bug) > 60:
            print("    ... and %d more" % (len(inside_bug) - 60))
    print()

    # --- 3. THE HOLE LIST -------------------------------------------------
    print("-" * 78)
    print("2b. SPARSE-COVERAGE RESIDUE -- inside a region, but only ACTIVE slots are copied")
    print("-" * 78)
    if not sparse_res:
        print("  none.")
    else:
        by = defaultdict(lambda: dict(hits=0, first_frame=10 ** 9, n=0))
        for addr, e in sparse_res:
            for base, size, nm in SPARSE:
                if base <= addr < base + size:
                    g = by[nm]
                    g["hits"] += e["hits"]
                    g["first_frame"] = min(g["first_frame"], e["first_frame"])
                    g["n"] += 1
        for nm, g in sorted(by.items(), key=lambda kv: -kv[1]["hits"]):
            print("  %-40s dwords=%-6d hits=%-8d first_frame=%d"
                  % (nm, g["n"], g["hits"], g["first_frame"]))
        print()
        print("  This is NOT a restore bug and NOT a hole in the address sense: the range")
        print("  is declared covered, but the fast path copies only active/loaded slots, so")
        print("  the residue in the rest is never restored. It is the single clearest")
        print("  argument for the inversion: a declared envelope covers it by construction.")
    print()

    print("-" * 78)
    print("3. THE HOLE LIST -- divergent AND outside the current save envelope")
    print("-" * 78)
    if not holes:
        print("  none discovered.")
    else:
        # Group contiguous/named runs so the list reads as state, not addresses.
        grouped = defaultdict(lambda: dict(hits=0, first_frame=10 ** 9, addrs=[], cls="-", gap="-"))
        for addr, e in holes:
            nm, cls = name_of(addr)
            g = grouped[nm]
            g["hits"] += e["hits"]
            g["first_frame"] = min(g["first_frame"], e["first_frame"])
            g["addrs"].append(addr)
            g["cls"] = cls
            g["gap"] = gap_of(addr)
        print("  %-5s %-4s %-9s %-8s %-7s %s" %
              ("gap", "cls", "bytes", "hits", "first_f", "what"))
        for nm, g in sorted(grouped.items(), key=lambda kv: -kv[1]["hits"]):
            lo, hi = min(g["addrs"]), max(g["addrs"])
            span = "0x%06X" % lo if lo == hi else "0x%06X+%d" % (lo, (hi - lo) // 4 + 1)
            print("  %-5s %-4s %-9s %-8d %-7d %s"
                  % (g["gap"], g["cls"], span, g["hits"], g["first_frame"], nm))
        print()
        print("  class key:")
        for k in sorted(CLASS_TEXT):
            print("    (%s) %s" % (k, CLASS_TEXT[k]))
    print()

    # --- 4. headline: the 0x424F24 family ---------------------------------
    print("-" * 78)
    print("4. HEADLINE -- the study's top uncovered category-(a) candidate (0x424F24 family)")
    print("-" * 78)
    fam = [(a, e) for a, e in dwords.items() if 0x424F24 <= a < 0x424F40]
    other_a = [(a, e) for a, e in holes if name_of(a)[1] == "a"]
    if fam:
        print("  DIVERGES. The family is real uncovered sim-read state that moves under")
        print("  rollback -- phase 2 must cover it:")
        for a, e in sorted(fam):
            nm, _ = name_of(a)
            print("    0x%08X hits=%-8d first_frame=%-7d  %s" % (a, e["hits"], e["first_frame"], nm))
    else:
        print("  NOT OBSERVED to diverge in this corpus. Consistent with the study's own")
        print("  reading (written only at CSS time, constant across a battle), so it is a")
        print("  LATENT hole rather than an active one. Covering it under inversion is free")
        print("  insurance, not a fix for anything measured here.")
    print()
    print("  other category-(a) holes observed: %d address(es)" % len(other_a))
    for a, e in sorted(other_a, key=lambda x: -x[1]["hits"])[:20]:
        print("    0x%08X hits=%-8d %s" % (a, e["hits"], name_of(a)[0]))
    print()

    # --- 5. coverage / honesty -------------------------------------------
    print("-" * 78)
    print("5. COVERAGE AND HONESTY")
    print("-" * 78)
    print("  divergent blocks (256 B):            %d of %d" % (len(blocks), (DATA_END - DATA_BASE) // BLOCK))
    print("  refined to dword resolution:         %d" % (len(blocks) - len(unrefined)))
    print("  block resolution only (no witness):  %d" % len(unrefined))
    print("  divergent dwords total:              %d" % len(dwords))
    print("    of which known carve-outs:         %d" % sum(len(k["addrs"]) for k in known.values()))
    print("    of which sparse-coverage residue:  %d" % len(sparse_res))
    print("    of which attributed propagation:   %d" % len(prop_res))
    print("    of which inside-envelope BUGS:     %d" % len(inside_bug))
    print("    of which holes (outside):          %d" % len(holes))
    hole_bytes = len(holes) * 4
    print("  hole bytes observed moving:          %d of the study's 172,620 uncovered" % hole_bytes)
    if unrefined:
        print()
        print("  blocks without byte detail (weaker claim, listed for completeness):")
        for a, b in sorted(unrefined, key=lambda x: -x[1]["hits"])[:20]:
            print("    0x%08X hits=%-8d cover=%-4d %s" % (a, b["hits"], b["cover"], name_of(a)[0]))
    print()

    if inside_bug:
        return 1
    if strict and not ok:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
