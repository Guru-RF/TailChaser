#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""Test of the callsign tag (docs/TAG.md).

    python3 tests/tag.py ./tagtest ./tailchaser-duplex

1. The message: the symbol signs of known messages (tagtest signs).
2. The waveform: tagtest's WAV, read back by the decoder below.
3. A real tailchaser-duplex with [tag] on: what it transmits carries the
   repeater's own callsign as "local" after a station on its input, and the
   talker's callsign as "network" after an over from the talkgroup — read
   from the quiet tail of each transmission.

The decoder here is the format written out once more, in plain Python, for a
recording whose frame start is known (the PTT track). Standard library only.
"""
import math
import os
import subprocess
import sys
import tempfile
import time
import wave
import array

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_simplex import (Checks, Node, Recording, Reflector, Sa818, crypto_selftest,  # noqa: E402
                         free_port, voice, write_wav)
from e2e_repeater import overs_at  # noqa: E402

FS = 16000
NSYM, CPS, SPC, NSYNC, NBITS = 164, 32, 8, 16, 148
FRAME = NSYM * CPS * SPC
SYM = CPS * SPC
ALPHA = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-/"
SYNCWORD = 0x9A6D
VECTORS = {
    ("ON6URE", "network"):
        "++-++-+++--+---+--+-+-++----++++-++++-+++++++--+---+--+++++++++++-+--++-+--+++--++--+++---+--+-----++++----+---------+++-+--+-++++++++++-++---+----+++-+--++++-+--++",
    ("ON0ORA", "local"):
        "++-++-+++--+---+-----+++-+++-++-++-+--+------+-+++---+++---+-+-++--+--+-+--++---++---++-++++++-+-++++++--+--++----+--++--+-+---++------+++--++--------++-+-++--+--++",
    ("ON4ABC-XY", "network"):
        "++-++-+++--+---+-+---++-+-----+++-+---+-+++++---++++-++++--++-+-+--+--+-+--+--++++++++-++++--+-+--+----++++-----+++++++++--+---+-++++++-+--++++--++---++--+--+-+--++",
}


def chips():
    r, out = 0x1ACE5, []
    for _ in range(NSYM * CPS):
        bit = (r ^ (r >> 2) ^ (r >> 3) ^ (r >> 5)) & 1
        r = (r >> 1) | (bit << 16)
        out.append(1.0 if r & 1 else -1.0)
    return out


CHIPS = chips()
W = 2 * math.pi * 4330 / FRAME
REF_I = [CHIPS[n // SPC] * math.cos(W * n) for n in range(FRAME)]
REF_Q = [CHIPS[n // SPC] * math.sin(W * n) for n in range(FRAME)]


def crc16(bits):
    r = 0xFFFF
    for b in bits:
        r ^= b << 15
        r = ((r << 1) ^ 0x1021) & 0xFFFF if r & 0x8000 else (r << 1) & 0xFFFF
    return [(r >> (15 - i)) & 1 for i in range(16)]


def viterbi(soft):
    par = lambda v: bin(v).count("1") & 1
    metric = [0.0] + [-1e18] * 63
    back = []
    for t in range(len(soft) // 2):
        new, bk = [-1e18] * 64, [0] * 64
        for s in range(64):
            if metric[s] < -1e17:
                continue
            for b in (0, 1):
                sr = ((s << 1) | b) & 0x7F
                m = metric[s] + (soft[2 * t] if par(sr & 0o171) else -soft[2 * t]) \
                              + (soft[2 * t + 1] if par(sr & 0o133) else -soft[2 * t + 1])
                n = sr & 0x3F
                if m > new[n]:
                    new[n], bk[n] = m, s
        metric = new
        back.append(bk)
    s, bits = 0, []
    for bk in reversed(back):
        bits.append(s & 1)
        s = bk[s]
    return bits[::-1][:68]


def read_frame(x, start):
    """The message of the frame that starts at sample `start`, or None."""
    if start + FRAME > len(x):
        return None
    z = []
    for k in range(NSYM):
        a = start + k * SYM
        i = sum(x[a + n] * REF_I[k * SYM + n] for n in range(SYM))
        q = sum(x[a + n] * REF_Q[k * SYM + n] for n in range(SYM))
        z.append(complex(i, q))
    d = [0.0] + [(z[k] * z[k - 1].conjugate()).real for k in range(1, NSYM)]
    if any((d[k] < 0) != bool((SYNCWORD >> k) & 1) for k in range(1, NSYNC)):
        return None                                  # the known symbols are not there
    il = [-v for v in d[NSYNC:]]
    bits = viterbi([il[(i * 59) % NBITS] for i in range(NBITS)])
    if crc16(bits[:52]) != bits[52:68]:
        return None
    v = 0
    for b in bits[:48]:
        v = (v << 1) | b
    s = ""
    for _ in range(9):
        s = ALPHA[v % 40] + s
        v //= 40
    return s.strip(), "local" if bits[48] else "network"


def scenario(tagtest, exe, tmp, check):
    for (call, where), want in VECTORS.items():
        got = subprocess.run([tagtest, "signs", call, where], capture_output=True, text=True).stdout.strip()
        check(got == want, f"the symbols of {call} ({where}) are the ones in docs/TAG.md")
    subprocess.run([tagtest, "wav", "ON4ABC-XY", "local", "6", f"{tmp}/t.wav"], check=True)
    with wave.open(f"{tmp}/t.wav") as w:
        a = array.array("h"); a.frombytes(w.readframes(w.getnframes()))
    got = [read_frame(a, 0), read_frame(a, FRAME)]
    check(got == [("ON4ABC-XY", "local")] * 2, f"the waveform reads back, frame after frame: {got}")
    subprocess.run([tagtest, "wav", "on6ure-toolong", "network", "3", f"{tmp}/t.wav"], check=True)
    with wave.open(f"{tmp}/t.wav") as w:
        a = array.array("h"); a.frombytes(w.readframes(w.getnframes()))
    check(read_frame(a, 0) == ("ON6URE-TO", "network"), f"a callsign is upper case and at most nine characters: {read_frame(a, 0)}")

    # a real repeater
    base = voice(3.0)
    top = max(abs(x) for x in base)
    write_wav(f"{tmp}/loud.wav", [x * 0.5 / top for x in base])
    ref = Reflector(tmp, proto=2, auth_key="e2e-secret")
    radio = Sa818()
    hp = free_port()
    conf = f"""[reflector]
host = 127.0.0.1:{ref.port}
callsign = ON0TST
auth_key = e2e-secret
default_tg = 8

[radio]
freq_rx = 431.200
freq_tx = 438.800
bandwidth = narrow
ctcss_rx = 131.8
ctcss_tx = 131.8

[tx]
delay_ms = 200
hang_ms = 6500
timeout_s = 60

[tag]
enabled = true
level_db = -40

[ident]
callsign = ON0TST
interval_min = 0

[gate]
mode = on

[audio]
alsa_device = file:pause:3000,{tmp}/loud.wav,pause:30000
tx_record = {tmp}/tx.wav

[status]
http_port = {hp}
bind = 127.0.0.1
"""
    node = Node(exe, tmp, "duplex", conf, 1773490000, radio)
    try:
        ov = overs_at(ref, 1, 12)
        if not check(bool(ov) and len(ov[0]) > 100, "a station on the input reaches the talkgroup"):
            return
        # the first transmission ends 6.5 s after the station; then one from the talkgroup
        time.sleep(max(0.0, node.t0 + 15.0 - time.monotonic()))
        ref.talk(8, "ON7BBB-P", ov[0][:100])
        time.sleep(10.5)
    finally:
        rc = node.stop()
        ref.close()
    check(rc == 0, f"clean exit on SIGTERM (exit code {rc})")
    rec = Recording(f"{tmp}/tx.wav")
    b = rec.bursts
    if not check(len(b) == 2, f"two transmissions (got {len(b)})"):
        return
    want = (("ON0TST", "local"), ("ON7BBB", "network"))      # ON7BBB-P: without what follows the dash
    names = ("a station on the input", "an over from the talkgroup")
    for (s, e_), w, name in zip(b, want, names):
        start = int(round(s * FS))
        frames = [read_frame(rec.audio, start + i * FRAME) for i in range(int((e_ - s) * FS) // FRAME)]
        tail = [f for f in frames if f]
        check(bool(tail) and tail[-1] == w and all(f in (w, ("ON0TST", "local")) for f in tail),
              f"{name}: the tail of the transmission says {w[0]}, {w[1]}  (frames: {frames})")
    quiet = max(abs(v) for v in rec.audio[int(b[0][1] * FS) + 400:int(b[1][0] * FS) - 400])
    check(quiet == 0, f"nothing of the tag without PTT (largest sample between the transmissions: {quiet})")


def main():
    tagtest, exe = (sys.argv[1:3] + ["./tagtest", "./tailchaser-duplex"])[:2] if len(sys.argv) >= 3 else ("./tagtest", "./tailchaser-duplex")
    crypto_selftest()
    tmp = tempfile.mkdtemp(prefix="tailchaser-tag-")
    check = Checks("the callsign tag")
    try:
        scenario(tagtest, exe, tmp, check)
    except Exception as e:                          # noqa: BLE001 — report, don't hide
        import traceback
        check(False, f"the test itself failed: {e!r}")
        check.note(traceback.format_exc().replace("\n", "\n       "))
    print(check.title)
    print("\n".join(l for l in check.lines if l.strip() not in ("ok", "FAIL")))
    if check.failed:
        try:
            print(open(f"{tmp}/duplex.log").read()[-3000:])
        except OSError:
            pass
        print(f"tag: {check.failed} failed — files kept in {tmp}")
        return 1
    print("tag: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
