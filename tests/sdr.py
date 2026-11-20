#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""tailchaser-sdrreceiver against stand-ins for the four kinds of web SDR.

    python3 tests/sdr.py ./tailchaser-sdrreceiver ./opuspkt

Each stand-in speaks its protocol and sends the same thing: FM hiss, then
3 s of a quieted carrier with a tone on it, then hiss again. The receiver has
to find the channel, open its squelch for the carrier and only for it.
"""
import base64
import hashlib
import json
import math
import random
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

failed = 0
FREQ = 431.2e6


def check(ok, what):
    global failed
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        failed += 1


def programme(rate):
    """6 s of hiss, 3 s of carrier with a 1 kHz tone, hiss — for ever. Blocks of 20 ms."""
    rnd = random.Random(1)
    n, t = rate // 50, 0
    while True:
        sec = (t / rate) % 12.0
        if 6.0 <= sec < 9.0:
            blk = [int(6000 * math.sin(2 * math.pi * 1000 * (t + i) / rate) + rnd.gauss(0, 12)) for i in range(n)]
        else:
            blk = [max(-32768, min(32767, int(rnd.gauss(0, 5000)))) for _ in range(n)]
        t += n
        yield blk


class Ws:
    """The server side of one websocket."""

    def __init__(self, sock, head):
        self.s, self.buf = sock, b""
        key = [l.split(":", 1)[1].strip() for l in head.split("\r\n") if l.lower().startswith("sec-websocket-key")][0]
        acc = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        sock.sendall(f"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     f"Sec-WebSocket-Accept: {acc}\r\n\r\n".encode())
        self.lock = threading.Lock()

    def send(self, payload, binary=False):
        if isinstance(payload, str):
            payload = payload.encode()
        n = len(payload)
        hdr = bytes([0x82 if binary else 0x81]) + (bytes([n]) if n < 126 else b"\x7e" + struct.pack("!H", n))
        with self.lock:
            self.s.sendall(hdr + payload)

    def recv(self, timeout):
        """A text message, or None."""
        self.s.settimeout(timeout)
        try:
            while True:
                if len(self.buf) >= 2:
                    ln, off = self.buf[1] & 127, 2
                    if ln == 126 and len(self.buf) >= 4:
                        ln, off = struct.unpack("!H", self.buf[2:4])[0], 4
                    if len(self.buf) >= off + 4 + ln and ln != 126 or (off == 4 and len(self.buf) >= off + 4 + ln):
                        mask, body = self.buf[off:off + 4], self.buf[off + 4:off + 4 + ln]
                        op = self.buf[0] & 15
                        self.buf = self.buf[off + 4 + ln:]
                        if op == 8:
                            raise ConnectionError
                        if op == 1:
                            return bytes(c ^ mask[i % 4] for i, c in enumerate(body)).decode()
                        continue
                chunk = self.s.recv(4096)
                if not chunk:
                    raise ConnectionError
                self.buf += chunk
        except socket.timeout:
            return None


class Server:
    def __init__(self, handler):
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.handler, self.said, self.heads = handler, [], []
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while True:
            c, _ = self.sock.accept()
            threading.Thread(target=self.one, args=(c,), daemon=True).start()

    def one(self, c):
        try:
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = c.recv(4096)
                if not chunk:
                    return
                data += chunk
            head, rest = data.split(b"\r\n\r\n", 1)
            head = head.decode()
            self.heads.append(head)
            self.handler(self, c, head, rest)
        except (ConnectionError, OSError):
            pass
        finally:
            c.close()


def pump(srv, ws, rate, frame, on_text=None):
    """Real time: a block every 20 ms, and whatever the client says in between."""
    t0, k = time.monotonic(), 0
    for blk in programme(rate):
        ws.send(frame(blk), binary=True)
        k += 1
        while True:
            left = t0 + k * 0.02 - time.monotonic()
            m = ws.recv(max(0.001, left))
            if m is not None:
                srv.said.append(m)
                if on_text:
                    on_text(m)
            if left <= 0:
                break


def openwebrx(srv, c, head, rest):
    ws = Ws(c, head)
    ws.send("CLIENT DE SERVER server=openwebrx version=v1.2.2")
    profiles = [{"id": "hf|40m", "name": "40m"}, {"id": "vhf|2m", "name": "2m"}, {"id": "uhf|70cm-high", "name": "70cm-high"}]
    # this connection starts on the HF device; the UHF one is on 70 cm
    ws.send(json.dumps({"type": "config", "value": {"center_freq": 7100000, "samp_rate": 384000, "sdr_id": "hf",
                                                    "profile_id": "40m", "audio_compression": "none"}}))
    ws.send(json.dumps({"type": "profiles", "value": profiles}))
    started = False
    while not started:
        m = ws.recv(10)
        if m is None:
            return
        srv.said.append(m)
        if m.startswith("{"):
            j = json.loads(m)
            if j.get("type") == "setsdr":
                dev = j["params"]["sdr"]
                cf = {"hf": (7100000, "40m"), "vhf": (145000000, "2m"), "uhf": (432000000, "70cm-high")}[dev]
                ws.send(json.dumps({"type": "config", "value": {"center_freq": cf[0], "samp_rate": 2400000,
                                                                "sdr_id": dev, "profile_id": cf[1]}}))
            started = j.get("type") == "dspcontrol" and j.get("action") == "start"
    ws.send(json.dumps({"type": "smeter", "value": 1e-7}))
    pump(srv, ws, 12000, lambda b: b"\x02" + struct.pack(f"<{len(b)}h", *b))


def kiwi(srv, c, head, rest):
    ws = Ws(c, head)
    stage = 0
    while stage < 2:
        m = ws.recv(10)
        if m is None:
            return
        srv.said.append(m)
        if m.startswith("SET auth"):
            ws.send(b"MSG freq_offset=0.000", binary=True)
            ws.send(b"MSG audio_init=0 audio_rate=12000", binary=True)
        elif m.startswith("SET AR OK"):
            ws.send(b"MSG sample_rate=11998.900", binary=True)
            stage = 1
        elif m.startswith("SET mod="):
            stage = 2
    seq = [0]

    def frame(b):
        seq[0] += 1
        return b"SND" + bytes([0]) + struct.pack("<I", seq[0]) + struct.pack(">H", 570) + struct.pack(f">{len(b)}h", *b)
    pump(srv, ws, 12000, frame)


def make_uber(opuspkt):
    raw = b"".join(struct.pack(f"<{len(b)}h", *b) for b, _ in zip(programme(24000), range(600)))
    out = subprocess.run([opuspkt, "24000"], input=raw, stdout=subprocess.PIPE, check=True).stdout
    pkts, i = [], 0
    while i + 2 <= len(out):
        n = out[i] | (out[i + 1] << 8)
        pkts.append(out[i + 2:i + 2 + n])
        i += 2 + n

    def uber(srv, c, head, rest):
        if head.startswith("POST /connection"):
            n = int([l.split(":")[1] for l in head.split("\r\n") if l.lower().startswith("content-length")][0])
            while len(rest) < n:
                rest += c.recv(4096)
            srv.said.append("POST " + rest.decode())
            body = json.dumps({"allowed": True, "session_timeout": 0, "max_session_time": 3600}).replace(" ", "")
            c.sendall(f"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\n"
                      f"Connection: close\r\n\r\n{body}".encode())
            return
        ws = Ws(c, head)
        t0, k = time.monotonic(), 0
        while True:
            p = pkts[k % len(pkts)]
            ws.send(struct.pack("<QIBff", int(time.time() * 1e9), 24000, 1, -40.0, -90.0) + p, binary=True)
            k += 1
            while True:
                left = t0 + k * 0.02 - time.monotonic()
                m = ws.recv(max(0.001, left))
                if m is not None:
                    srv.said.append(m)
                    if '"ping"' in m:
                        ws.send('{"type":"pong"}')
                if left <= 0:
                    break
    return uber


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def run(exe, kind, srv, tmp):
    hp = free_port()
    conf = f"{tmp}/{kind}.conf"
    with open(conf, "w") as f:
        f.write(f"""[sdr]
type = {kind}
url = http://127.0.0.1:{srv.port}
freq = 431.200
profile = 70cm
buffer_ms = 200
[uplink]
listen_port = {free_port()}
auth_key = k
[gate]
mode = off
[status]
http_port = {hp}
bind = 127.0.0.1
admin = false
""")
    log = open(f"{tmp}/{kind}.log", "w")
    proc = subprocess.Popen([exe, "-c", conf], stderr=log)
    seen, t0 = [], time.monotonic()
    try:
        while time.monotonic() - t0 < 14.5:
            time.sleep(0.1)
            try:
                st = json.load(urllib.request.urlopen(f"http://127.0.0.1:{hp}/status.json", timeout=1))
            except OSError:
                continue
            seen.append((time.monotonic() - t0, st))
    finally:
        proc.terminate()
        proc.wait(timeout=5)
    return seen, open(f"{tmp}/{kind}.log").read()


def judge(kind, seen, log):
    check(any(s["sdr"]["listening"] for _, s in seen), f"{kind}: it connects and listens ({seen[-1][1]['sdr']['state'] if seen else 'no status'})")
    opens = [t for t, s in seen if s["gate"]["cos"]]
    dur = opens[-1] - opens[0] if opens else 0.0
    spans = sum(1 for (_, a), (_, b) in zip(seen, seen[1:]) if b["gate"]["cos"] and not a["gate"]["cos"])
    check(spans == 1 and 2.5 <= dur <= 3.4, f"{kind}: the squelch opens once, for the carrier ({dur:.1f} s of 3.0, {spans} time(s))")
    lev = max((s["gate"]["rssi"] for _, s in seen if s["gate"]["cos"]), default=-1)
    check(lev >= 80, f"{kind}: a full-quieting signal is reported as strong (signal {lev} of 100)")
    check(seen and seen[-1][1]["sdr"]["underruns"] <= 1 and abs(seen[-1][1]["sdr"]["rate"] % 12000 - 6000) > 5990,
          f"{kind}: the audio keeps coming ({seen[-1][1]['sdr']['rate']} Hz in, {seen[-1][1]['sdr']['buffer_ms']} ms in store)")


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "./tailchaser-sdrreceiver"
    opuspkt = sys.argv[2] if len(sys.argv) > 2 else "./opuspkt"
    tmp = tempfile.mkdtemp(prefix="tailchaser-sdr-")
    print("a receiver on a web SDR")
    servers = {"openwebrx": Server(openwebrx), "kiwisdr": Server(kiwi), "web888": Server(kiwi), "ubersdr": Server(make_uber(opuspkt))}
    results = {}
    th = [threading.Thread(target=lambda k=k, s=s: results.__setitem__(k, run(exe, k, s, tmp))) for k, s in servers.items()]
    for t in th: t.start()
    for t in th: t.join()
    for kind, srv in servers.items():
        seen, log = results[kind]
        judge(kind, seen, log)
        said = srv.said
        if kind == "openwebrx":
            tune = [json.loads(m) for m in said if m.startswith("{") and '"offset_freq"' in m]
            check(any('"setsdr"' in m and '"uhf"' in m for m in said) and tune and tune[-1]["params"]["offset_freq"] == -800000
                  and tune[-1]["params"]["mod"] == "nfm" and tune[-1]["params"]["squelch_level"] == -150,
                  "openwebrx: it moves to the device that has the band and tunes there (offset -800 kHz, FM, squelch open)")
            check(not any("selectprofile" in m for m in said), "openwebrx: it never changes a profile")
        elif kind in ("kiwisdr", "web888"):
            mod = [m for m in said if m.startswith("SET mod=")]
            check(mod and mod[0] == "SET mod=nbfm low_cut=-4000 high_cut=4000 freq=431200.000" and
                  any(m == "SET compression=0" for m in said) and any(m == "SET keepalive" for m in said),
                  f"{kind}: narrow FM on 431200 kHz, uncompressed, kept alive")
        else:
            post = [m for m in said if m.startswith("POST ")]
            ws = [h.split("\r\n")[0] for h in srv.heads if h.startswith("GET /ws?")]
            sid = json.loads(post[0][5:])["user_session_id"] if post else "-"
            check(post and ws and f"user_session_id={sid}" in ws[0] and "frequency=431200000" in ws[0]
                  and "mode=nfm" in ws[0] and "format=opus" in ws[0]
                  and all("user-agent:" in h.lower() for h in srv.heads),
                  "ubersdr: a session is asked for first, then narrow FM in Opus on 431200000 Hz with that session")
    if failed:
        print(f"{failed} check(s) FAILED   (files kept in {tmp})")
        return 1
    print("all checks passed")
    subprocess.run(["rm", "-rf", tmp])
    return 0


if __name__ == "__main__":
    sys.exit(main())
