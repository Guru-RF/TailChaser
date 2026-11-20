#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""Build a lab corpus for tuning the speech detector (tools/vadtest).

    make_corpus.py OUTDIR speech1.wav [speech2.wav ...]

Each speech file must be 16 kHz mono 16-bit WAV (any clean recording of someone
talking). For every one it writes a "through the radio" version (300-3000 Hz)
and copies of it buried in FM squelch noise at several signal-to-noise ratios.
It also writes the things that must NOT be taken for speech:

    noise_fm.wav       squelch noise / a carrier too weak to quiet the receiver
    noise_white.wav    flat band-limited noise
    carrier_quiet.wav  a full-quieting dead carrier (near silence)
    carrier_hum.wav    dead carrier with mains hum
    carrier_ctcss.wav  dead carrier with the CTCSS tone leaking through
    tone_1750.wav      1750 Hz tone burst
    whistle.wav        somebody whistling into the microphone
    glide_low.wav      a low tone gliding 380-520 Hz
    sine_<f>.wav       steady sines, 350 Hz .. 2800 Hz
    twotone.wav        1000 + 1500 Hz
    noisytone_<f>_snr<n>.wav   a tone buried in squelch noise
    dtmf.wav           DTMF digits

Needs numpy only. This is a lab aid: the real proof is `mode = monitor` on the
air (see README).
"""
import sys
import wave

import numpy as np

FS = 16000


def read_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getframerate() == FS and w.getsampwidth() == 2, path
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
        if w.getnchannels() > 1:
            x = x.reshape(-1, w.getnchannels())[:, 0]
    return x / 32768.0


def write_wav(path, x):
    x = np.clip(x, -1.0, 1.0)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes((x * 32767.0).astype("<i2").tobytes())


def shape(x, gain_of_f):
    """Filter by multiplying the spectrum with gain_of_f(frequencies)."""
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / FS)
    return np.fft.irfft(X * gain_of_f(f), len(x))


def band(f, lo=300.0, hi=3000.0):
    """Radio audio pass-band: 4th-order-ish skirts."""
    hp = (f / lo) ** 4 / np.sqrt(1.0 + (f / lo) ** 8)
    lp = 1.0 / np.sqrt(1.0 + (f / hi) ** 8)
    return hp * lp


def deemph(f):
    """6 dB/octave de-emphasis (corner 300 Hz)."""
    return 1.0 / np.sqrt(1.0 + (f / 300.0) ** 2)


def rms(x):
    return np.sqrt(np.mean(x * x))


def db(v):
    return 10.0 ** (v / 20.0)


def active_rms(x):
    """RMS over the frames where somebody is actually speaking."""
    n = len(x) // 320
    fr = x[: n * 320].reshape(n, 320)
    e = np.sqrt(np.mean(fr * fr, axis=1))
    return np.sqrt(np.mean(e[e > e.max() * db(-30)] ** 2))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out = sys.argv[1]
    rng = np.random.default_rng(1)
    secs = 180
    n = secs * FS
    t = np.arange(n) / FS

    white = rng.standard_normal(n)
    noise_fm = shape(white, lambda f: band(f) * deemph(f))
    noise_fm *= db(-20) / rms(noise_fm)
    write_wav(f"{out}/noise_fm.wav", noise_fm)

    noise_white = shape(rng.standard_normal(n), band)
    noise_white *= db(-20) / rms(noise_white)
    write_wav(f"{out}/noise_white.wav", noise_white)

    hiss = rng.standard_normal(n)
    write_wav(f"{out}/carrier_quiet.wav", hiss * db(-66))

    hum = sum(a * np.sin(2 * np.pi * h * t + p)
              for h, a, p in ((100, 1.0, 0.0), (200, 0.6, 1.0), (300, 0.5, 2.0),
                              (400, 0.3, 0.5), (500, 0.2, 1.7)))
    write_wav(f"{out}/carrier_hum.wav", hum * db(-28) / rms(hum) + hiss * db(-60))

    write_wav(f"{out}/carrier_ctcss.wav",
              np.sin(2 * np.pi * 131.8 * t) * db(-22) + hiss * db(-60))

    write_wav(f"{out}/tone_1750.wav", np.sin(2 * np.pi * 1750 * t) * db(-14) + hiss * db(-55))

    # whistle: 1.1 kHz gliding +-150 Hz with a 5 Hz vibrato
    fw = 1100 + 150 * np.sin(2 * np.pi * 0.4 * t) + 25 * np.sin(2 * np.pi * 5 * t)
    write_wav(f"{out}/whistle.wav", np.sin(2 * np.pi * np.cumsum(fw) / FS) * db(-14) + hiss * db(-55))

    fg = 450 + 70 * np.sin(2 * np.pi * 0.5 * t)
    write_wav(f"{out}/glide_low.wav", np.sin(2 * np.pi * np.cumsum(fg) / FS) * db(-14) + hiss * db(-50))
    for f in (350, 400, 450, 500, 600, 800, 1000, 2000, 2800):
        write_wav(f"{out}/sine_{f}.wav", np.sin(2 * np.pi * f * t) * db(-14) + hiss * db(-50))
    write_wav(f"{out}/twotone.wav",
              (np.sin(2 * np.pi * 1000 * t) + np.sin(2 * np.pi * 1500 * t)) * db(-20) + hiss * db(-50))
    for f in (400, 1000, 1750):
        for snr in (12, 6):
            mix = np.sin(2 * np.pi * f * t) * rms(noise_fm) * db(snr) * np.sqrt(2) + noise_fm
            write_wav(f"{out}/noisytone_{f}_snr{snr:02d}.wav", mix * min(1.0, 0.9 / np.abs(mix).max()))

    lo = (697, 770, 852, 941)
    hi = (1209, 1336, 1477, 1633)
    d = np.zeros(n)
    k = 0
    pos = 0
    while pos + FS // 2 < n:
        m = int(0.2 * FS)
        tt = np.arange(m) / FS
        d[pos:pos + m] = (np.sin(2 * np.pi * lo[k % 4] * tt) +
                          np.sin(2 * np.pi * hi[(k // 4) % 4] * tt)) * db(-20)
        pos += int(0.4 * FS)
        k += 1
    write_wav(f"{out}/dtmf.wav", d + hiss * db(-55))

    for path in sys.argv[2:]:
        name = path.rsplit("/", 1)[-1].rsplit(".", 1)[0]
        s = shape(read_wav(path), band)
        s *= db(-20) / active_rms(s)
        write_wav(f"{out}/{name}_radio.wav", s)
        for snr in (20, 12, 6, 3, 0):
            nz = np.resize(noise_fm, len(s)) * db(-snr)
            mix = s + nz
            write_wav(f"{out}/{name}_snr{snr:02d}.wav", mix * min(1.0, 0.9 / np.abs(mix).max()))


if __name__ == "__main__":
    main()
