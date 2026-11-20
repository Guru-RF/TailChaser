#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""The receive normaliser on generated overs.

    python3 tests/agc.py ./agctest

The same talker at five very different levels — a station with hardly any
deviation up to one that overdrives the receiver — must come out of the
normaliser equally loud, without clipping, and noise alone must not be blown
up to speech level in an instant.
"""
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_receiver import noise, voice, write_wav   # noqa: E402

LINE = re.compile(r"in\s+(-?[\d.]+)\s+out\s+(-?[\d.]+)\s+settled\s+(-?[\d.]+) dBFS\s+gain\s+([+-]?[\d.]+) dB\s+peak\s+(\d+)")


def run(exe, *args):
    out = subprocess.run([exe, *args], capture_output=True, text=True, check=True).stdout
    rows = [LINE.search(l) for l in out.splitlines()]
    return [tuple(float(x) for x in m.groups()) for m in rows if m]


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "./agctest"
    tmp = tempfile.mkdtemp(prefix="tailchaser-agc-")
    failed = 0

    def check(ok, what):
        nonlocal failed
        print(("  ok   " if ok else "  FAIL ") + what)
        failed += 0 if ok else 1

    base = voice(5.0)
    top = max(abs(x) for x in base)
    scales = {"whisper": 0.02, "quiet": 0.04, "normal": 0.15, "loud": 0.5, "overdriven": 0.98}
    results = {}
    for name, peak in scales.items():
        path = f"{tmp}/{name}.wav"
        write_wav(path, [x * peak / top for x in base])
        # each level on a fresh normaliser: nothing carried over from the one before
        results[name] = run(exe, path)[0]
    ins = [r[0] for r in results.values()]
    settled = [r[2] for r in results.values()]
    for name, r in results.items():
        print(f"       {name:<11} in {r[0]:6.1f} dBFS -> {r[2]:6.1f} dBFS (gain {r[3]:+.1f} dB, peak {int(r[4])})")
    check(max(ins) - min(ins) > 30, f"the test overs are {max(ins) - min(ins):.0f} dB apart going in")
    check(max(settled) - min(settled) <= 3.0,
          f"...and within {max(settled) - min(settled):.1f} dB of each other coming out (3 dB allowed)")
    check(all(abs(s + 20.0) <= 3.0 for s in settled), "every one of them ends up at the -20 dBFS target (±3 dB)")
    check(all(r[4] < 32767 for r in results.values()), "nothing reaches full scale: no clipping")

    # the first second of an over: the gain of the over before it is the starting point
    two = run(exe, f"{tmp}/loud.wav", f"{tmp}/quiet.wav")
    check(abs(two[1][2] + 20.0) <= 3.0, f"a quiet station right after a loud one is brought up as well ({two[1][2]:.1f} dBFS)")
    two = run(exe, f"{tmp}/quiet.wav", f"{tmp}/loud.wav")
    check(two[1][4] < 32767 and abs(two[1][2] + 20.0) <= 3.0,
          f"a loud station right after a quiet one is taken down at once, without clipping (peak {int(two[1][4])})")

    # limits
    r = run(exe, "-g", "6", f"{tmp}/whisper.wav")[0]
    check(r[3] <= 6.2, f"max gain is respected: +{r[3]:.1f} dB with a 6 dB limit")
    r = run(exe, "-t", "-26", f"{tmp}/normal.wav")[0]
    check(abs(r[2] + 26.0) <= 3.0, f"another target is honoured: {r[2]:.1f} dBFS for -26")

    # noise below the floor is left alone
    write_wav(f"{tmp}/hiss.wav", noise(3.0, 0.0002))
    r = run(exe, f"{tmp}/hiss.wav")[0]
    check(r[1] < -50.0, f"the hiss of an empty channel is not amplified to speech level ({r[1]:.1f} dBFS)")

    print("\nall checks passed" if not failed else f"\n{failed} check(s) FAILED   (files kept in {tmp})")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
