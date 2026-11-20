#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""devcal — the deviation of a transmitter in Hz, measured on a web SDR.

    devcal.py --sdr http://websdr.example/ --profile 70cm --freq 438.800 --ctcss 131.8 \\
              --trigger "ssh node sudo systemctl kill -s USR1 tailchaser-transmitter"

Any OpenWebRX receiver that hears the transmitter will do: --sdr is its
address, --profile the part of the band it has to be showing (devcal goes to
the SDR device that has that profile, but never changes the profile — that
would change it for everybody listening). --max-dev is what full level should
give: 2500 Hz on a 12.5 kHz channel (the default), 5000 Hz on a 25 kHz one.

No deviation meter and no calibrated receiver: the SDR listens to the carrier
in SSB. There an FM signal is a row of tones — the carrier and, a tone's
frequency apart, its sidebands — and how strong the sidebands are next to the
carrier depends on the deviation only (Bessel functions), not on the SDR's
gain, its AGC or the signal strength. The node's test transmission (SIGUSR1)
sends 1000 Hz at a known level ([tx] test_level_db); from the deviation that
gives, devcal says where full level lands and what to change.

Only the Python standard library is used. It listens; it changes nothing on
the SDR for anybody else.
"""
import argparse
import cmath
import importlib.util
import json
import math
import os
import subprocess
import sys
import time


def bessel(n, m):
    return sum((-1) ** k / (math.factorial(k) * math.factorial(k + n)) * (m / 2) ** (2 * k + n) for k in range(30))


def index_from_lines(amps):
    """Modulation index from the amplitudes of carrier and sidebands
    (amps[n] = line n tone-frequencies from the carrier, both sides averaged):
    the index whose Bessel pattern |J0|, |J1|, ... looks like them. Works
    beyond the carrier nulls too — a low CTCSS tone is far beyond the first."""
    norm = math.sqrt(sum(v * v for v in amps)) or 1.0
    got = [v / norm for v in amps]
    errs = []
    for step in range(1, 1601):
        m = step * 0.005
        js = [abs(bessel(n, m)) for n in range(len(amps))]
        jn = math.sqrt(sum(v * v for v in js)) or 1.0
        errs.append((sum((g - j / jn) ** 2 for g, j in zip(got, js)), m))
    # With few lines several indexes fit as well: the lowest of them is the
    # one a transmitter that is not wildly over-driven has.
    floor = min(errs)[0]
    for i, (err, m) in enumerate(errs):
        if err <= floor + 2e-3 and (i + 1 == len(errs) or errs[i + 1][0] >= err):
            return m
    return min(errs)[1]


def fft(a):
    n = len(a)
    if n == 1:
        return a
    ev, od = fft(a[0::2]), fft(a[1::2])
    out = [0j] * n
    for k in range(n // 2):
        t = cmath.exp(-2j * math.pi * k / n) * od[k]
        out[k], out[k + n // 2] = ev[k] + t, ev[k] - t
    return out


def line(x, rate, f):
    """Amplitude of the tone at f Hz in x (Hann window)."""
    n = len(x)
    w = cmath.exp(-2j * math.pi * f / rate)
    acc, ph, tot = 0j, 1 + 0j, 0.0
    for i, v in enumerate(x):
        h = 0.5 - 0.5 * math.cos(2 * math.pi * i / (n - 1))
        acc += v * h * ph
        ph *= w
        tot += h
    return 2 * abs(acc) / tot


def peak_near(x, rate, f, span=6.0, step=1.0):
    best = max((line(x, rate, f + d * step), f + d * step) for d in range(int(-span / step), int(span / step) + 1))
    return best[1], best[0]


def find_carrier(x, rate, tone):
    """The carrier is where the spectrum is symmetric: every sideband has its
    twin on the other side. Returns its audio frequency, or None."""
    n = 4096
    spec = [0.0] * (n // 2)
    blocks = 0
    for s in range(0, len(x) - n, n // 2):
        seg = [x[s + i] * (0.5 - 0.5 * math.cos(2 * math.pi * i / (n - 1))) for i in range(n)]
        for k, v in enumerate(fft(seg)[:n // 2]):
            spec[k] += abs(v)
        blocks += 1
    if not blocks:
        return None
    hz = rate / n
    floor = sorted(spec)[len(spec) // 2]
    if max(spec) < 8 * floor:
        return None
    sm = [0.0] + [max(0.0, max(spec[k - 1:k + 2]) - 2 * floor) for k in range(1, n // 2 - 1)] + [0.0]
    lo, hi = int(150 / hz), int((rate / 2 - 150) / hz)
    span = int((tone + 150) / hz)
    best = None
    dt = int(round(tone / hz))
    need = 0.02 * max(sm)
    for k in range(lo + span, hi - span):
        # the test tone's sidebands have to be there, on both sides: half way
        # between two lines a spectrum looks symmetric too
        if min(max(sm[k + dt - 1:k + dt + 2]), max(sm[k - dt - 1:k - dt + 2])) < need:
            continue
        q = sm[k]
        for d in range(3, span):
            up, dn = sm[k + d], sm[k - d]
            q += 2 * min(up, dn) - abs(up - dn)
        if best is None or q > best[0]:
            best = (q, k)
    if best is None or best[0] <= 0:
        return None
    return peak_near(x, rate, best[1] * hz, span=2 * hz, step=0.5)[0]


def measure(x, rate, tone, ctcss):
    c = find_carrier(x, rate, tone)
    if c is None:
        return None
    a0 = peak_near(x, rate, c, 2, 0.5)[1]
    out = {"carrier_hz": c}
    for name, f in (("tone", tone), ("ctcss", ctcss)):
        if not f:
            continue
        if c - f < 150 or c + f > rate / 2 - 150:
            out[name] = None
            continue
        amps = [a0]
        for n in range(1, 7):
            if c - n * f < 150 or c + n * f > rate / 2 - 150:
                break
            amps.append((peak_near(x, rate, c + n * f, 4)[1] + peak_near(x, rate, c - n * f, 4)[1]) / 2)
        m = index_from_lines(amps)
        ref = max(amps)
        out[name] = {"index": m, "dev": m * f, "lines": [v / ref for v in amps]}
    return out


def synth(rate, secs, carrier, tone, dev, ctcss=0.0, ctcss_dev=0.0):
    """What the SDR's SSB audio of such a transmission looks like — for the self-test."""
    out, ph = [], 0.0
    for i in range(int(rate * secs)):
        t = i / rate
        inst = dev * math.cos(2 * math.pi * tone * t) + ctcss_dev * math.cos(2 * math.pi * ctcss * t)
        ph += 2 * math.pi * (carrier + inst) / rate
        out.append(8000 * math.cos(ph))
    return out


def selftest():
    ok = True
    for dev, cd in ((600, 0), (1500, 270), (2405, 0), (300, 500), (620, 268)):
        r = measure(synth(12000, 2.0, 3210.0, 1000.0, dev, 131.8, cd), 12000, 1000.0, 131.8 if cd else 0)
        got = r and r["tone"] and r["tone"]["dev"]
        good = got is not None and abs(got - dev) < 0.03 * dev + 5 and abs(r["carrier_hz"] - 3210) < 2
        if cd:
            good = good and abs(r["ctcss"]["dev"] - cd) < 0.03 * cd + 5
        print(f"  {'ok  ' if good else 'FAIL'} 1000 Hz at {dev} Hz deviation{f', CTCSS at {cd}' if cd else ''}: "
              f"measured {got and round(got)} Hz" + (f", CTCSS {round(r['ctcss']['dev'])} Hz" if cd and r else ""))
        ok = ok and good
    print("all checks passed" if ok else "1 check(s) FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("url", nargs="?", help="the web SDR (an OpenWebRX receiver that hears the transmitter)")
    ap.add_argument("--sdr", help="the same, as an option; or set TAILCHASER_SDR in the environment")
    ap.add_argument("--selftest", action="store_true", help="check the arithmetic on made-up signals, no SDR")
    ap.add_argument("--profile", help="part of the SDR profile's name the frequency is in, e.g. 70cm")
    ap.add_argument("--freq", type=float, help="the transmitter, MHz")
    ap.add_argument("--tone", type=float, default=1000.0, help="the test tone, Hz (1000)")
    ap.add_argument("--ctcss", type=float, default=0.0, help="the CTCSS tone, Hz: its deviation is measured too")
    ap.add_argument("--test-level", type=float, default=-20.0, help="[tx] test_level_db of the node (-20)")
    ap.add_argument("--max-dev", type=float, default=2500.0, help="deviation full level should give: 2500 narrow, 5000 wide")
    ap.add_argument("--trigger", help="command that starts the test transmission (else: start it yourself when asked)")
    ap.add_argument("--seconds", type=float, default=4.0, help="how long the test tone lasts")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    a.url = a.sdr or a.url or os.environ.get("TAILCHASER_SDR")
    if not a.url or not a.freq:
        ap.error("the SDR's address (--sdr, or TAILCHASER_SDR) and --freq are needed")

    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location("sdrmon", os.path.join(here, "sdrmon.py"))
    sdr = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sdr)

    rx = sdr.Receiver(a.url)
    rx.pump(4)
    if "center_freq" not in rx.config:
        rx.pump(4)
    if a.profile:
        want = [p for p in rx.profiles if a.profile.lower() in p["name"].lower()]
        if not want:
            sys.exit(f"no profile named like '{a.profile}'")
        dev_id = want[0]["id"].split("|")[0]
        if dev_id != rx.profile_id().split("|")[0]:
            rx.ws.send(json.dumps({"type": "setsdr", "params": {"sdr": dev_id}}))   # this connection only
            rx.pump(5)
        if rx.profile_id() != want[0]["id"]:
            sys.exit(f"the receiver is not on '{want[0]['name']}' now — devcal does not change that for others")
    hz = a.freq * 1e6
    if abs(hz - rx.config["center_freq"]) > rx.config["samp_rate"] / 2 - 10000:
        sys.exit("that frequency is not in what the SDR shows now")

    def listen(shift):
        rx.ws.send(json.dumps({"type": "dspcontrol", "params": {
            "low_cut": 100, "high_cut": 5900, "offset_freq": int(hz - 3000 + shift - rx.config["center_freq"]),
            "mod": "usb", "squelch_level": -150, "secondary_mod": False}}))
        rx.ws.send(json.dumps({"type": "dspcontrol", "action": "start"}))
        rx.pump(1.5)

    def one_pass(shift):
        listen(shift)
        if a.trigger:
            subprocess.Popen(a.trigger, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            input("start the test transmission now and press Enter ")
        rx.pump(1.6)                                    # keyed, TX delay, the SDR's AGC settled
        rx.audio = []
        rx.pump(max(1.5, a.seconds - 1.2))
        return measure([float(v) for v in rx.audio], sdr.RATE, a.tone, a.ctcss)

    r = one_pass(0.0)
    if r is None:
        sys.exit("no carrier with a test tone was heard — is the transmission on the air, on that frequency?")
    off = r["carrier_hz"] - 3000.0
    if r["tone"] is None or abs(off) > 900:             # too close to the edge of what the SDR passes
        print(f"carrier {off:+.0f} Hz from where it was expected — once more, centred", flush=True)
        time.sleep(22)                                  # the first transmission has to end
        r = one_pass(off)
        if r is None or r["tone"] is None:
            sys.exit("the carrier could not be centred")
        off += r["carrier_hz"] - 3000.0

    t = r["tone"]
    full = t["dev"] * 10 ** (-a.test_level / 20)
    delta = 20 * math.log10(a.max_dev / full)
    print(f"carrier            {off:+.0f} Hz off (transmitter and SDR together)")
    if r.get("ctcss"):
        print(f"CTCSS {a.ctcss:5.1f} Hz     {r['ctcss']['dev']:.0f} Hz deviation")
    print(f"test tone {a.tone:.0f} Hz  {t['dev']:.0f} Hz deviation at {a.test_level:.0f} dBFS"
          f"   (carrier and sidebands: {' '.join(f'{v:.2f}' for v in t['lines'])})")
    print(f"full level (0 dBFS) would be {full:.0f} Hz — wanted {a.max_dev:.0f} Hz")
    if abs(delta) < 0.5:
        print("that is right: nothing to change")
    else:
        print(f"change the transmit level by {delta:+.1f} dB: [tx] level_db {delta:+.1f} from what it is now "
              f"(or the sound card: [audio] playback_pct), then measure again")
    return 0


if __name__ == "__main__":
    sys.exit(main())
