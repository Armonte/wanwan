#!/usr/bin/env python3
"""dump_sounds.py -- per-sound WAV dump + audio gate for the C++ .fpk decoder.

Given a reconstructed C++ output (.bin, built with FPK_WITH_OPUS) and the matching
original .player/.stage/.demo, this:
  - parses both with tools/kgt/fpk.py's Container,
  - dumps every codec==1 (WAV) sound's reconstructed bytes to OUTDIR/<idx>.wav,
  - validates each dumped blob is a real RIFF/WAVE (parses via fpk.wav_fmt),
  - asserts the reconstructed sound's channels AND sample-rate equal the
    ORIGINAL sound's channels/sample-rate (the audio correctness gate).

Audio is lossy (Opus), so PCM bytes differ; only the WAV structure + the
declared channels/sample-rate must be preserved.

usage: dump_sounds.py ORIGINAL OUT.bin OUTDIR
exit 0 = all WAV sounds valid + channels/rate preserved, 1 = a mismatch, 2 = usage/parse.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "kgt"))
import fpk as F  # noqa: E402


def is_valid_riff_wave(data: bytes) -> bool:
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return False
    # RIFF size field must cover the payload (allow exact 36+data accounting).
    riff_sz = F._u32(data, 4)
    if riff_sz + 8 != len(data):
        return False
    # must contain a parseable fmt chunk and a data chunk
    pos, has_fmt, has_data = 12, False, False
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        csz = F._u32(data, pos + 4)
        if cid == b"fmt ":
            has_fmt = True
        elif cid == b"data":
            has_data = True
            if pos + 8 + csz > len(data):
                return False
        pos += 8 + csz + (csz & 1)
    return has_fmt and has_data


def main(argv):
    if len(argv) != 4:
        sys.stderr.write("usage: dump_sounds.py ORIGINAL OUT.bin OUTDIR\n")
        return 2

    orig_path, out_path, outdir = argv[1], argv[2], argv[3]
    try:
        src = F.Container(open(orig_path, "rb").read())
        rc = F.Container(open(out_path, "rb").read())
    except Exception as e:
        print(f"FAIL  parse error: {e}")
        return 2

    if src.sound_n != rc.sound_n:
        print(f"FAIL  sound_n: {src.sound_n} vs {rc.sound_n}")
        return 1

    os.makedirs(outdir, exist_ok=True)
    name = os.path.basename(orig_path)

    total_wav = 0       # WAV-typed sounds in the original
    decoded_ok = 0      # dumped WAVs that are valid RIFF + ch/sr preserved
    all_ok = True

    for i, ((sh, ss), (rh, rs)) in enumerate(zip(src.sounds, rc.sounds)):
        if (ss.sound_type & 0x0F) != 1:
            continue  # non-WAV (MIDI/CDDA/stop) -- not an opus-decode candidate
        sf = F.wav_fmt(ss.data)
        if sf is None:
            continue  # original WAV-typed but not a parseable RIFF; skip
        total_wav += 1

        wav_path = os.path.join(outdir, f"{i:03d}.wav")
        with open(wav_path, "wb") as f:
            f.write(rs.data)

        rf = F.wav_fmt(rs.data)
        valid = is_valid_riff_wave(rs.data)
        if not valid or rf is None:
            all_ok = False
            print(f"  [FAIL] snd[{i}] reconstructed bytes are not a valid RIFF/WAVE "
                  f"(len={len(rs.data)})")
            continue

        sch, ssr, _sb = sf
        rch, rsr, rb = rf
        ch_ok = (sch == rch)
        sr_ok = (ssr == rsr)
        if ch_ok and sr_ok and rb == 16:
            decoded_ok += 1
            print(f"  [OK ] snd[{i}] ch={rch} sr={rsr} bits={rb}  "
                  f"orig(ch={sch} sr={ssr})  {len(rs.data)} B")
        else:
            all_ok = False
            why = []
            if not ch_ok:
                why.append(f"ch {sch}->{rch}")
            if not sr_ok:
                why.append(f"sr {ssr}->{rsr}")
            if rb != 16:
                why.append(f"bits={rb}")
            print(f"  [FAIL] snd[{i}] {', '.join(why)}")

    print(f"  {name}: decoded_ok={decoded_ok}/{total_wav} WAV sounds, "
          f"channels+rate {'preserved' if all_ok else 'MISMATCH'}")
    return 0 if all_ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
