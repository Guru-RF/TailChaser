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

Only OpenWebRX receivers, and only the Python standard library. It listens;
it changes nothing on the SDR for anybody else.
"""
import argparse
import importlib.util
import json
import math
import os
import random
import subprocess
import sys
import time

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
    print("all checks passed" if not bad else f"{bad} check(s) FAILED")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("url", nargs="?", help="the web SDR (an OpenWebRX receiver that hears the transmitter)")
    ap.add_argument("--sdr", help="the same, as an option; or set TAILCHASER_SDR in the environment")
    ap.add_argument("--selftest", action="store_true", help="check the arithmetic on made-up signals, no SDR")
    ap.add_argument("--profile", help="part of the SDR profile's name the frequency is in, e.g. 70cm-high")
    ap.add_argument("--freq", type=float, help="the transmitter's frequency, MHz")
    ap.add_argument("--trigger", help="command that starts the test transmission, run here on the node")
    ap.add_argument("--tone", type=float, default=1000.0, help="the test tone, Hz (1000)")
    ap.add_argument("--tx-delay-ms", type=int, default=300, help="[tx] delay_ms of the node (300)")
    ap.add_argument("--buffer-ms", type=int, default=300, help="[sdr] buffer_ms of the tailchaser-sdrreceiver (300)")
    ap.add_argument("--wait", type=float, default=8.0, help="seconds to wait for the tone")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    a.url = a.sdr or a.url or os.environ.get("TAILCHASER_SDR")
    if not a.url or not a.freq or not a.trigger:
        ap.error("the SDR's address (--sdr, or TAILCHASER_SDR), --freq and --trigger are needed")

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
    lag = int(round((sdr_ms + a.buffer_ms) / 10.0)) * 10
    print(f"the tone was on the air   {a.tx_delay_ms + SOUND_CARD_MS} ms after it was asked for "
          f"(TX delay {a.tx_delay_ms} ms, sound card about {SOUND_CARD_MS} ms)")
    print(f"the web SDR brought it    {sdr_ms:.0f} ms later (its processing and the internet)")
    print(f"tailchaser-sdrreceiver    + {a.buffer_ms} ms ([sdr] buffer_ms)")
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
