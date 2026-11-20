#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""sdrmon — listen to repeaters on an OpenWebRX receiver and measure their audio.

    sdrmon.py URL                                  what the receiver offers: profiles, bookmarks
    sdrmon.py URL --profile 70cm-high              ...of that profile (see --switch)
    sdrmon.py URL --profile 70cm-high --listen ON0ORA,ON0BXL,ON0DEN --seconds 120 --wav DIR

The way to set a repeater's transmit audio without a deviation meter: listen
to it on a web receiver next to repeaters that are known to be right, and
compare. For every station listened to it prints, over the time a carrier was
there: the signal strength, the audio level of the loud parts and the peak,
how the level is spread over the audio band (bass / mid / treble — what
pre-emphasis changes) and the level of a CTCSS tone under it.

One station at a time, `--seconds` each, in turns of `--turn` seconds so that
all of them are heard during the same conversation.

A receiver with several SDR devices: sdrmon moves its own connection to the
device that has the profile, which disturbs nobody. A profile of a device is
shared by everybody listening to that device: sdrmon never changes one unless
told to with --switch (and puts it back afterwards, unless --stay).

The audio of an OpenWebRX receiver has passed its own de-emphasis and AGC:
levels compare between stations on the same receiver, they are not deviation
in kHz.

Standard library only. The protocol is OpenWebRX's own web client's
(websocket on <url>/ws/, IMA ADPCM audio at 12 kHz).
"""
import argparse
import base64
import json
import math
import os
import socket
import ssl
import struct
import sys
import time
import urllib.parse
import wave

RATE = 12000

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
        80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
        544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
        2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
        12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
ADJ = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


class Adpcm:
    """csdr's AdpcmEncoder(sync=True), decoded: IMA ADPCM, low nibble first, with
    "SYNC" + index + predictor (int16 LE each) before the first byte and after
    every 1001 bytes of codes."""

    def __init__(self, plain=False):
        self.index = self.prev = 0
        self.buf = b""
        self.synced = False
        self.left = 0
        self.plain = plain      # OpenWebRX 1.0 / 1.1: the same codec from 0 / 0, no SYNC at all

    def _nibble(self, code, out):
        step = STEP[self.index]
        diff = step >> 3
        if code & 1: diff += step >> 2
        if code & 2: diff += step >> 1
        if code & 4: diff += step
        self.prev = max(-32768, min(32767, self.prev - diff if code & 8 else self.prev + diff))
        self.index = max(0, min(88, self.index + ADJ[code]))
        out.append(self.prev)

    def feed(self, data):
        out = []
        if self.plain:
            for b in data:
                self._nibble(b & 15, out)
                self._nibble(b >> 4, out)
            return out
        self.buf += data
        while True:
            if not self.synced or self.left == 0:
                if not self.synced:
                    i = self.buf.find(b"SYNC")
                    if i < 0:
                        self.buf = self.buf[-3:]
                        break
                    self.buf = self.buf[i:]
                if len(self.buf) < 8:
                    break
                if self.buf[:4] != b"SYNC":
                    self.synced = False
                    continue
                self.index, self.prev = struct.unpack("<hh", self.buf[4:8])
                self.index = max(0, min(88, self.index))
                self.buf = self.buf[8:]
                self.synced, self.left = True, 1001
            n = min(self.left, len(self.buf))
            if n == 0:
                break
            for b in self.buf[:n]:
                self._nibble(b & 15, out)
                self._nibble(b >> 4, out)
            self.buf = self.buf[n:]
            self.left -= n
        return out


class Ws:
    def __init__(self, url):
        u = urllib.parse.urlparse(url if "://" in url else "http://" + url)
        tls = u.scheme in ("https", "wss")
        host, port = u.hostname, u.port or (443 if tls else 80)
        path = (u.path if u.path.endswith("/") else u.path + "/") + "ws/"
        s = socket.create_connection((host, port), timeout=10)
        if tls:
            s = ssl.create_default_context().wrap_socket(s, server_hostname=host)
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall((f"GET {path} HTTP/1.1\r\nHost: {u.netloc}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nOrigin: {u.scheme}://{u.netloc}\r\n"
                   "User-Agent: sdrmon\r\n\r\n").encode())
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = s.recv(4096)
            if not chunk:
                raise RuntimeError("closed during the websocket handshake")
            data += chunk
        head, self.buf = data.split(b"\r\n\r\n", 1)
        if b" 101" not in head.split(b"\r\n", 1)[0]:
            raise RuntimeError("no websocket here: " + head.split(b"\r\n", 1)[0].decode())
        self.s = s
        self.frag = None

    def send(self, payload, op=1):
        if isinstance(payload, str):
            payload = payload.encode()
        mask = os.urandom(4)
        n = len(payload)
        hdr = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack("!H", n))
        self.s.sendall(hdr + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(payload)))

    def _need(self, n, deadline):
        while len(self.buf) < n:
            left = deadline - time.monotonic()
            if left <= 0:
                return False
            self.s.settimeout(left)
            try:
                chunk = self.s.recv(65536)
            except (socket.timeout, ssl.SSLWantReadError):
                return False
            if not chunk:
                raise ConnectionError("the receiver closed the connection")
            self.buf += chunk
        return True

    def recv(self, timeout):
        """(opcode, payload) of the next message — 1 text, 2 binary — or None."""
        deadline = time.monotonic() + timeout
        while True:
            if not self._need(2, deadline):
                return None
            fin, op, ln, off = self.buf[0] & 0x80, self.buf[0] & 15, self.buf[1] & 127, 2
            if ln == 126:
                if not self._need(4, deadline): return None
                ln, off = struct.unpack("!H", self.buf[2:4])[0], 4
            elif ln == 127:
                if not self._need(10, deadline): return None
                ln, off = struct.unpack("!Q", self.buf[2:10])[0], 10
            if not self._need(off + ln, deadline + 5):
                return None
            payload, self.buf = self.buf[off:off + ln], self.buf[off + ln:]
            if op == 9:
                self.send(payload, 10)
                continue
            if op == 8:
                raise ConnectionError("the receiver closed the connection")
            if op == 10:
                continue
            if op == 0:
                if self.frag is None: continue
                self.frag[1] += payload
            else:
                self.frag = [op, bytearray(payload)]
            if fin:
                m, self.frag = self.frag, None
                return m[0], bytes(m[1])


class Receiver:
    def __init__(self, url):
        self.ws = Ws(url)
        self.config, self.profiles, self.bookmarks = {}, [], []
        self.smeter = 0.0
        self.clients = 0
        self.server = ""
        self.adpcm = None
        self.audio = []
        self.ws.send("SERVER DE CLIENT client=sdrmon type=receiver")
        self.ws.send(json.dumps({"type": "connectionproperties", "params": {"output_rate": RATE, "hd_output_rate": 48000}}))

    def pump(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            m = self.ws.recv(min(0.25, max(0.01, end - time.monotonic())))
            if m is None:
                continue
            op, pl = m
            if op == 1:
                if pl.startswith(b"CLIENT DE SERVER"):
                    self.server = pl.decode(errors="replace")
                    ver = self.server.rsplit("version=v", 1)[-1].split(".")
                    self.adpcm = Adpcm(plain=ver[:2] in (["1", "0"], ["1", "1"]) or ver[0] == "0")
                    continue
                try:
                    j = json.loads(pl)
                except ValueError:
                    continue
                t, v = j.get("type"), j.get("value")
                if t == "config":
                    self.config.update(v or {})
                elif t == "profiles":
                    self.profiles = v or []
                elif t == "bookmarks":
                    self.bookmarks = v or []
                elif t == "smeter":
                    self.smeter = float(v)
                elif t == "clients":
                    self.clients = int(v)
                elif t == "backoff":
                    raise ConnectionError("the receiver refuses: " + str(j.get("reason")))
                elif t in ("sdr_error", "demodulator_error"):
                    print(f"  receiver says: {v}", file=sys.stderr)
            elif op == 2 and pl and pl[0] == 2:
                if self.config.get("audio_compression") == "adpcm":
                    self.audio += (self.adpcm or Adpcm()).feed(pl[1:])
                else:
                    self.audio += [x[0] for x in struct.iter_unpack("<h", pl[1:len(pl) - (len(pl) - 1) % 2])]

    def profile_id(self):
        return f"{self.config.get('sdr_id')}|{self.config.get('profile_id')}"

    def tune(self, hz):
        off = int(hz - self.config["center_freq"])
        self.ws.send(json.dumps({"type": "dspcontrol", "params": {
            "low_cut": -4000, "high_cut": 4000, "offset_freq": off, "mod": "nfm",
            "squelch_level": -150, "secondary_mod": False}}))
        self.ws.send(json.dumps({"type": "dspcontrol", "action": "start"}))


def db(x, ref=32768.0):
    return 20 * math.log10(max(x, 1e-9) / ref)


def goertzel(x, f):
    w = 2 * math.cos(2 * math.pi * f / RATE)
    a = b = 0.0
    for v in x:
        a, b = v + w * a - b, a
    return math.sqrt(max(a * a + b * b - w * a * b, 0.0)) * 2 / max(len(x), 1)


class Biquad:
    def __init__(self, kind, f, q=0.707):
        w = 2 * math.pi * f / RATE
        al, c = math.sin(w) / (2 * q), math.cos(w)
        if kind == "lp":
            b = [(1 - c) / 2, 1 - c, (1 - c) / 2]
        else:
            b = [(1 + c) / 2, -(1 + c), (1 + c) / 2]
        a0 = 1 + al
        self.b = [v / a0 for v in b]
        self.a = [-2 * c / a0, (1 - al) / a0]
        self.z = [0.0, 0.0]

    def run(self, x):
        b0, b1, b2 = self.b
        a1, a2 = self.a
        z1, z2 = self.z
        out = []
        for v in x:
            y = b0 * v + z1
            z1 = b1 * v - a1 * y + z2
            z2 = b2 * v - a2 * y
            out.append(y)
        self.z = [z1, z2]
        return out


def band(x, lo, hi):
    y = Biquad("hp", lo).run(Biquad("hp", lo).run(x))
    return Biquad("lp", hi).run(Biquad("lp", hi).run(y))


def rms(x):
    return math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0


class Stats:
    """What was heard of one station."""

    def __init__(self, name, hz, tone):
        self.name, self.hz, self.tone = name, hz, tone
        self.pcm = []           # audio while a carrier was there
        self.sig = []           # S-meter readings (dB) while a carrier was there
        self.noise = []         # ...and while there was none
        self.all = []

    def report(self):
        n = len(self.pcm)
        secs = n / RATE
        line = f"{self.name:<8} {self.hz / 1e6:9.4f} MHz  carrier {secs:5.0f} s"
        if secs < 2:
            return line + "  — not heard"
        fr = RATE // 50
        voice = band(self.pcm, 300, 3000)
        frames = [rms(voice[i:i + fr]) for i in range(0, n - fr, fr)]
        top = sorted(frames)[int(len(frames) * 0.95)]
        loud = [f for f in frames if f >= top / 8]
        speech = math.sqrt(sum(f * f for f in loud) / len(loud)) if loud else 0.0
        active = 100.0 * len(loud) / len(frames)
        peak = max(abs(v) for v in self.pcm)
        lo, mid, hi = rms(band(self.pcm, 300, 700)), rms(band(self.pcm, 700, 1500)), rms(band(self.pcm, 1500, 3000))
        line += (f"  signal {sum(self.sig) / len(self.sig):6.1f} dB"
                 f"  speech {db(speech):6.1f} dBFS  peak {db(peak):6.1f}  loud {active:3.0f} %"
                 f"  bass/mid/treble {db(lo):5.1f} / {db(mid):5.1f} / {db(hi):5.1f}")
        if self.tone:
            t = [goertzel(self.pcm[i:i + RATE], self.tone) for i in range(0, n - RATE, RATE)]
            if t:
                line += f"  tone {self.tone:g} Hz {db(sorted(t)[len(t) // 2] / math.sqrt(2)):6.1f}"
        return line


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("url")
    ap.add_argument("--profile", help="part of the profile's name, e.g. 70cm-high")
    ap.add_argument("--switch", action="store_true",
                    help="select that profile if the receiver is on another one — for EVERYBODY listening to it")
    ap.add_argument("--stay", action="store_true",
                    help="after --switch, leave the receiver on that profile (default: put it back as it was)")
    ap.add_argument("--listen", help="bookmark names (or NAME=MHz), comma separated")
    ap.add_argument("--seconds", type=float, default=60, help="per station, in all")
    ap.add_argument("--turn", type=float, default=20, help="seconds per station per round")
    ap.add_argument("--carrier-db", type=float, default=10.0, help="S-meter this far above the quietest reading = a carrier")
    ap.add_argument("--tone", type=float, default=0.0, help="CTCSS tone to measure, Hz (same for all; NAME:tone overrides)")
    ap.add_argument("--wav", help="directory for one WAV per station (audio while a carrier was there)")
    a = ap.parse_args()

    rx = Receiver(a.url)
    rx.pump(4)
    if "center_freq" not in rx.config:
        rx.pump(4)
    print(f"{rx.server}  — {rx.clients} listener(s), this one included")
    cur = rx.profile_id()
    names = {p["id"]: p["name"] for p in rx.profiles}
    print(f"profile now: {names.get(cur, cur)}  (centre {rx.config.get('center_freq', 0) / 1e6:.4f} MHz, "
          f"{rx.config.get('samp_rate', 0) / 1e6:.3f} MHz wide, audio {rx.config.get('audio_compression')})")

    switched = False
    if a.profile:
        want = [p for p in rx.profiles if a.profile.lower() in p["name"].lower()]
        if not want:
            sys.exit(f"no profile named like '{a.profile}'. There are: " + ", ".join(names.values()))
        if want[0]["id"] != cur:
            sdr = want[0]["id"].split("|")[0]
            if sdr != cur.split("|")[0]:
                # Another SDR device: go and listen there, on whatever profile it
                # is on. That moves this connection only and changes nothing.
                rx.ws.send(json.dumps({"type": "setsdr", "params": {"sdr": sdr}}))
                rx.pump(5)
        if want[0]["id"] != rx.profile_id():
            if not a.switch:
                sys.exit(f"the receiver is not on '{want[0]['name']}' (it is on "
                         f"'{names.get(rx.profile_id(), rx.profile_id())}'). --switch selects it — "
                         "for everybody listening to that SDR.")
            print(f"selecting profile: {want[0]['name']}")
            rx.ws.send(json.dumps({"type": "selectprofile", "params": {"profile": want[0]["id"]}}))
            rx.pump(6)
            if rx.profile_id() != want[0]["id"]:
                sys.exit("the receiver did not change profile")
            switched = True
        print(f"listening on: {names.get(rx.profile_id())}  (centre {rx.config.get('center_freq', 0) / 1e6:.4f} MHz)")
    if not a.listen:
        print("profiles:")
        for p in rx.profiles:
            print(f"  {'*' if p['id'] == rx.profile_id() else ' '} {p['name']}")
        print("bookmarks of this profile:")
        for b in sorted(rx.bookmarks, key=lambda b: b["frequency"]):
            print(f"    {b['frequency'] / 1e6:9.4f}  {b.get('modulation', ''):<5} {b['name']}")
        return

    stations = []
    for item in a.listen.split(","):
        tone = a.tone
        if ":" in item:
            item, t = item.split(":", 1)
            tone = float(t)
        if "=" in item:
            name, mhz = item.split("=", 1)
            hz = float(mhz) * 1e6
        else:
            hit = [b for b in rx.bookmarks if b["name"].strip().lower() == item.strip().lower()] or \
                  [b for b in rx.bookmarks if item.strip().lower() in b["name"].lower()]
            if not hit:
                sys.exit(f"no bookmark '{item}' in this profile")
            name, hz = hit[0]["name"].strip(), hit[0]["frequency"]
        stations.append(Stats(name, hz, tone))

    rounds = max(1, int(math.ceil(a.seconds / a.turn)))
    for r in range(rounds):
        for st in stations:
            rx.tune(st.hz)
            rx.pump(1.0)                    # the demodulator settles, the old station's audio drains
            rx.audio = []
            t_end = time.monotonic() + a.turn
            while time.monotonic() < t_end:
                rx.audio = []
                rx.pump(0.5)
                s_db = 10 * math.log10(max(rx.smeter, 1e-20))
                st.all.append((s_db, rx.audio))
            # a carrier = clearly above the quietest this channel has been
            floor = min(s for s, _ in st.all)
            st.pcm, st.sig = [], []
            for s_db, pcm in st.all:
                if s_db >= floor + a.carrier_db:
                    st.pcm += pcm
                    st.sig.append(s_db)
            print(f"  round {r + 1}/{rounds}  {st.name:<8} carrier so far {len(st.pcm) / RATE:5.0f} s "
                  f"(floor {floor:.0f} dB)", flush=True)
    if switched and not a.stay:
        print(f"putting the receiver back on: {names.get(cur, cur)}")
        rx.ws.send(json.dumps({"type": "selectprofile", "params": {"profile": cur}}))
        rx.pump(4)
    print()
    for st in stations:
        print(st.report())
        if a.wav and st.pcm:
            os.makedirs(a.wav, exist_ok=True)
            with wave.open(os.path.join(a.wav, f"{st.name}.wav"), "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
                w.writeframes(struct.pack(f"<{len(st.pcm)}h", *st.pcm))


if __name__ == "__main__":
    try:
        main()
    except (ConnectionError, OSError, RuntimeError) as e:
        sys.exit(f"sdrmon: {e}")
