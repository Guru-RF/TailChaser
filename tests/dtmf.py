#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""DTMF: the decoder hears digits as radios send them and none in speech, and
the gate keeps the tones off the talkgroup.

    python3 tests/dtmf.py ./dtmftest ./gatetest

Everything is generated here (standard library only): digits that are short,
lopsided, off frequency, noisy, clipped, riding on hum; and talkers — a glottal
pulse train with a wandering pitch through three formant resonators, stepping
through the vowels. Such a talker is purer than any real voice, which makes it
the harder test: a high voice has only one harmonic inside each formant, and
when those two land on a low and a high DTMF tone the signal IS two sines.
"""
import math
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

FS = 16000
LOW = (697.0, 770.0, 852.0, 941.0)
HIGH = (1209.0, 1336.0, 1477.0, 1633.0)
KEYS = ("123A", "456B", "789C", "*0#D")
TONES = {k: (LOW[r], HIGH[c]) for r, row in enumerate(KEYS) for c, k in enumerate(row)}
ALL_KEYS = "1123A456B789C*0#DD91#1"       # every key, and some pressed twice in a row

FAILED = 0


def check(cond, what):
    global FAILED
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        FAILED += 1
    return cond


def db(x):
    return 10.0 ** (x / 20.0)


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(s * 32767))))
                               for s in samples))


def read_wav(path):
    with wave.open(path, "rb") as w:
        raw = w.readframes(w.getnframes())
    return [s / 32768.0 for (s,) in struct.iter_unpack("<h", raw)]


def digits(keys, tone_ms=100, gap_ms=100, level=0.15, twist_db=0.0, off=0.0, seed=1):
    """A row of key presses. `twist_db` > 0 makes the high tone the stronger one,
    `off` detunes the keypad (0.015 = 1.5 % high). The gaps vary by up to 20 ms,
    so the tones start anywhere in the decoder's 20 ms blocks."""
    rnd = random.Random(seed)
    out = [0.0] * int(0.1 * FS + rnd.randrange(320))
    a_lo, a_hi = level * db(-twist_db / 2), level * db(twist_db / 2)
    for k in keys:
        f_lo, f_hi = (f * (1.0 + off) for f in TONES[k])
        n = int(tone_ms * FS / 1000)
        p_lo, p_hi = rnd.uniform(0, 6.283), rnd.uniform(0, 6.283)
        ramp = int(0.002 * FS)
        for i in range(n):
            env = min(1.0, (i + 1) / ramp, (n - i) / ramp)
            out.append(env * (a_lo * math.sin(2 * math.pi * f_lo * i / FS + p_lo) +
                              a_hi * math.sin(2 * math.pi * f_hi * i / FS + p_hi)))
        out += [0.0] * (int(gap_ms * FS / 1000) + rnd.randrange(320))
    return out + [0.0] * int(0.1 * FS)


def hiss(n, level_db, seed=7):
    rnd = random.Random(seed)
    s = db(level_db)
    return [rnd.gauss(0.0, s) for _ in range(n)]


def add(x, y):
    return [a + b for a, b in zip(x, y)]


def sine(n, freq, level):
    return [level * math.sin(2 * math.pi * freq * i / FS) for i in range(n)]


VOWELS = {  # first three formants, Hz (Peterson & Barney, men)
    "a": (730, 1090, 2440), "uh": (640, 1190, 2390), "er": (490, 1350, 1690), "ae": (660, 1720, 2410),
    "e": (530, 1840, 2480), "i": (270, 2290, 3010), "o": (570, 840, 2410), "u": (300, 870, 2240),
}


def talker(secs, pitch_lo, pitch_hi, seed, tract=1.0, vowels=tuple(VOWELS)):
    """Syllables of 120-320 ms on a random vowel, each gliding to a new pitch
    between pitch_lo and pitch_hi; `tract` scales the formants (1.17: a woman,
    1.3: a child)."""
    rnd = random.Random(seed)
    out, phase = [], 0.0
    state = [[0.0, 0.0] for _ in range(3)]
    f0 = target = (pitch_lo + pitch_hi) / 2
    t0 = t_next = syl = 0.0
    coef = []
    for i in range(int(secs * FS)):
        t = i / FS
        if t >= t_next:
            syl = rnd.uniform(0.12, 0.32)
            t0, t_next = t, t + syl + rnd.uniform(0.02, 0.12)
            target = rnd.uniform(pitch_lo, pitch_hi)
            coef = []
            for fc, bw in zip(VOWELS[rnd.choice(vowels)], (90.0, 110.0, 170.0)):
                r = math.exp(-math.pi * bw / FS)
                coef.append((2 * r * math.cos(2 * math.pi * fc * tract / FS), -r * r))
        f0 += (target - f0) * 0.0004
        phase += f0 * (1 + 0.01 * math.sin(2 * math.pi * 5.1 * t)) / FS
        x = 0.0
        if phase >= 1.0:
            phase -= 1.0
            x = 1.0
        y = 0.0
        for (a1, a2), st, g in zip(coef, state, (1.0, 0.5, 0.25)):
            w = x + a1 * st[0] + a2 * st[1]
            st[1], st[0] = st[0], w
            y += g * w
        u = (t - t0) / syl
        env = math.sin(math.pi * u) ** 0.5 if 0.0 <= u <= 1.0 else 0.0
        out.append(0.1 * y * env + rnd.gauss(0.0, 0.002))
    return out


def rms_db(x):
    return 10 * math.log10(sum(s * s for s in x) / max(1, len(x)) + 1e-12)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: dtmf.py ./dtmftest ./gatetest")
    dtmftest, gatetest = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="tailchaser-dtmf-")

    def decode(name, samples):
        path = f"{tmp}/{name}.wav"
        write_wav(path, samples)
        out = subprocess.run([dtmftest, path], capture_output=True, text=True, timeout=60).stdout
        f = out.rstrip("\n").split("\t")
        return "" if len(f) < 4 or f[3] == "-" else f[3]

    def heard(what, cases):
        """Every case must decode to exactly the keys that were pressed."""
        bad = []
        for name, keys, samples in cases:
            got = decode(name, samples)
            if got != keys:
                bad.append(f"{name}: pressed {keys}, heard {got or 'nothing'}")
        for b in bad:
            print("       " + b)
        check(not bad, what)

    def silent(what, cases):
        bad = []
        for name, samples in cases:
            got = decode(name, samples)
            if got:
                bad.append(f"{name}: heard {got}")
        for b in bad:
            print("       " + b)
        check(not bad, what)

    try:
        print("DTMF: digits")
        plain = digits(ALL_KEYS)
        n = len(plain)
        heard("every key, some twice in a row, 100 ms each", [("plain", ALL_KEYS, plain)])
        heard("50 ms digits 50 ms apart, wherever they start",
              [(f"fast{s}", ALL_KEYS, digits(ALL_KEYS, 50, 50, seed=s)) for s in (1, 2, 3)])
        heard("one tone 9 dB stronger than the other, either way",
              [("twist+9", ALL_KEYS, digits(ALL_KEYS, twist_db=9)),
               ("twist-9", ALL_KEYS, digits(ALL_KEYS, twist_db=-9))])
        heard("a keypad 1.5 % low, or high",
              [("low", ALL_KEYS, digits(ALL_KEYS, off=-0.015)),
               ("high", ALL_KEYS, digits(ALL_KEYS, off=0.015))])
        heard("in noise 10 dB below the digits",
              [("noisy", ALL_KEYS, add(plain, hiss(n, 20 * math.log10(0.15) - 10)))])
        heard("on a DC offset, on mains hum as loud as the digits, with a CTCSS tone underneath",
              [("dc", ALL_KEYS, [s + 0.05 for s in plain]),
               ("hum", ALL_KEYS, add(add(plain, sine(n, 50.0, 0.15)), sine(n, 150.0, 0.05))),
               ("ctcss", ALL_KEYS, add(plain, sine(n, 88.5, 0.03)))])
        heard("quiet (-45 dBFS) and clipped by the transmitter",
              [("quiet", ALL_KEYS, add(digits(ALL_KEYS, level=0.004), hiss(n, -65))),
               ("clipped", ALL_KEYS, [max(-0.5, min(0.5, 4 * s)) for s in digits(ALL_KEYS, level=0.3)])])

        print("DTMF: what is not a digit")
        silent("a man, a woman and a child talking, 30 s each",
               [("man", talker(30, 85, 155, 1)), ("man-low", talker(30, 70, 110, 2, 0.95)),
                ("woman", talker(30, 165, 255, 1, 1.17)), ("woman-2", talker(30, 165, 255, 2, 1.17)),
                ("child", talker(30, 250, 400, 1, 1.3)), ("child-2", talker(30, 250, 400, 2, 1.3))])
        silent("a voice built to sit on the digit 1: pitch 174 Hz, its 4th and 7th harmonic on 697 and 1219 Hz",
               [("monotone", talker(30, 172, 176, 1)), ("one-vowel", talker(30, 173.5, 174.5, 3, 1.0, ("uh",)))])
        k = 10 * FS
        silent("noise, a 1750 Hz burst, 941 Hz alone, 1000 + 1500 Hz, 700 + 1200 Hz with a third tone",
               [("noise", hiss(k, -20)), ("1750", sine(k, 1750.0, 0.2)), ("941", sine(k, 941.0, 0.2)),
                ("twotone", add(sine(k, 1000.0, 0.15), sine(k, 1500.0, 0.15))),
                ("three", add(add(sine(k, 697.0, 0.15), sine(k, 1209.0, 0.15)), sine(k, 1000.0, 0.12)))])

        print("DTMF: the gate")
        # one over: three seconds of talk, a command, two more seconds of talk
        cmd = "911745#"
        d = digits(cmd, level=0.1, seed=5)
        lead, pause = [0.0] * int(0.137 * FS), [0.0] * int(0.4 * FS)
        a, b = talker(3.0, 85, 155, 4), talker(2.0, 85, 155, 5)
        over = lead + a + pause + d + pause + b + [0.0] * int(0.2 * FS)
        over = add(over, hiss(len(over), -65))
        a0, a1 = len(lead), len(lead) + len(a)          # the first words
        d0, d1 = a1 + len(pause), a1 + len(pause) + len(d)      # the row of digits
        b0, b1 = d1 + len(pause), d1 + len(pause) + len(b)      # the last words
        write_wav(f"{tmp}/over.wav", over)
        x = read_wav(f"{tmp}/over.wav")
        for mode in ("on", "monitor", "off"):
            out = subprocess.run([gatetest, "-m", mode, "-o", f"{tmp}/heard.wav", f"{tmp}/over.wav"],
                                 capture_output=True, text=True, timeout=60).stdout
            got = "".join(re.findall(r"node: DTMF (\S)", out))
            h = read_wav(f"{tmp}/heard.wav")
            cut = len(x) // 320 * 320 - len(h)          # mode on: the over begins at the voice, not at the carrier
            tones = h[d0 - cut - int(0.2 * FS):d1 - cut + int(0.2 * FS)]
            peak = max(abs(v) for v in tones) if cut >= 0 and tones else 1.0
            check(got == cmd and peak < 0.01,
                  f"gate {mode}: a command inside a spoken over is decoded ({got or 'nothing'}) and none of "
                  f"its tones reaches the talkgroup (peak there {peak:.4f}; the tones peak at 0.2)")
            check(cut >= 0 and h[a0 + FS - cut:a1 - cut] == x[a0 + FS:a1] and h[b0 - cut:b1 - cut] == x[b0:b1],
                  "...and the words before and after it arrive untouched")
        write_wav(f"{tmp}/digits.wav", add(d, hiss(len(d), -65)))
        out = subprocess.run([gatetest, "-m", "on", f"{tmp}/digits.wav"],
                             capture_output=True, text=True, timeout=60).stdout
        check("".join(re.findall(r"node: DTMF (\S)", out)) == cmd and "-> blocked" in out
              and "looks like tone" in out and "over BEGINS" not in out,
              "gate on: a transmission with nothing but digits is decoded, stays off the talkgroup and is called a tone")
        # the same key as the last thing in one transmission and the first thing in the next
        five = digits("5", tone_ms=300, gap_ms=0, level=0.1)
        write_wav(f"{tmp}/ends.wav", [0.0] * int(0.2 * FS) + five[:-int(0.1 * FS)])
        write_wav(f"{tmp}/begins.wav", five[int(0.1 * FS) + 320:] + [0.0] * int(0.2 * FS))
        out = subprocess.run([gatetest, "-m", "on", "-g", "1000", f"{tmp}/ends.wav", f"{tmp}/begins.wav"],
                             capture_output=True, text=True, timeout=60).stdout
        got = "".join(re.findall(r"node: DTMF (\S)", out))
        check(got == "55", f"a key held when one transmission ends and pressed again as the next begins is two presses ({got or 'nothing'})")
        # ...but one press when it was only the squelch that dropped out for a moment
        for mode in ("on", "monitor"):
            out = subprocess.run([gatetest, "-m", mode, "-g", "100", f"{tmp}/ends.wav", f"{tmp}/begins.wav"],
                                 capture_output=True, text=True, timeout=60).stdout
            got = "".join(re.findall(r"node: DTMF (\S)", out))
            check(got == "5", f"gate {mode}: a key held through a 100 ms squelch drop-out is one press ({got or 'nothing'})")

        # monitor mode says what mode on does, opening by opening
        write_wav(f"{tmp}/s1_voice.wav", talker(2.5, 100, 140, seed=5))
        write_wav(f"{tmp}/s2_noise.wav", hiss(int(0.6 * FS), -20, seed=3))                 # in conversation: passes
        write_wav(f"{tmp}/s3_digits.wav", digits("91#", tone_ms=120, gap_ms=120))
        write_wav(f"{tmp}/s4_noise.wav", hiss(int(0.6 * FS), -20, seed=4))                 # digits start no conversation
        write_wav(f"{tmp}/s5_late.wav", [0.0] * int(3.4 * FS) + talker(2.0, 100, 140, seed=6))  # a voice after the deadline
        write_wav(f"{tmp}/s6_noise.wav", hiss(int(0.6 * FS), -20, seed=5))                 # ...nor does that
        seq = [f"{tmp}/s{i}_{n}.wav" for i, n in ((1, "voice"), (2, "noise"), (3, "digits"), (4, "noise"), (5, "late"), (6, "noise"))]
        verdicts = {}
        for mode in ("on", "monitor"):
            out = subprocess.run([gatetest, "-m", mode, "-H", "2", "-g", "1000"] + seq[:2], capture_output=True, text=True, timeout=60).stdout
            out += subprocess.run([gatetest, "-m", mode, "-H", "2", "-g", "1000"] + seq[2:], capture_output=True, text=True, timeout=60).stdout
            pat = r"opening \S+ -> (\S+)" if mode == "on" else r"opening \S+ -> ungated \(on: ([^,)]+)"
            verdicts[mode] = re.findall(pat, out)
        check(verdicts["on"] == ["speech", "in-conversation", "blocked", "blocked", "blocked", "blocked"],
              f"gate on: a voice, noise right after it, digits only, noise, a voice after the deadline, noise ({' / '.join(verdicts['on'])})")
        check(verdicts["monitor"] == verdicts["on"],
              f"gate monitor: says the same of each of them ({' / '.join(verdicts['monitor'])})")
    finally:
        if FAILED and os.environ.get("KEEP"):
            print(f"kept: {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    print()
    if FAILED:
        print(f"{FAILED} check(s) FAILED")
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
