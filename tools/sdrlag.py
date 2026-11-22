#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""sdrlag — how far behind a web SDR is, measured with the node's own transmitter.

    sudo python3 tools/sdrlag.py --sdr http://websdr.example/ --profile 70cm-high --freq 438.800 \\
              --trigger "systemctl kill -s USR1 tailchaser-transmitter"

Run it ON THE NODE: the moment the test transmission is asked for is the
clock everything is measured against, and over ssh that moment is not known
well enough.

A web SDR brings what it hears a second or two late — its own processing, the
internet, and the buffer tailchaser-sdrreceiver keeps against jitter. That
delay does not change from over to over, so it is measured once and written
down: [receiverN] lag_ms on the repeater or node that votes with that
receiver. Without it the voter learns the delay from the overs two receivers
share, which takes a few overs and is less exact.

How: the node's test transmission (SIGUSR1) sends 1000 Hz at a known moment.
sdrlag listens to the transmitter's OUTPUT on the web SDR, in FM, and times
the first of that tone. --freq is therefore the transmit frequency (a simplex
node: its one frequency), and --profile the part of the band that has it; the
SDR has to be showing that band now — sdrlag goes to the SDR device that has
the profile, but never changes a profile (that would change it for everybody
listening).

    --tx-delay-ms   [tx] delay_ms of the node (the tone starts that long after
                    the carrier)
    --buffer-ms     [sdr] buffer_ms of the tailchaser-sdrreceiver (300)
    --ctcss-delay-ms  [ctcss] delay_ms of the tailchaser-sdrreceiver when it has
                    detect = true (300 then): the tone squelch holds the audio
                    back that long. 0 when it listens without a tone.

That way is for OpenWebRX receivers. A KiwiSDR, a Web-888 and an UberSDR are
short wave receivers: no transmitter of ours reaches them. They are timed on
a time signal instead, with nothing transmitted at all:

    python3 tools/sdrlag.py --type kiwisdr --sdr http://kiwi.example:8073
    python3 tools/sdrlag.py --type web888  --sdr http://web888.example:8073
    python3 tools/sdrlag.py --type ubersdr --sdr https://ubersdr.example/

DCF77 (77.5 kHz, Mainflingen) drops its carrier at the start of every second
of UTC, except second 59. sdrlag listens to it for 80 s and compares when the
seconds arrive with this machine's clock, which therefore has to be right
(NTP). The receiver has to hear DCF77: Europe, with an antenna that does long
wave. It can be run anywhere, but the internet between the receiver and the
node is part of the delay: on the node is best.

Only the Python standard library (UberSDR: and libopus, which TailChaser
needs anyway). It listens; it changes nothing on the SDR for anybody else.
"""
import argparse
import ctypes
import ctypes.util
import importlib.util
import json
import math
import os
import random
import struct
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import uuid

SOUND_CARD_MS = 100         # the node's sound card: from "send this" to the air
WIN_MS = 20


def stream_base(marks, rate):
    """marks: [(clock when a block had arrived, samples received by then)].
    A block is never early, only late (the network, our own loop): the block
    that was least late says when sample 0 arrived."""
    return min(t - n / rate for t, n in marks)


def tone_onset(x, rate, tone, need=5):
    """The sample where `tone` begins: the first of `need` windows in a row
    that are mostly that one frequency. None when it is not there."""
    n = int(rate * WIN_MS / 1000)
    w = 2 * math.cos(2 * math.pi * tone / rate)
    run = 0
    for i in range(0, len(x) - n + 1, n):
        a = b = e = 0.0
        for v in x[i:i + n]:
            a, b = v + w * a - b, a
            e += v * v
        p = (a * a + b * b - w * a * b) * 2 / n          # the tone's share of the energy
        if e > n * 100.0 and p > 0.5 * e:
            run += 1
            if run == need:
                return i - (need - 1) * n
        else:
            run = 0
    return None


# --------------------------------------------------------------------------
# a time signal: DCF77

DCF77_HZ = 77500.0


def seconds_late(x, marks, rate, tone=1000.0):
    """x: DCF77 in USB, its carrier at `tone`. marks: [(UTC when a block had
    arrived, samples by then)]. -> (seconds late, spread of the marks in s,
    how many marks), or (None, why)."""
    base = stream_base(marks, rate)                     # UTC when sample 0 arrived
    n = int(rate) // 100
    w = 2 * math.cos(2 * math.pi * tone / rate)
    env = []                                            # the carrier, every 10 ms exactly, whatever the rate
    for k in range(int(len(x) * 100 / rate) - 1):
        i = int(round(k * rate / 100.0))
        a = b = 0.0
        for v in x[i:i + n]:
            a, b = v + w * a - b, a
        env.append(math.sqrt(max(a * a + b * b - w * a * b, 0.0)) * 2 / n)
    if len(env) < 6500:
        return None, "too little audio: 65 s at least, to see second 59 go by"
    med = sorted(env)[len(env) // 2]
    if med < 20.0:
        return None, "no carrier on 77.5 kHz — this receiver does not hear DCF77"
    # where in our second the carrier drops: fold the weak moments on one second
    hist = [0] * 100
    for k, e in enumerate(env):
        if e < 0.5 * med:
            hist[int(((base + k * 0.01) % 1.0) * 100) % 100] += 1
    best, phase = None, 0
    for ph in range(100):
        d = sum(hist[(ph + j) % 100] for j in range(8)) - sum(hist[(ph - 1 - j) % 100] for j in range(8))
        if best is None or d > best:
            best, phase = d, ph
    # second by second: when exactly it drops, and how deep
    drops, depth = [], {}
    for sec in range(int(math.floor(base)) + 1, int(base + len(env) / 100.0) - 1):
        k0 = int(round((sec + phase / 100.0 - 0.06 - base) * 100))
        if k0 < 30 or k0 + 30 >= len(env):
            continue
        ref = sorted(env[k0 - 25:k0])[12]
        seg = env[k0:k0 + 22]
        depth[sec] = min(seg[6:16]) / max(ref, 1e-9)
        for j, e in enumerate(seg):
            if e < 0.5 * ref:
                prev = seg[j - 1] if j else ref
                frac = (prev - 0.5 * ref) / max(prev - e, 1e-9)
                drops.append(base + (k0 + j - 1 + frac) * 0.01 + 0.005 - sec)
                break
    if len(drops) < 40:
        return None, "the second marks of DCF77 are not clear enough on this receiver"
    drops.sort()
    part = drops[len(drops) // 2]
    spread = drops[int(len(drops) * 0.9)] - drops[int(len(drops) * 0.1)]
    if spread > 0.03:
        return None, f"the second marks of DCF77 are not steady on this receiver (they vary by {spread * 1000:.0f} ms)"
    # the second without a drop is second 59
    bare = [q for q in depth if depth[q] > 0.75]        # a minute apart when there are two
    rest = [depth[q] for q in depth if depth[q] <= 0.75]
    if not bare or any((q - bare[0]) % 60 for q in bare) or (rest and max(rest) > 0.6):
        return None, "second 59 (the one without a mark) was not seen clearly — once more"
    return ((bare[0] - 59) % 60 + part, spread, len(drops)), None


def listen_kiwi(sdr, url, seconds, audio, marks):
    """KiwiSDR and Web-888: the same protocol."""
    u = urllib.parse.urlparse(url if "://" in url else "http://" + url)
    ws = sdr.Ws(url, f"{u.path.rstrip('/')}/kiwi/{int(time.time())}/SND")
    ws.send("SET auth t=kiwi p=#")
    rate, off, tuned, ka, end = 12000.0, 0.0, False, 0.0, time.time() + 15
    while time.time() < end:
        if time.time() > ka:
            ws.send("SET keepalive")
            ka = time.time() + 5
        m = ws.recv(0.05)
        if m is None:
            continue
        p = m[1]
        if p[:3] == b"MSG":
            kv = dict(q.split("=", 1) for q in p[3:].decode(errors="replace").split() if "=" in q)
            if "too_busy" in kv:
                raise ConnectionError("its owner lets no programs listen, only its own web page" if kv["too_busy"] == "0"
                                      else "it is full")
            if kv.get("badp", "0") != "0":
                raise ConnectionError("it wants a password, or another connection from here is there already")
            if "freq_offset" in kv:
                off = float(kv["freq_offset"])
            if "audio_rate" in kv:
                ws.send(f"SET AR OK in={int(kv['audio_rate'])} out=44100")
            if "sample_rate" in kv and not tuned:
                rate = float(kv["sample_rate"])
                for c in ("SET squelch=0 param=0.00", "SET genattn=0", "SET gen=0 mix=-1",
                          f"SET mod=usb low_cut=300 high_cut=2700 freq={(DCF77_HZ - 1000.0) / 1000 - off:.3f}",
                          "SET agc=1 hang=0 thresh=-100 slope=6 decay=4000 manGain=50", "SET compression=0",
                          "SET ident_user=sdrlag", "SET keepalive"):
                    ws.send(c)
                tuned, end = True, time.time() + seconds
        elif p[:3] == b"SND" and tuned and len(p) > 10 and not p[3] & 0x18:
            a = p[10:]
            audio += struct.unpack(("<" if p[3] & 0x80 else ">") + f"{len(a) // 2}h", a[:len(a) // 2 * 2])
            marks.append((time.time(), len(audio)))
    if not tuned:
        raise ConnectionError("it does not answer as a KiwiSDR or Web-888")
    return rate


def listen_uber(sdr, url, seconds, audio, marks):
    base = (url if "://" in url else "http://" + url).rstrip("/")
    lib = ctypes.util.find_library("opus")
    if not lib:
        sys.exit("libopus is needed for an UberSDR (apt install libopus0)")
    opus = ctypes.CDLL(lib)
    opus.opus_decoder_create.restype = ctypes.c_void_p
    sid = str(uuid.uuid4())
    req = urllib.request.Request(base + "/connection", data=json.dumps({"user_session_id": sid, "password": ""}).encode(),
                                 headers={"Content-Type": "application/json", "User-Agent": "sdrlag"})
    ans = json.load(urllib.request.urlopen(req, timeout=10))
    if not ans.get("allowed"):
        raise ConnectionError("it refuses: " + str(ans.get("reason", "")))
    ws = sdr.Ws(base, f"{urllib.parse.urlparse(base).path}/ws?frequency={int(DCF77_HZ - 1000)}&mode=usb&bandwidthLow=300"
                      f"&bandwidthHigh=2700&format=opus&version=2&user_session_id={sid}")
    dec, rate = None, 0
    pcm = (ctypes.c_int16 * 5760)()
    end, ka = time.time() + seconds, time.time() + 10
    while time.time() < end:
        if time.time() > ka:
            ws.send('{"type":"ping"}')
            ka = time.time() + 10
        m = ws.recv(0.05)
        if m is None:
            continue
        op, p = m
        if op == 1:
            if '"error"' in p.decode(errors="replace"):
                raise ConnectionError(p.decode(errors="replace")[:160])
            continue
        if len(p) < 22 or p[12] != 1:
            continue
        r = struct.unpack("<I", p[8:12])[0]
        if dec is None or r != rate:
            dec, rate = ctypes.c_void_p(opus.opus_decoder_create(r, 1, ctypes.byref(ctypes.c_int(0)))), r
        got = opus.opus_decode(dec, p[21:], len(p) - 21, pcm, 5760, 0)
        if got > 0:
            audio += pcm[:got]
            marks.append((time.time(), len(audio)))
    if not rate:
        raise ConnectionError("it sends no Opus audio")
    return float(rate)


def time_signal(a, sdr):
    try:
        out = subprocess.run(["timedatectl", "show", "-p", "NTPSynchronized", "--value"], capture_output=True, text=True, timeout=5)
        if out.returncode == 0 and out.stdout.strip() == "no":
            print("this machine's clock is NOT synchronised (NTP): the result is only as good as the clock", flush=True)
    except (OSError, subprocess.SubprocessError):
        pass
    audio, marks = [], []
    print(f"listening to DCF77 (77.5 kHz) on {a.url} for {a.seconds:.0f} s ...", flush=True)
    try:
        rate = (listen_uber if a.type == "ubersdr" else listen_kiwi)(sdr, a.url, a.seconds, audio, marks)
    except (ConnectionError, OSError, RuntimeError) as e:
        sys.exit(f"{a.url}: {e}")
    r, why = seconds_late([float(v) for v in audio], marks, rate)
    if r is None:
        sys.exit(why)
    late, spread, n = r
    sdr_ms = late * 1000.0
    lag = int(round((sdr_ms + a.buffer_ms + a.ctcss_delay_ms) / 10.0)) * 10
    print(f"{n} second marks, steady within {spread * 1000:.0f} ms")
    print(f"the web SDR brings them   {sdr_ms:.0f} ms late (its processing and the internet to here)")
    print(f"tailchaser-sdrreceiver    + {a.buffer_ms} ms ([sdr] buffer_ms)"
          + (f" + {a.ctcss_delay_ms} ms ([ctcss] delay_ms)" if a.ctcss_delay_ms else ""))
    print(f"that receiver is behind   {lag} ms")
    print(f"\nOn the repeater or node, in the section of that receiver:\n\n    lag_ms        = {lag}\n")
    return 0


def selftest():
    rate, rng, bad = 12000, random.Random(7), 0
    for lag, tone_at in ((1.37, 2.4), (0.62, 1.1)):
        x = [rng.gauss(0, 6000) for _ in range(int(tone_at * rate))]
        x += [9000 * math.sin(2 * math.pi * 1000 * i / rate) + rng.gauss(0, 300) for i in range(2 * rate)]
        # the stream reaches us `lag` late, in blocks, each a little later than it should be
        marks, n, t0 = [], 0, 100.0
        while n < len(x):
            n = min(len(x), n + 512)
            marks.append((t0 + lag + n / rate + rng.choice((0.0, 0.01, 0.04, 0.2)), n))
        on = tone_onset(x, rate, 1000.0)
        got = stream_base(marks, rate) + on / rate - (t0 + tone_at)
        ok = on is not None and abs(got - lag) < 0.03
        bad += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} a tone {lag:.2f} s late, in blocks with jitter, is timed as {got:.3f} s late")
    ok = tone_onset([rng.gauss(0, 6000) for _ in range(3 * rate)], rate, 1000.0) is None
    bad += not ok
    print(f"  {'ok  ' if ok else 'FAIL'} the hiss of an empty channel is not taken for the tone")
    # DCF77 as a KiwiSDR brings it: not quite 12000 samples a second, 1.31 s late
    for frate, lag in ((11998.92, 1.31), (24000.0, 0.045)):
        t0 = 1700000030.4                               # UTC of the first sample on the air
        x = []
        for i in range(int(75 * frate)):
            t = t0 + i / frate
            sec, into = int(t) % 60, t % 1.0
            low = sec != 59 and into < (0.2 if sec % 3 == 0 else 0.1)
            x.append((0.15 if low else 1.0) * 8000 * math.sin(2 * math.pi * 1000 * i / frate) + rng.gauss(0, 150))
        marks, n = [], 0
        while n < len(x):
            n = min(len(x), n + 512)
            marks.append((t0 + lag + n / frate + rng.choice((0.0, 0.01, 0.03, 0.15)), n))
        r, why = seconds_late(x, marks, frate)
        ok = r is not None and abs(r[0] - lag) < 0.015
        bad += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} DCF77 at {frate:.2f} samples a second, {lag * 1000:.0f} ms late, is timed as "
              f"{'%.0f ms late' % (r[0] * 1000) if r else why}")
    r, why = seconds_late([rng.gauss(0, 3000) for _ in range(70 * 12000)], [(100.0 + i, i * 12000) for i in range(1, 71)], 12000.0)
    ok = r is None
    bad += not ok
    print(f"  {'ok  ' if ok else 'FAIL'} a receiver that does not hear DCF77 gives no figure ({why})")
    print("all checks passed" if not bad else f"{bad} check(s) FAILED")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("url", nargs="?", help="the web SDR (an OpenWebRX receiver that hears the transmitter)")
    ap.add_argument("--sdr", help="the same, as an option; or set TAILCHASER_SDR in the environment")
    ap.add_argument("--selftest", action="store_true", help="check the arithmetic on made-up signals, no SDR")
    ap.add_argument("--type", default="openwebrx", choices=("openwebrx", "kiwisdr", "web888", "ubersdr"),
                    help="the kind of web SDR (openwebrx): the others are timed on DCF77, nothing is transmitted")
    ap.add_argument("--seconds", type=float, default=80.0, help="how long to listen to DCF77 (80; more than 65)")
    ap.add_argument("--profile", help="part of the SDR profile's name the frequency is in, e.g. 70cm-high")
    ap.add_argument("--freq", type=float, help="the transmitter's frequency, MHz")
    ap.add_argument("--trigger", help="command that starts the test transmission, run here on the node")
    ap.add_argument("--tone", type=float, default=1000.0, help="the test tone, Hz (1000)")
    ap.add_argument("--tx-delay-ms", type=int, default=300, help="[tx] delay_ms of the node (300)")
    ap.add_argument("--buffer-ms", type=int, default=300, help="[sdr] buffer_ms of the tailchaser-sdrreceiver (300)")
    ap.add_argument("--ctcss-delay-ms", type=int, default=0,
                    help="[ctcss] delay_ms of the tailchaser-sdrreceiver if it has detect = true (300), else 0")
    ap.add_argument("--wait", type=float, default=8.0, help="seconds to wait for the tone")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    a.url = a.sdr or a.url or os.environ.get("TAILCHASER_SDR")
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location("sdrmon", os.path.join(here, "sdrmon.py"))
    sdr = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sdr)
    if a.type != "openwebrx":
        if not a.url:
            ap.error("the SDR's address is needed (--sdr, or TAILCHASER_SDR)")
        return time_signal(a, sdr)
    if not a.url or not a.freq or not a.trigger:
        ap.error("the SDR's address (--sdr, or TAILCHASER_SDR), --freq and --trigger are needed")

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
            sys.exit(f"the receiver is not on '{want[0]['name']}' now — sdrlag does not change that for others")
    hz = a.freq * 1e6
    if abs(hz - rx.config["center_freq"]) > rx.config["samp_rate"] / 2 - 10000:
        sys.exit("that frequency is not in what the SDR shows now")

    rx.tune(hz)
    rx.pump(1.5)
    rx.audio = []
    marks = []

    def listen(seconds, until=None):
        end = time.monotonic() + seconds
        while time.monotonic() < end and not (until and until()):
            rx.pump(0.03)
            marks.append((time.monotonic(), len(rx.audio)))

    listen(2.0)                                         # the stream's clock, and the channel before the test
    if not rx.audio:
        sys.exit("the SDR sends no audio")
    if tone_onset([float(v) for v in rx.audio], sdr.RATE, a.tone) is not None:
        sys.exit(f"there is a {a.tone:.0f} Hz tone on the frequency already — wait until the channel is free")
    before = len(rx.audio)

    t_a = time.monotonic()
    proc = subprocess.Popen(a.trigger, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    listen(10.0, until=lambda: proc.poll() is not None)
    if proc.poll() is None:
        proc.kill()
        sys.exit("the trigger command does not come back")
    t_b = time.monotonic()
    if proc.returncode != 0:
        sys.exit(f"the trigger command failed (exit {proc.returncode}) — nothing was transmitted")
    took = t_b - t_a
    asked = (t_a + t_b) / 2                             # somewhere in there the node was told

    found = [None]

    def have():
        found[0] = tone_onset([float(v) for v in rx.audio[before:]], sdr.RATE, a.tone)
        return found[0] is not None
    listen(a.wait, until=have)
    if found[0] is None:
        sys.exit(f"no {a.tone:.0f} Hz tone within {a.wait:.0f} s — is the transmitter on that frequency, "
                 "does the SDR hear it, was the node free to transmit?")
    heard = stream_base(marks, sdr.RATE) + (before + found[0]) / sdr.RATE
    on_air = asked + (a.tx_delay_ms + SOUND_CARD_MS) / 1000.0
    sdr_ms = (heard - on_air) * 1000.0
    lag = int(round((sdr_ms + a.buffer_ms + a.ctcss_delay_ms) / 10.0)) * 10
    print(f"the tone was on the air   {a.tx_delay_ms + SOUND_CARD_MS} ms after it was asked for "
          f"(TX delay {a.tx_delay_ms} ms, sound card about {SOUND_CARD_MS} ms)")
    print(f"the web SDR brought it    {sdr_ms:.0f} ms later (its processing and the internet)")
    print(f"tailchaser-sdrreceiver    + {a.buffer_ms} ms ([sdr] buffer_ms)"
          + (f" + {a.ctcss_delay_ms} ms ([ctcss] delay_ms)" if a.ctcss_delay_ms else ""))
    print(f"that receiver is behind   {lag} ms   (give or take {max(50, int(took * 500))} ms)")
    if took > 0.25:
        print(f"  the trigger took {took:.1f} s: run sdrlag on the node itself, not over ssh, for an exact figure")
    if sdr_ms < 0 or lag > 10000:
        print("  that cannot be right — check --tx-delay-ms and that it was this node's tone")
        return 1
    print(f"\nOn the repeater or node, in the section of that receiver:\n\n    lag_ms        = {lag}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
