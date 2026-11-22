#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""Receivers in any number and of any kind.

    python3 tests/multirx.py ./tailchaser-receiver ./tailchaser-duplex ./tailchaser-transmitter ./tailchaser-sdrreceiver

  - a transmitter whose only receiver is a tailchaser-sdrreceiver (on a
    stand-in for an OpenWebRX): a repeater without a receiver of its own;
  - a repeater with its own receiver and four more: two in step (receive
    sites) and two that are late (web SDRs), all hearing the same station.
"""
import os
import sys
import tempfile
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_simplex import (Checks, Node, Reflector, Sa818, crypto_selftest, free_port, near, voice, write_wav)  # noqa: E402
from e2e_repeater import overs_at  # noqa: E402
from sdr import Server, openwebrx  # noqa: E402

KEY = "multi-key"
T0 = 1773490000


def sdr_only(exes, tmp, check):
    ref = Reflector(tmp, proto=2, auth_key="e2e-secret")
    srv = Server(openwebrx)
    port, hp = free_port(), free_port()
    sdr = Node(exes[3], tmp, "sdrreceiver", f"""[sdr]
type = openwebrx
url = http://127.0.0.1:{srv.port}
freq = 431.200
profile = 70cm
buffer_ms = 200
[uplink]
listen_port = {port}
auth_key = {KEY}
[gate]
mode = off
[status]
http_port = 0
""", T0, Sa818())
    tx = Node(exes[2], tmp, "transmitter", f"""[reflector]
host = 127.0.0.1:{ref.port}
callsign = ON0TST
auth_key = e2e-secret
default_tg = 8
[receiver]
host = 127.0.0.1:{port}
auth_key = {KEY}
name = web SDR
[radio]
freq_tx = 438.800
[tx]
delay_ms = 200
hang_ms = 500
timeout_s = 60
[ident]
callsign = ON0TST
interval_min = 0
[audio]
alsa_device = file:pause:1000
tx_record = {tmp}/tx.wav
[status]
http_port = {hp}
bind = 127.0.0.1
admin = false
""", T0, Sa818())
    try:
        ov = overs_at(ref, 1, 18)
        time.sleep(2.0)
        st = tx.status()
        d = len(ov[0]) * 0.02 if ov else 0.0
        rc = st["voter"]["receivers"]
        check(len(rc) == 1 and rc[0]["name"] == "web SDR" and rc[0]["connected"],
              "a transmitter with a web SDR as its only receiver: connected")
        check(near(d, 3.0, 0.5), f"a station on the web SDR reaches the talkgroup ({d:.2f} s of 3.0)")
        check(st["totals"]["passed"] == 1 and "from web SDR" in tx.logtext() and ", repeated" in tx.logtext(),
              "...and is repeated by the transmitter, once")
    finally:
        tx.stop()
        sdr.stop()
    try:
        w = wave.open(f"{tmp}/tx.wav")
        secs = w.getnframes() / w.getframerate()
    except (OSError, wave.Error):
        secs = 0.0
    check(secs >= 3.0, f"...on the air ({secs:.1f} s transmitted)")


def many(exes, tmp, wavs, check):
    ref = Reflector(tmp, proto=2, auth_key="e2e-secret")
    hp = free_port()
    v = wavs["v"]
    # name, signal, what it hears: A at 2.0 s (each with its own delay); B, later, only the last one
    sites = [("site north", 100, f"pause:2100,{v}"), ("site south", 60, f"pause:2100,{v}"),
             ("SDR one", 100, f"pause:3200,{v}"), ("SDR two", 100, f"pause:3500,{v},pause:5000,{v}")]
    rxs, secs = [], ""
    for i, (name, lev, play) in enumerate(sites):
        port = free_port()
        os.mkdir(f"{tmp}/rx{i}")
        rxs.append(Node(exes[0], f"{tmp}/rx{i}", "receiver", f"""[uplink]
listen_port = {port}
auth_key = {KEY}
[radio]
freq_rx = 431.200
[siglev]
mode = fixed
fixed = {lev}
[gate]
mode = on
[audio]
alsa_device = file:{play}
[status]
http_port = 0
""", T0, Sa818()))
        secs += f"[receiver{i + 1 if i else ''}]\nhost = 127.0.0.1:{port}\nauth_key = {KEY}\nname = {name}\n"
    node = Node(exes[1], tmp, "duplex", f"""[reflector]
host = 127.0.0.1:{ref.port}
callsign = ON0TST
auth_key = e2e-secret
default_tg = 8
{secs}[radio]
freq_rx = 431.200
freq_tx = 438.800
[siglev]
mode = fixed
fixed = 20
[tx]
delay_ms = 200
hang_ms = 500
timeout_s = 60
[ident]
callsign = ON0TST
interval_min = 0
[gate]
mode = on
[audio]
alsa_device = file:pause:2000,{v}
[status]
http_port = {hp}
bind = 127.0.0.1
admin = false
""", T0, Sa818())
    try:
        ov = overs_at(ref, 2, 20)
        time.sleep(2.5)
        st, log = node.status(), node.logtext()
        rc = st["voter"]["receivers"]
        check(len(rc) == 4 and all(r["connected"] for r in rc) and [r["name"] for r in rc] == [s[0] for s in sites],
              "a repeater with its own receiver and four more, two receive sites and two web SDRs: all connected")
        d = [len(o) * 0.02 for o in ov]
        check(len(ov) == 2 and len(overs_at(ref, 3, 1)) == 2, f"two stations, two overs on the talkgroup — nothing twice ({len(ov)})")
        check("site north hears it better (signal 100, the receiver here 20) — it takes the over" in log,
              "A, weak here: the best receive site that is in step takes the over")
        check(d and near(d[0], 3.1, 0.25), f"...and A is passed on once, whole ({d[0] if d else 0:.2f} s)")
        check("still hears a station" not in log.split("from SDR two")[0],
              "...what the two web SDRs bring of A afterwards is not repeated")
        check(len(d) == 2 and near(d[1], 3.0, 0.25) and "from SDR two" in log,
              f"B, heard by one web SDR only, is passed on ({d[1] if len(d) > 1 else 0:.2f} s)")
    finally:
        node.stop()
        for r in rxs:
            r.stop()


def main():
    exes = sys.argv[1:5] if len(sys.argv) >= 5 else ["./tailchaser-receiver", "./tailchaser-duplex",
                                                     "./tailchaser-transmitter", "./tailchaser-sdrreceiver"]
    crypto_selftest()
    tmp = tempfile.mkdtemp(prefix="tailchaser-multirx-")
    base = voice(3.0)
    top = max(abs(x) for x in base)
    wavs = {"v": f"{tmp}/v.wav"}
    write_wav(wavs["v"], [x * 0.4 / top for x in base])
    failed = 0
    for title, fn in (("a web SDR as the only receiver", lambda c: sdr_only(exes, f"{tmp}/a", c)),
                      ("five receivers: here, two sites, two web SDRs", lambda c: many(exes, f"{tmp}/b", wavs, c))):
        os.mkdir(f"{tmp}/{'a' if 'only' in title else 'b'}")
        check = Checks(title)
        try:
            fn(check)
        except Exception as e:                      # noqa: BLE001
            import traceback
            check(False, f"the test itself failed: {e!r}")
            check.note(traceback.format_exc().replace("\n", "\n       "))
        print(check.title)
        for line in check.lines:
            print(line)
        failed += check.failed
    if failed:
        print(f"{failed} check(s) FAILED   (files kept in {tmp})")
        return 1
    print("all checks passed")
    import subprocess
    subprocess.run(["rm", "-rf", tmp])
    return 0


if __name__ == "__main__":
    sys.exit(main())
