#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""The software tone squelch: offline on generated overs, then in a receiver
that has nothing but audio.

    python3 tests/ctcss.py ./ctcsstest ./tailchaser-receiver

A station with the right sub-audible tone must open the squelch, for the whole
over and no longer. The same voice without a tone, with the next tone in the
CTCSS table, and the noise of an empty channel must not.
"""
import json
import math
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_receiver import FS, KEY, Repeater, msg, noise, voice, write_wav   # noqa: E402

TONE = 79.7
OPEN = re.compile(r"open ([\d.]+) to ([\d.]+) s\s+level (-?[\d.]+) dBFS\s+snr (-?[\d.]+) dB")
DELAY = 0.3


def sine(secs, f, level):
    return [level * math.sin(2 * math.pi * f * i / FS) for i in range(int(secs * FS))]


def mix(*parts):
    return [sum(x) for x in zip(*parts)]


def talker(secs, level=0.3):
    v = voice(secs)
    top = max(abs(x) for x in v)
    return [x * level / top for x in v]


def goertzel(x, f):
    """Amplitude of the component at f."""
    re_ = sum(v * math.cos(2 * math.pi * f * i / FS) for i, v in enumerate(x))
    im_ = sum(v * math.sin(2 * math.pi * f * i / FS) for i, v in enumerate(x))
    return 2 * math.hypot(re_, im_) / max(1, len(x))


def main():
    tool = sys.argv[1] if len(sys.argv) > 1 else "./ctcsstest"
    exe = sys.argv[2] if len(sys.argv) > 2 else "./tailchaser-receiver"
    tmp = tempfile.mkdtemp(prefix="tailchaser-ctcss-")
    failures = []

    def check(ok, what):
        print(("  ok   " if ok else "  FAIL ") + what)
        if not ok:
            failures.append(what)
        return ok

    # ---- the detector alone: 1 s of channel noise, 3 s of over, 1 s of noise ----
    hiss = noise(1.0, 0.1)
    v = talker(3.0)
    overs = {
        "right":    mix(v, sine(3.0, TONE, 0.03), noise(3.0, 0.005)),
        "weak":     mix(v, sine(3.0, TONE, 0.01), noise(3.0, 0.03)),
        "offtune":  mix(v, sine(3.0, TONE + 0.6, 0.03), noise(3.0, 0.005)),
        "no tone":  mix(v, noise(3.0, 0.005)),
        "82.5 Hz":  mix(v, sine(3.0, 82.5, 0.03), noise(3.0, 0.005)),
        "77.0 Hz":  mix(v, sine(3.0, 77.0, 0.03), noise(3.0, 0.005)),
        "131.8 Hz": mix(v, sine(3.0, 131.8, 0.03), noise(3.0, 0.005)),
        "noise":    noise(3.0, 0.2),
    }

    def run(name, f=TONE):
        path = f"{tmp}/{name.replace(' ', '_')}.wav"
        write_wav(path, hiss + overs[name] + hiss)
        out = subprocess.run([tool, "-f", str(f), path], capture_output=True, text=True, check=True).stdout
        return [tuple(float(x) for x in m.groups()) for m in OPEN.finditer(out)]

    for name, what in (("right", "a station with the tone"),
                       ("weak", "a weak one: the tone 10 dB down, in the noise"),
                       ("offtune", "a tone that is 0.6 Hz off")):
        r = run(name)
        ok = len(r) == 1 and abs(r[0][0] - (1.0 + DELAY)) <= 0.15 and abs(r[0][1] - (4.0 + DELAY)) <= 0.15
        got = f"{r[0][0] - DELAY:.2f} to {r[0][1] - DELAY:.2f} s of 1.00 to 4.00, snr {r[0][3]:.0f} dB" if r else "never"
        check(ok, f"{what} opens the squelch for the over and no longer ({got})")
    for name, what in (("no tone", "the same voice without a tone"), ("82.5 Hz", "with the next tone up, 82.5 Hz"),
                       ("77.0 Hz", "with the next tone down, 77.0 Hz"), ("131.8 Hz", "with another tone, 131.8 Hz"),
                       ("noise", "the noise of an empty channel")):
        check(len(run(name)) == 0, f"{what}: stays closed")
    r = run("131.8 Hz", 131.8)
    check(len(r) == 1, "a decoder set to 131.8 Hz does open for that one")

    # ---- a receiver with nothing but audio: no SA818, no squelch line ----
    clips = [("with", overs["right"]), ("without", overs["no tone"]), ("other", overs["82.5 Hz"]),
             ("hiss", noise(2.0, 0.2))]
    for name, samples in clips:
        write_wav(f"{tmp}/{name}.wav", samples)
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    http_port = port + 1
    with open(f"{tmp}/receiver.conf", "w") as f:
        f.write(f"""[uplink]
listen_port = {port}
auth_key = {KEY}
proto_minor = 7
[radio]
type = none
cos_line = none
[ctcss]
detect = true
tone = {TONE}
[siglev]
mode = noise
noise_open_db = -25
noise_full_db = -51
[audio]
alsa_device = file:{','.join(f'{tmp}/{n}.wav' for n, _ in clips)}
normalize = false
[gate]
mode = on
hold_s = 1
[status]
http_port = {http_port}
bind = 127.0.0.1
""")
    t = subprocess.run([exe, "-t", "-c", f"{tmp}/receiver.conf"], capture_output=True, text=True)
    check(t.returncode == 0 and "tone squelch in software" in t.stderr and "no squelch line" in t.stderr,
          "a receiver without SA818 and without squelch line is a valid configuration")
    bad = f"{tmp}/bad.conf"
    with open(bad, "w") as f:
        f.write(open(f"{tmp}/receiver.conf").read().replace("detect = true", "detect = false"))
    t = subprocess.run([exe, "-t", "-c", bad], capture_output=True, text=True)
    check(t.returncode != 0 and "cos_line is required" in t.stderr,
          "...but without the tone squelch it is refused: nothing would open the squelch")

    proc = subprocess.Popen([exe, "-c", f"{tmp}/receiver.conf"], stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.3)
        rep = Repeater(port)
        _, ok = rep.login(KEY)
        rep.select_codec("RAW")
        rep.set_mute(0)
        overs_rx, pcm, t0, last_hb = [], [], time.time(), time.time()
        siglevs = []
        seen_open = False
        total = 1.0 + sum(len(s) / FS + 3.0 for _, s in clips) + 1.5
        while time.time() - t0 < total:
            if time.time() - last_hb > 5:
                rep.s.sendall(msg(1))
                last_hb = time.time()
            if not seen_open and 1.5 < time.time() - t0 < 3.5:
                st = json.load(urllib.request.urlopen(f"http://127.0.0.1:{http_port}/status.json", timeout=3))
                seen_open = st["ctcss"]["open"] and st["ctcss"]["snr_db"] > 15
            try:
                mtype, pl = rep.recv_msg(timeout=0.5)
            except socket.timeout:
                continue
            if mtype is None:
                break
            if mtype == 250:
                is_open, lev = struct.unpack("<?f", pl[:5])
                siglevs.append(lev)
                if is_open:
                    overs_rx.append([time.time() - t0, None])
                elif overs_rx:
                    overs_rx[-1][1] = time.time() - t0
            elif mtype == 254:
                siglevs.append(struct.unpack("<f", pl[:4])[0])
            elif mtype == 102 and overs_rx and len(overs_rx) == 1:
                n = struct.unpack("<i", pl[:4])[0]
                pcm.extend(struct.unpack(f"<{n // 4}f", pl[4:4 + n]))
        check(len(overs_rx) == 1, f"of four openings only the one with the tone reached the repeater (got {len(overs_rx)})")
        secs = len(pcm) / FS
        check(abs(secs - 3.0) <= 0.25, f"...whole: {secs:.2f} s of 3.00 s")
        if pcm:
            mid = pcm[len(pcm) // 4: 3 * len(pcm) // 4]
            scale = max(abs(x) for x in pcm) or 1.0
            left = goertzel(mid, TONE) / scale * 0.3 if scale > 2 else goertzel(mid, TONE)
            check(left < 0.003, f"...and without the tone: {20 * math.log10(left / 0.03 + 1e-9):.0f} dB of it is left")
        check(seen_open, "the status page shows the tone while it is there")
        best = max(siglevs) if siglevs else -1
        check(len(siglevs) >= 3 and best >= 80, f"a signal level from the receiver noise went to the repeater: "
              f"up to {best:.0f} of 100 for a clean station ({len(siglevs)} reports)")
        hist = json.load(urllib.request.urlopen(f"http://127.0.0.1:{http_port}/status.json", timeout=3))
        check(hist["noise"]["meter"] and hist["gate"]["rssi_hi"] == 100 and hist["gate"]["rssi_lo"] == 0,
              "...and the S-meter on the status page runs on it")
        st = json.load(urllib.request.urlopen(f"http://127.0.0.1:{http_port}/status.json", timeout=3))
        check(st["ctcss"]["detect"] and not st["ctcss"]["open"] and abs(st["ctcss"]["tone_hz"] - TONE) < 0.05
              and st["radio"]["ctcss_rx"] == "79.7 sw",
              "...and afterwards: closed, 79.7 Hz, decoded in software")
        rep.s.close()
    finally:
        proc.terminate()
        log = proc.stderr.read()
        proc.wait()
    if failures:
        print("\n--- tailchaser-receiver log ---\n" + log)
        print(f"\n{len(failures)} check(s) FAILED   (files kept in {tmp})")
        sys.exit(1)
    print("\nall checks passed")


if __name__ == "__main__":
    main()
