#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""Listening in: /monitor/rx and /monitor/tx on the status port.

    python3 tests/monitor.py ./tailchaser-duplex

A repeater on file audio: a station on the input for 3 s. With
[status] monitor = true two websocket listeners hear the input and what is
transmitted; without it there is nothing to connect to.
"""
import base64
import json
import math
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_simplex import free_port, voice, write_wav  # noqa: E402

failed = 0


def check(ok, what):
    global failed
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        failed += 1


def listen(port, which, seconds, out):
    """Collect samples from /monitor/<which>; out gets (status line, [(time, samples)])."""
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(f"GET /monitor/{which} HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        c = s.recv(4096)
        if not c:
            break
        buf += c
    head, _, buf = buf.partition(b"\r\n\r\n")
    line = head.split(b"\r\n")[0].decode()
    got, t0 = [], time.monotonic()
    if " 101" in line:
        s.settimeout(0.5)
        while time.monotonic() - t0 < seconds:
            try:
                c = s.recv(65536)
            except socket.timeout:
                continue
            if not c:
                break
            buf += c
            while len(buf) >= 2:
                ln, off = buf[1] & 127, 2
                if ln == 126:
                    if len(buf) < 4:
                        break
                    ln, off = struct.unpack("!H", buf[2:4])[0], 4
                if len(buf) < off + ln:
                    break
                op, body, buf = buf[0] & 15, buf[off:off + ln], buf[off + ln:]
                if op == 2:
                    got.append((time.monotonic() - t0, struct.unpack(f"<{len(body) // 2}h", body)))
    s.close()
    out[which] = (line, got)


def rms_db(x):
    return 10 * math.log10(sum(v * v for v in x) / max(1, len(x)) / 32768.0 ** 2 + 1e-12)


def node(exe, tmp, wav, monitor):
    hp = free_port()
    conf = f"{tmp}/duplex-{monitor}.conf"
    with open(conf, "w") as f:
        f.write(f"""[reflector]
host = 127.0.0.1:1
callsign = ON0TST
auth_key = x
default_tg = 8
[radio]
type = none
cos_line = 12
ptt_line = 16
[ident]
callsign = ON0TST
interval_min = 0
[tx]
delay_ms = 200
hang_ms = 500
[gate]
mode = off
[audio]
alsa_device = file:pause:2000,{wav}
[status]
http_port = {hp}
bind = 127.0.0.1
admin = false
{'monitor = true' if monitor else ''}
""")
    proc = subprocess.Popen([exe, "-c", conf], stderr=open(f"{tmp}/node-{monitor}.log", "w"))
    for _ in range(50):
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{hp}/status.json", timeout=1).read()
            break
        except OSError:
            time.sleep(0.1)
    return proc, hp


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "./tailchaser-duplex"
    tmp = tempfile.mkdtemp(prefix="tailchaser-monitor-")
    base = voice(3.0)
    top = max(abs(x) for x in base)
    wav = f"{tmp}/v.wav"
    write_wav(wav, [x * 0.4 / top for x in base])
    print("listening in from the status page")

    off, hp0 = node(exe, tmp, wav, False)
    on, hp = node(exe, tmp, wav, True)
    try:
        out0 = {}
        listen(hp0, "rx", 0.5, out0)
        st0 = json.load(urllib.request.urlopen(f"http://127.0.0.1:{hp0}/status.json", timeout=2))
        check(" 404" in out0["rx"][0] and st0["monitor"] is False, "off unless it is turned on: nothing to connect to, and the page has no buttons")
        out = {}
        th = [threading.Thread(target=listen, args=(hp, w, 7.5, out)) for w in ("rx", "tx")]
        for t in th: t.start()
        time.sleep(0.5)
        page = urllib.request.urlopen(f"http://127.0.0.1:{hp}/", timeout=2).read().decode()
        st = json.load(urllib.request.urlopen(f"http://127.0.0.1:{hp}/status.json", timeout=2))
        check(st["monitor"] is True and "mon('rx')" in page and "mute('tx')" in page and "id=sp_rx" in page, "turned on: the status says so, the page offers it — and still answers while somebody listens")
        for t in th: t.join()
        rx = [v for _, c in out["rx"][1] for v in c]
        tx = [v for _, c in out["tx"][1] for v in c]
        check(" 101" in out["rx"][0] and 6.0 <= len(rx) / 16000 <= 8.0,
              f"the input: all the receiver gives, squelch or not, in real time ({len(rx) / 16000:.1f} s in 7.5 s)")
        loud = [i for i in range(0, len(rx) - 1600, 1600) if rms_db(rx[i:i + 1600]) > -50]
        check(20 <= len(loud) <= 33, f"...with the station on it ({len(loud) / 10:.1f} s of speech of 3.0)")
        check(" 101" in out["tx"][0] and 2.8 <= len(tx) / 16000 <= 5.5,
              f"the output: what goes to the transmitter, while it transmits ({len(tx) / 16000:.1f} s)")
        loudt = [i for i in range(0, len(tx) - 1600, 1600) if rms_db(tx[i:i + 1600]) > -50]
        check(20 <= len(loudt) <= 36, f"...the station repeated, and the beep ({len(loudt) / 10:.1f} s loud)")
        time.sleep(1.2)
        log = open(f"{tmp}/node-True.log").read()
        check("listening in is ON" in log and "somebody listens to the input" in log and "a listener on the output left" in log,
              "the log says that it is on, and when somebody listens")
        more = {}
        ts = [threading.Thread(target=listen, args=(hp, "rx", 1.5, more.setdefault(i, {}))) for i in range(5)]
        for t in ts: t.start()
        for t in ts: t.join()
        codes = sorted(m["rx"][0].split()[1] for m in more.values())
        check(codes == ["101", "101", "101", "101", "503"], f"four listeners at most ({', '.join(codes)})")
    finally:
        for p in (off, on):
            p.terminate()
            p.wait(timeout=5)
    if failed:
        print(f"{failed} check(s) FAILED   (files kept in {tmp})")
        return 1
    print("all checks passed")
    subprocess.run(["rm", "-rf", tmp])
    return 0


if __name__ == "__main__":
    sys.exit(main())
