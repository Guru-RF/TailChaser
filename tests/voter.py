#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""Voting between receivers, without any hardware.

    python3 tests/voter.py ./tailchaser-receiver ./tailchaser-duplex ./tailchaser-simplex

A tailchaser-duplex with its own receiver and a second one elsewhere (a real
tailchaser-receiver over the RemoteTrx link) that hears everything 1.2 s
late, as a web SDR does:

  A  both hear it                    -> repeated once, not twice
  B  only the second receiver        -> repeated
  C  the one here loses it after 1 s -> the second receiver carries on

then the same pair with a weak signal here and a strong one there (the
better receiver takes the over), and a simplex node whose second receiver
hears the node's own transmitter (which must not come back).
"""
import json
import os
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_simplex import (Checks, Node, Reflector, Sa818, crypto_selftest, free_port, near, voice, write_wav)  # noqa: E402
from e2e_repeater import overs_at  # noqa: E402

KEY = "vote-key"
T0 = 1773490000


def receiver_conf(port, playlist, siglev=100):
    return f"""[uplink]
listen_port = {port}
auth_key = {KEY}
[radio]
freq_rx = 431.200
[siglev]
mode = fixed
fixed = {siglev}
[gate]
mode = on
[audio]
alsa_device = file:{playlist}
[status]
http_port = 0
"""


def node_conf(ref, port, hp, playlist, tmp, extra="", duplex=True, siglev=100):
    radio = "freq_rx = 431.200\nfreq_tx = 438.800" if duplex else "freq = 145.450"
    return f"""[reflector]
host = 127.0.0.1:{ref.port}
callsign = ON0TST
auth_key = e2e-secret
default_tg = 8
[receiver]
host = 127.0.0.1:{port}
auth_key = {KEY}
name = the SDR
[radio]
{radio}
[siglev]
mode = fixed
fixed = {siglev}
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
alsa_device = file:{playlist}
tx_record = {tmp}/tx.wav
[status]
http_port = {hp}
bind = 127.0.0.1
admin = false
{extra}
"""


def late_copy(exes, tmp, wavs, check):
    ref = Reflector(tmp, proto=2, auth_key="e2e-secret")
    port, hp = free_port(), free_port()
    rx = Node(exes[0], tmp, "receiver", receiver_conf(
        port, f"pause:3200,{wavs['v']},pause:1800,{wavs['v']},pause:3200,{wavs['v']}"), T0, Sa818())
    node = Node(exes[1], tmp, "duplex", node_conf(
        ref, port, hp, f"pause:2000,{wavs['v']},pause:11000,{wavs['short']}", tmp, "[voter]\nswitch = false\n[receiver]\nlag_ms = 1200\n"), T0, Sa818())
    try:
        ov = overs_at(ref, 4, 16)
        time.sleep(4.0)
        log = node.logtext()
        st = node.status()
        d = [len(o) * 0.02 for o in ov]
        check(len(ov) == 4, f"four overs reach the talkgroup ({', '.join(f'{x:.1f}' for x in d)} s)")
        if len(ov) < 4:
            return
        check(near(d[0], 3.0, 0.2), f"A, heard here and 1.2 s later there, is passed on once and whole ({d[0]:.2f} s)")
        check(near(d[1], 3.0, 0.2) and "from the SDR" in log, f"B, heard by the second receiver only, is passed on ({d[1]:.2f} s)")
        check(near(d[2], 1.0, 0.25), f"C: the receiver here loses it after a second ({d[2]:.2f} s)...")
        check(0.6 <= d[3] <= 2.4 and "still hears a station" in log,
              f"...the second one still hears it after its delay, and carries on ({d[3]:.2f} s more)")
        v = st["voter"]
        check(v["receivers"][0]["name"] == "the SDR" and v["receivers"][0]["connected"] and v["receivers"][0]["overs"] == 2,
              "the status tells which receiver had how many overs")
        check(sum(1 for x in d if x > 0.3) == 4 and st["tx"]["count"] <= 4, "nothing was transmitted twice")
        check(v["receivers"][0]["lag_ms"] == 1200 and v["receivers"][0]["lag_set"] and "is 1200 ms behind (set" in log,
              "how far behind it is was measured and set ([receiver] lag_ms): the voter keeps to that")
    finally:
        node.stop()
        rx.stop()


def letters(exes, tmp, wavs, check):
    """The receivers' letters in the node information (a certificate login: the
    password login of the other scenarios sends no node information)."""
    ref = Reflector(tmp)
    hp = free_port()
    conf = node_conf(ref, free_port(), hp, "pause:1000", tmp).replace(
        "auth_key = e2e-secret", f"email = sysop@example.org\npki_dir = {tmp}/pki")
    node = Node(exes[1], tmp, "duplex", conf, T0, Sa818())
    try:
        e = ref.wait("nodeinfo", 40)
        rxs = json.loads(e[2])["qth"][0]["rx"] if e else {}
        check(sorted(rxs) == ["A", "B"] and rxs["A"]["name"] == "Rx1" and rxs["B"]["name"] == "the SDR"
              and rxs["B"].get("freq") == 431.2,
              f"the node information names both receivers, each under its letter ({sorted(rxs)}: B is {rxs.get('B', {}).get('name')})")
        s = ref.wait("siglev", 5)
        check(s is not None and s[2] == [("A", 0, True, False, False), ("B", 0, False, False, False)],
              f"...and the reflector is told that B, which the node cannot reach, is not in use ({s and s[2]})")
    finally:
        node.stop()


def better_one(exes, tmp, wavs, check):
    for lag in (200, 1200):
        ref = Reflector(tmp, proto=2, auth_key="e2e-secret")
        port, hp = free_port(), free_port()
        rx = Node(exes[0], tmp, "receiver", receiver_conf(port, f"pause:{2000 + lag},{wavs['v']}"), T0, Sa818())
        node = Node(exes[1], tmp, "duplex", node_conf(ref, port, hp, f"pause:2000,{wavs['v']}", tmp, siglev=20), T0, Sa818())
        try:
            ov = overs_at(ref, 1, 12)
            time.sleep(2.5)
            log = node.logtext()
            d = len(ov[0]) * 0.02 if ov else 0.0
            took = "the SDR hears it better (signal 100, the receiver here 20) — it takes the over" in log
            if lag == 200:
                check(took, "a weak signal here, a strong one there, in step: the better receiver takes the over")
                sl = [d for _, d in ref.all("siglev") if d]
                check(any(d == [("A", 20, True, True, True), ("B", 0, True, False, False)] for d in sl)
                      and any(d == [("A", 20, True, True, False), ("B", 100, True, True, True)] for d in sl)
                      and sl[-1] == [("A", 0, True, False, False), ("B", 0, True, False, False)],
                      "the reflector hears the vote: A alone with signal 20, then B with 100 listened to, then both closed")
                check(near(d, 3.2, 0.3), f"...in the same over, which ends when that receiver's does ({d:.2f} s: 3.0 s and its delay)")
            else:
                check(not took and "s behind — the over stays where it is" in log,
                      "the better receiver is a second behind (a web SDR): it does not take a running over")
                check(near(d, 3.0, 0.2) and len(overs_at(ref, 1, 1)) == 1,
                      f"...so nobody hears the last words twice: the over is passed on once, as long as it was ({d:.2f} s)")
        finally:
            node.stop()
            rx.stop()


def own_transmitter(exes, tmp, wavs, check):
    ref = Reflector(tmp, proto=2, auth_key="e2e-secret")
    port, hp = free_port(), free_port()
    # the second receiver "hears" the node's own transmission (3 s from 3.0), 1.5 s late
    rx = Node(exes[0], tmp, "receiver", receiver_conf(port, f"pause:3700,{wavs['v']},pause:6000,{wavs['v']}"), T0, Sa818())
    node = Node(exes[2], tmp, "simplex", node_conf(ref, port, hp, "pause:60000", tmp, duplex=False), T0, Sa818())
    try:
        e = ref.wait("auth", 6)
        check(e is not None, "the simplex node logs in")
        time.sleep(max(0.0, node.t0 + 3.0 - time.monotonic()))
        src = [bytes(40)] * 150                       # 3 s from the talkgroup (silence in Opus is fine: it keys)
        m = ref.mark()
        ref.talk(8, "ON4AAA", src)
        time.sleep(6.5)
        back = [d for _, d in ref.all("audio", m)]
        check(len(back) == 0, f"what the second receiver hears of the node's own transmission does not go back to the talkgroup ({len(back) * 0.02:.1f} s)")
        ov = overs_at(ref, 1, 12)
        d = len(ov[0]) * 0.02 if ov else 0.0
        check(near(d, 3.0, 0.25) and "from the SDR" in node.logtext(),
              f"a station it hears later, on a quiet channel, does ({d:.2f} s)")
    finally:
        node.stop()
        rx.stop()


def main():
    exes = sys.argv[1:4] if len(sys.argv) >= 4 else ["./tailchaser-receiver", "./tailchaser-duplex", "./tailchaser-simplex"]
    crypto_selftest()
    tmp = tempfile.mkdtemp(prefix="tailchaser-voter-")
    base = voice(3.0)
    top = max(abs(x) for x in base)
    wavs = {"v": f"{tmp}/v.wav", "short": f"{tmp}/short.wav"}
    write_wav(wavs["v"], [x * 0.4 / top for x in base])
    write_wav(wavs["short"], [x * 0.4 / top for x in base[:16000]])
    runs = []
    for title, fn, sub in (("voting: a second receiver that is late", late_copy, "late"),
                           ("voting: the better receiver", better_one, "better"),
                           ("voting: a simplex node and its own transmitter", own_transmitter, "own"),
                           ("voting: what the reflector is told about the receivers", letters, "letters")):
        os.mkdir(f"{tmp}/{sub}")
        check = Checks(title)

        def run(fn=fn, check=check, sub=sub):
            try:
                fn(exes, f"{tmp}/{sub}", wavs, check)
            except Exception as e:                      # noqa: BLE001
                import traceback
                check(False, f"the test itself failed: {e!r}")
                check.note(traceback.format_exc().replace("\n", "\n       "))
        th = threading.Thread(target=run)
        th.start()
        runs.append((check, th))
    failed = 0
    for check, th in runs:
        th.join()
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
