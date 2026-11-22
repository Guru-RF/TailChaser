#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""End-to-end test of tailchaser-receiver without any hardware.

    python3 tests/e2e_receiver.py ./tailchaser-receiver

tailchaser-receiver is started with file audio input: a playlist of generated WAV files,
each played in real time as one squelch opening. This script plays the part of
the repeater — SvxLink's Net receiver — and speaks the RemoteTrx protocol to
it: version check, HMAC-SHA1 login, codec selection, un-mute. It then checks
that the repeater is told about exactly the openings that carry a voice or
DTMF, and that the audio arrives complete.

Standard library only. Takes about 25 seconds (the audio plays in real time).
"""
import hashlib
import hmac
import json
import math
import os
import random
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
import wave

FS = 16000
KEY = "e2e-test-key"

MSG = {0: "ProtoVer", 1: "Heartbeat", 10: "AuthChallenge", 12: "AuthOk", 102: "Audio",
       250: "Squelch", 251: "Dtmf", 254: "SiglevUpdate"}


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(s * 32767))))
                               for s in samples))


def noise(secs, level=0.1):
    rnd = random.Random(1)
    return [rnd.gauss(0, level) for _ in range(int(secs * FS))]


def tone(secs, f=1750.0, level=0.2):
    return [level * math.sin(2 * math.pi * f * i / FS) for i in range(int(secs * FS))]


def voice(secs):
    """A crude talker: a glottal pulse train with a wandering pitch, through two
    formant resonators, in syllables."""
    rnd = random.Random(2)
    out, phase = [], 0.0
    res = []
    for fc, bw in ((700.0, 110.0), (1250.0, 140.0)):
        r = math.exp(-math.pi * bw / FS)
        res.append([2 * r * math.cos(2 * math.pi * fc / FS), -r * r, 0.0, 0.0])
    for i in range(int(secs * FS)):
        t = i / FS
        f0 = 125 + 28 * math.sin(2 * math.pi * 0.7 * t) + 9 * math.sin(2 * math.pi * 2.3 * t)
        phase += f0 / FS
        x = 0.0
        if phase >= 1.0:
            phase -= 1.0
            x = 1.0
        y = 0.0
        for r in res:
            v = x + r[0] * r[2] + r[1] * r[3]
            r[3], r[2] = r[2], v
            y += v
        env = max(0.0, math.sin(2 * math.pi * 1.6 * t)) ** 0.5
        out.append(0.12 * y * env + rnd.gauss(0, 0.002))
    return out


def dtmf(digits="15#"):
    lo = {"1": 697, "2": 697, "3": 697, "4": 770, "5": 770, "6": 770,
          "7": 852, "8": 852, "9": 852, "*": 941, "0": 941, "#": 941}
    hi = {"1": 1209, "4": 1209, "7": 1209, "*": 1209, "2": 1336, "5": 1336,
          "8": 1336, "0": 1336, "3": 1477, "6": 1477, "9": 1477, "#": 1477}
    out = [0.0] * int(0.15 * FS)
    for d in digits:
        out += [0.15 * (math.sin(2 * math.pi * lo[d] * i / FS) +
                        math.sin(2 * math.pi * hi[d] * i / FS)) for i in range(int(0.2 * FS))]
        out += [0.0] * int(0.2 * FS)
    return out


def msg(mtype, payload=b""):
    return struct.pack("<II", mtype, 8 + len(payload)) + payload


class Repeater:
    """The repeater's side of the link."""

    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""

    def recv_msg(self, timeout=5.0):
        self.s.settimeout(timeout)
        while True:
            if len(self.buf) >= 8:
                mtype, size = struct.unpack("<II", self.buf[:8])
                if len(self.buf) >= size:
                    payload, self.buf = self.buf[8:size], self.buf[size:]
                    return mtype, payload
            chunk = self.s.recv(65536)
            if not chunk:
                return None, None
            self.buf += chunk

    def login(self, key):
        mtype, pl = self.recv_msg()
        assert mtype == 0, f"expected ProtoVer, got {mtype}"
        ver = struct.unpack("<HH", pl)
        mtype, pl = self.recv_msg()
        assert mtype == 10 and len(pl) == 20, "expected a 20-byte AuthChallenge"
        self.s.sendall(msg(11, hmac.new(key.encode(), pl, hashlib.sha1).digest()))
        mtype, pl = self.recv_msg()
        return ver, mtype == 12

    def select_codec(self, name):
        self.s.sendall(msg(100, name.encode().ljust(32, b"\0") + b"\0" + b"\0" * 256))

    def set_mute(self, state):
        self.s.sendall(msg(200, struct.pack("<i", state)))


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "./tailchaser-receiver"
    tmp = tempfile.mkdtemp(prefix="tailchaser-e2e-rx-")
    clips = [("noise", noise(0.5)), ("tone", tone(1.0)), ("voice", voice(3.0)),
             ("noise2", noise(0.5)), ("dtmf", dtmf())]
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
freq_rx = 431.2000
[audio]
alsa_device = file:{','.join(f'{tmp}/{n}.wav' for n, _ in clips)}
[gate]
mode = on
hold_s = 1
[status]
http_port = {http_port}
bind = 127.0.0.1
""")
    proc = subprocess.Popen([exe, "-c", f"{tmp}/receiver.conf"], stderr=subprocess.PIPE, text=True)
    failures = []

    def check(ok, what):
        print(("  ok   " if ok else "  FAIL ") + what)
        if not ok:
            failures.append(what)

    try:
        time.sleep(0.3)
        bad = Repeater(port)
        _, ok = bad.login("wrong-key")
        check(not ok, "a wrong auth key is refused")
        bad.s.close()

        rep = Repeater(port)
        ver, ok = rep.login(KEY)
        check(ver == (2, 7), f"protocol version announced is 2.7 (got {ver[0]}.{ver[1]})")
        check(ok, "the right auth key is accepted")
        rep.select_codec("RAW")
        rep.set_mute(0)

        events, audio_bytes, t0 = [], 0, time.time()
        overs = []                       # [open time, close time, audio bytes]
        digits = []
        total = 1.0 + sum(len(s) / FS + 3.0 for _, s in clips) + 1.5
        last_hb = time.time()
        while time.time() - t0 < total:
            if time.time() - last_hb > 5:          # the repeater sends heartbeats too
                rep.s.sendall(msg(1))
                last_hb = time.time()
            try:
                mtype, pl = rep.recv_msg(timeout=1.0)
            except socket.timeout:
                continue
            if mtype is None:
                break
            t = time.time() - t0
            if mtype == 250:
                is_open, siglev, rx_id = struct.unpack("<?fc", pl[:6])
                check(len(pl) == 6, "squelch message has the protocol 2.7 layout (14 bytes)") \
                    if not events else None
                events.append((t, is_open))
                if is_open:
                    overs.append([t, None, 0])
                elif overs:
                    overs[-1][1] = t
            elif mtype == 102:
                n = struct.unpack("<i", pl[:4])[0]
                if n != len(pl) - 4:
                    failures.append("audio message length field is wrong")
                if overs and overs[-1][1] is None:
                    overs[-1][2] += n
                else:
                    failures.append("audio arrived while the squelch was closed")
            elif mtype == 251:
                d, dur = struct.unpack("<ci", pl[:5])
                digits.append(d.decode())
        for o in overs:
            print(f"       over: open {o[0]:.2f}s, close {o[1] if o[1] is None else round(o[1], 2)}s, "
                  f"{o[2] / 4 / FS:.2f}s of audio")
        check(len(overs) == 2, f"the repeater was opened twice — voice and DTMF — not for "
                               f"noise or a 1750 Hz tone (got {len(overs)})")
        if len(overs) >= 1:
            secs = overs[0][2] / 4 / FS
            check(2.6 <= secs <= 3.05, f"the spoken over arrived, up to where the gate was when the station unkeyed: {secs:.2f}s of 3.00s")
            check(overs[0][1] is not None, "the spoken over was closed again")
            if overs[0][1] is not None:
                took = overs[0][1] - overs[0][0]
                check(took < secs + 0.2, f"...and closed at once, without a tail of silence ({took:.2f}s open for {secs:.2f}s of audio)")
        check(digits == list("15#"), f"DTMF digits reported: {''.join(digits)!r} (sent '15#')")

        st = json.load(urllib.request.urlopen(f"http://127.0.0.1:{http_port}/status.json", timeout=3))
        tot = st["totals"]
        check(tot["openings"] == 5 and tot["passed"] == 2 and tot["blocked"] == 3,
              f"status page totals: {tot['openings']} openings, {tot['passed']} passed, "
              f"{tot['blocked']} blocked (want 5/2/3)")
        check(tot["blocked_noise"] == 2 and tot["blocked_tone"] == 1,
              f"blocked openings named correctly: {tot['blocked_noise']} noise, "
              f"{tot['blocked_tone']} tone (want 2/1)")
        check(st["uplink"]["connected"] and st["uplink"]["codec"] == "RAW",
              "status page shows the repeater connected with codec RAW")
        page = urllib.request.urlopen(f"http://127.0.0.1:{http_port}/", timeout=3).read()
        check(b"TailChaser" in page, "status page is served")
        rep.s.close()
    finally:
        proc.terminate()
        log = proc.stderr.read()
        proc.wait()
    if failures:
        print("\n--- tailchaser-receiver log ---\n" + log)
        print(f"\n{len(failures)} check(s) FAILED")
        sys.exit(1)
    print("\nall checks passed")


if __name__ == "__main__":
    main()
