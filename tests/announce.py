#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""The spoken announcement: making the files, and playing them.

    python3 tests/announce.py ./tailchaser-duplex tools/tailchaser-announce

1. tailchaser-announce against a stand-in for the voice service: the texts it
   asks for, the token it sends, the files it writes, what it keeps.
2. tailchaser-duplex with an announcement due: in a quiet moment after a QSO
   (busy = wait), and under a station that keys up while it plays (busy =
   duck) — where it must go quieter and come back.
"""
import http.server
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_simplex import FS, Node, Recording, Reflector, Sa818, free_port, near, voice, write_wav  # noqa: E402

CALL = "ON0TST"
TONE = 3100.0           # the "announcement" in the daemon tests: a tone that can be measured in a mix


def wav_bytes(samples, rate):
    hdr = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 0xFFFFFFFF, b"WAVE", b"fmt ", 16, 1, 1, rate, rate * 2,
                      2, 16, b"data", 0xFFFFFFFF)            # streamed: the lengths are not known
    return hdr + struct.pack(f"<{len(samples)}h", *samples)


class Service(http.server.BaseHTTPRequestHandler):
    """Answers like the voice services do, and remembers what it was asked."""
    asked = []

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        Service.asked.append((self.path, dict(self.headers), body))
        if self.path.startswith("/v1/audio/speech"):
            if self.headers.get("Authorization") != "Bearer test-token":
                self.send_error(401, "bad token")
                return
            pcm = [int(8000 * math.sin(2 * math.pi * 440 * i / 24000)) for i in range(24000)]
            data, ctype = wav_bytes(pcm, 24000), "audio/wav"
        else:
            pcm = [int(8000 * math.sin(2 * math.pi * 440 * i / 16000)) for i in range(16000)]
            data, ctype = struct.pack(f"<{len(pcm)}h", *pcm), "application/octet-stream"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *a):
        pass


def goertzel(x, f):
    re_ = sum(v * math.cos(2 * math.pi * f * i / FS) for i, v in enumerate(x))
    im_ = sum(v * math.sin(2 * math.pi * f * i / FS) for i, v in enumerate(x))
    return 2 * math.hypot(re_, im_) / max(1, len(x))


def tone_db(rec, t0, t1):
    """Level of the announcement tone between t0 and t1 (dBFS RMS)."""
    seg = rec.audio[int(t0 * FS):int(t1 * FS)]
    return 20 * math.log10(goertzel(seg, TONE) / math.sqrt(2) / 32768 + 1e-9)


def generator(tool, tmp, check):
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Service)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    port = srv.server_address[1]
    out = f"{tmp}/spool"

    def conf(extra, text='"This is %CALL. It is %TIME on %DATE."'):
        path = f"{tmp}/gen.conf"
        with open(path, "w") as f:
            f.write(f"""[ident]
callsign = {CALL}
interval_min = 60
[announce]
enabled = true
dir = {out}
text = {text}       # a comment
date_format = %Y-%m-%d
{extra}
""")
        return path

    def run(path, *args):
        return subprocess.run([sys.executable, tool, "-c", path, *args], capture_output=True, text=True,
                              env=dict(os.environ, TZ="UTC"))

    openai = f"provider = openai\nurl = http://127.0.0.1:{port}/v1/audio/speech\ntoken = test-token\nvoice = nova"
    r = run(conf(openai), "--dry-run", "--all-day")
    lines = r.stdout.strip().splitlines()
    today = time.strftime("%Y-%m-%d", time.gmtime())
    r2 = run(conf(openai + "\ncall_spelling = phonetic\ntime_style = spoken\nwith_ident = true"), "--dry-run", "--all-day")
    said = dict(l.split("  ", 1) for l in r2.stdout.strip().splitlines())
    want = {"00:15": "quarter past midnight", "00:45": "quarter to one", "12:00": "twelve noon",
            "13:30": "half past one", "15:00": "three o'clock", "23:45": "quarter to midnight", "00:00": "midnight"}
    check(all(said.get(k, "") == f"This is Oscar, November, Zero, Tango, Sierra, Tango. It is {v} on {today if k != '00:00' else time.strftime('%Y-%m-%d', time.gmtime(time.time() + 86400))}."
              for k, v in want.items()),
          f"call_spelling = phonetic and time_style = spoken: said the way people say it  [{said.get('00:45')}]")
    check(len(lines) == 72 and lines[0] == f"00:15  This is O N 0 T S T. It is 00:15 on {today}.",
          f"the day's texts: 72 of them, the callsign spelled, time and date filled in  [{lines[0] if lines else r.stderr}]")
    check(not any(l.startswith(("01:00", "12:00", "00:00")) for l in lines) and lines[-1].startswith("23:45"),
          "...at :15, :30 and :45 — not on the hour, when the Morse is sent")
    check(not Service.asked, "a dry run asks the service nothing")

    r = run(conf(openai + "\nwith_ident = true"), "--dry-run", "--all-day")
    both = r.stdout.strip().splitlines()
    tomorrow = time.strftime("%Y-%m-%d", time.gmtime(time.time() + 86400))
    check(len(both) == 96 and both[3].startswith("01:00") and both[-1] == f"00:00  This is O N 0 T S T. It is 00:00 on {tomorrow}.",
          f"with_ident: on the hour as well, 96 a day — the last one for the coming midnight  [{both[-1] if both else r.stderr}]")
    r = run(conf(openai), "--all-day")
    files = sorted(f for f in os.listdir(out) if f.endswith(".wav")) if os.path.isdir(out) else []
    check(r.returncode == 0 and len(files) == 72 and files[0] == "0015.wav" and "0100.wav" not in files,
          f"72 files written, 0015.wav to 2345.wav  [{r.stdout.strip() or r.stderr.strip()}]")
    check(len(Service.asked) == 72 and Service.asked[0][2] == {"model": "gpt-4o-mini-tts", "voice": "nova",
          "input": f"This is O N 0 T S T. It is 00:15 on {today}.", "response_format": "wav"},
          "the service was asked for each text once, with model, voice and text")
    if files:
        with wave.open(f"{out}/0015.wav") as w:
            n, rate = w.getnframes(), w.getframerate()
            x = struct.unpack(f"<{n}h", w.readframes(n))
        cross = sum(1 for a, b in zip(x, x[1:]) if (a < 0) != (b < 0)) / 2 / (n / rate)
        check(rate == 16000 and near(n / rate, 1.0, 0.02) and near(cross, 440, 5),
              f"24 kHz from the service becomes 16 kHz for the daemon: {n / rate:.2f} s, the 440 Hz still {cross:.0f} Hz")
    before = len(Service.asked)
    r = run(conf(openai), "--all-day")
    check(r.returncode == 0 and len(Service.asked) == before, "a second run the same day asks for nothing: all kept")

    Service.asked.clear()
    r = run(conf(openai, '"Good day from %CALL."'), "--all-day")
    check(r.returncode == 0 and len(Service.asked) == 1, "a text without the time is one request for the whole day")

    Service.asked.clear()
    r = run(conf(openai.replace("test-token", "wrong"), '"Never said, %TIME."'), "--say", "10:15")
    check(r.returncode != 0 and "401" in r.stderr, "a wrong token: an error that says so, and no file")

    Service.asked.clear()
    eleven = f"provider = elevenlabs\nurl = http://127.0.0.1:{port}/v1/text-to-speech\ntoken = el-token\nvoice = abc123"
    r = run(conf(eleven), "--say", "10:15")
    a = Service.asked[0] if Service.asked else ("", {}, {})
    hdr = {k.lower(): v for k, v in a[1].items()}
    check(r.returncode == 0 and a[0] == "/v1/text-to-speech/abc123?output_format=pcm_16000"
          and hdr.get("xi-api-key") == "el-token" and a[2].get("text", "").startswith("This is O N 0 T S T. It is 10:15"),
          f"the other service: its address, its header, its text  [{a[0]}]")
    srv.shutdown()


def node_conf(ref, hp, tmp, audio, busy):
    return f"""[reflector]
host = 127.0.0.1:{ref.port}
callsign = {CALL}
auth_key = e2e-secret
default_tg = 8

[radio]
freq_rx = 431.200
freq_tx = 438.800

[tx]
delay_ms = 200
hang_ms = 1000
timeout_s = 60
duck_db = 10

[announce]
enabled = true
dir = {tmp}/say
busy = {busy}
wait_s = 20
level_db = -16
token = not-for-the-daemon

[ident]
callsign = {CALL}
interval_min = 60

[gate]
mode = on

[audio]
alsa_device = {audio}
tx_record = {tmp}/tx-{busy}.wav

[status]
http_port = {hp}
bind = 127.0.0.1
"""


def daemon(exe, tmp, check):
    # an announcement time 5 s after the start, and not on the hour
    slot = (1773490000 // 900 + 1) * 900
    if slot % 3600 == 0:
        slot += 900
    hhmm = time.strftime("%H%M", time.gmtime(slot))
    os.makedirs(f"{tmp}/say", exist_ok=True)
    quiet = [0.05 * math.sin(2 * math.pi * TONE * i / FS) for i in range(8 * FS)]       # the level must not matter
    write_wav(f"{tmp}/say/{hhmm}.wav", quiet)
    v = voice(3.0)
    top = max(abs(x) for x in v)
    write_wav(f"{tmp}/station.wav", [x * 0.4 / top for x in v])

    hour = (slot // 3600 + 1) * 3600
    write_wav(f"{tmp}/say/{time.strftime('%H%M', time.gmtime(hour))}.wav", quiet[:3 * FS])
    for busy, audio in (("wait", f"file:pause:4500,{tmp}/station.wav"),
                        ("duck", f"file:pause:9500,{tmp}/station.wav"),
                        ("hour", "file:pause:1000")):
        ref = Reflector(tmp, proto=2, auth_key="e2e-secret")
        radio = Sa818()
        conf = node_conf(ref, free_port(), tmp, audio, "wait" if busy == "hour" else busy)
        if busy == "hour":
            # and the carrier after it as after an over: [ident] hang_ms = -1
            conf = (conf.replace("busy = wait", "busy = wait\nwith_ident = true").replace("tx-wait.wav", "tx-hour.wav")
                    .replace("interval_min = 60", "interval_min = 60\nhang_ms = -1"))
        node = Node(exe, tmp, f"duplex-{busy}", conf, hour - 5 if busy == "hour" else slot - 7.5, radio)
        try:
            time.sleep(21.5)
        finally:
            rc = node.stop()
            ref.close()
        log = open(node.log_path).read()
        rec = Recording(f"{tmp}/tx-{busy}.wav")
        b = rec.bursts
        said = [l for l in log.splitlines() if "announce:" in l and "every" not in l]
        if busy == "wait":
            # the station talks from 3 to 6 s; the announcement was due at 5 s
            ok = check(len(b) == 2 and len(said) == 1,
                       f"a station is talking when the time comes: the announcement waits for the carrier to drop "
                       f"({len(b)} transmissions)  [{said[-1][13:] if said else 'nothing said'}]")
            if ok:
                s, e_ = b[1]
                check(tone_db(rec, b[0][0], b[0][1]) < -50 and near(tone_db(rec, s + 1.0, s + 7.0), -16, 1.5),
                      f"...nothing of it on their transmission, then all of it at {tone_db(rec, s + 1.0, s + 7.0):.1f} dBFS "
                      f"(wanted -16, the file was at -29)")
                check(near(e_ - s, 0.2 + 8.0, 0.5),
                      f"...{e_ - s:.1f} s keyed: TX delay, 8 s of announcement, and no hang after it "
                      f"([ident] hang_ms: 0 when not set)")
        elif busy == "hour":
            order = [("ident" if "ident:" in l else "announce") for l in log.splitlines()
                     if ("ident: ON0TST" in l and "every" not in l) or l in said]
            ok = check(len(b) == 1 and order == ["ident", "announce"],
                       f"with_ident, on the hour: the Morse, then the announcement, in one transmission ({len(b)}, {order})")
            if ok:
                s, e_ = b[0]
                g0, g1 = rec.span(s, e_)
                check(tone_db(rec, e_ - 3.5, e_ - 1.5) > -18 and near(rec.tone_hz(s + 0.5, s + 2.0), 600, 30),
                      f"...600 Hz Morse first, the announcement after it ({e_ - s:.1f} s keyed)")
                check(tone_db(rec, e_ - 0.8, e_ - 0.1) < -40,
                      f"...with [ident] hang_ms = -1 the carrier stays up [tx] hang_ms after it "
                      f"(the last 0.7 s: {tone_db(rec, e_ - 0.8, e_ - 0.1):.0f} dBFS)")
        else:
            # the announcement runs from 5 to 13 s; the station is repeated from about 8 to 11 s
            ok = check(len(b) == 1 and len(said) == 1, f"the announcement is on the air when a station keys up: "
                                                     f"one transmission ({len(b)})")
            if ok:
                s = b[0][0]
                alone, under, back = tone_db(rec, s + 0.5, s + 2.5), tone_db(rec, s + 4.4, s + 5.4), tone_db(rec, s + 6.9, s + 7.8)
                check(near(alone, -16, 1.5) and near(alone - under, 10, 2.5),
                      f"...it goes under them: {alone:.1f} dBFS alone, {under:.1f} dBFS while they talk (10 dB down)")
                check(near(back, alone, 2.5), f"...and comes back up when they have stopped: {back:.1f} dBFS")
        check(rc == 0, f"clean exit ({busy})")


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "./tailchaser-duplex"
    tool = sys.argv[2] if len(sys.argv) > 2 else "tools/tailchaser-announce"
    tmp = tempfile.mkdtemp(prefix="tailchaser-announce-")
    failures = []

    def check(ok, what):
        print(("  ok   " if ok else "  FAIL ") + what)
        if not ok:
            failures.append(what)
        return ok

    generator(tool, tmp, check)
    daemon(exe, tmp, check)
    if failures:
        print(f"\n{len(failures)} check(s) FAILED   (files kept in {tmp})")
        sys.exit(1)
    print("\nall checks passed")


if __name__ == "__main__":
    main()
