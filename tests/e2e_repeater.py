#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""End-to-end test of the repeater roles without any hardware.

    python3 tests/e2e_repeater.py ./tailchaser-receiver ./tailchaser-transmitter ./tailchaser-duplex

Scenario 1 — a split repeater: a real tailchaser-receiver plays generated WAV
files as squelch openings, a real tailchaser-transmitter is connected to it
over the RemoteTrx link and to a stand-in reflector. Checked: the link comes
up, every over is transmitted again AND goes to the talkgroup, a weak and a
strong station come out equally loud, the roger beep and the hang time, and a
talkgroup over is transmitted.

Scenario 2 — tailchaser-duplex, receiver and transmitter in one program: the
same, plus the priority rule (a station on the input wins over the talkgroup).

Scenario 3 — tailchaser-duplex as it comes (`[ident] hang_ms` not set): after
an over the carrier stays up `[tx] hang_ms`, after the identification on a
quiet channel it drops as soon as the Morse is out.

Standard library only; the stand-ins come from tests/e2e_simplex.py. Takes
about half a minute (the audio plays in real time).
"""
import math
import os
import signal
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_simplex import (FS, MINUTE, Checks, Node, Recording, Reflector, Sa818,  # noqa: E402
                         crypto_selftest, free_port, near, voice, write_wav)

CALL = "ON0TST"
KEY = "e2e-link-key"
T0 = 1773490000         # any moment well away from an identification


def db(x):
    return 20 * math.log10(max(x, 1e-9) / 32768.0)


def groups(rec, s, e_, thr=400.0, gap=5):
    """Stretches of audio between s and e_ seconds: [(start, end), ...], split
    where the channel is silent for `gap` frames or more."""
    out, start, quiet = [], None, 0
    for f in range(int(s / 0.02), min(int(e_ / 0.02), len(rec.rms))):
        if rec.rms[f] > thr:
            if start is None:
                start = f
            last, quiet = f, 0
        elif start is not None:
            quiet += 1
            if quiet >= gap:
                out.append((start * 0.02, (last + 1) * 0.02))
                start = None
    if start is not None:
        out.append((start * 0.02, (last + 1) * 0.02))
    return out


def speech_db(rec, s, e_):
    """Level of the loud frames between s and e_ (dBFS RMS)."""
    fr = rec.rms[int(s / 0.02):int(e_ / 0.02)]
    top = max(fr, default=0.0)
    loud = [x for x in fr if x >= top / 8]
    return db(math.sqrt(sum(x * x for x in loud) / len(loud))) if loud and top > 0 else -120.0


def beep_of(rec, s, e_):
    """The last stretch of audio in a transmission: (start, length, Hz)."""
    g = groups(rec, s, e_)
    if not g:
        return None, 0.0, 0.0
    a, b = g[-1]
    return a, b - a, rec.tone_hz(a, b)


def pitch(rec, t0, t1):
    """Pitch between t0 and t1 seconds, from the zero crossings."""
    seg = rec.audio[int(t0 * FS):int(t1 * FS)]
    cross = sum(1 for a, b in zip(seg, seg[1:]) if (a < 0) != (b < 0))
    return cross / (2 * (t1 - t0)) if t1 > t0 else 0.0


def tones(rec, t_beep, beep_len):
    if not t_beep:
        return "nothing"
    return f"{pitch(rec, t_beep + 0.01, t_beep + 0.06):.0f} then {pitch(rec, t_beep + beep_len - 0.06, t_beep + beep_len - 0.01):.0f} Hz"


def bleep_blop(rec, t_beep, beep_len):
    """Two tones: 1200 Hz, then 800 Hz, 70 ms each with 20 ms between."""
    return (t_beep is not None and near(beep_len, 0.16, 0.045)
            and near(pitch(rec, t_beep + 0.01, t_beep + 0.06), 1200, 80)
            and near(pitch(rec, t_beep + beep_len - 0.06, t_beep + beep_len - 0.01), 800, 80))


def blop(rec, t_beep, beep_len):
    """One tone: 800 Hz for 70 ms."""
    return (t_beep is not None and near(beep_len, 0.07, 0.035)
            and near(pitch(rec, t_beep + 0.01, t_beep + beep_len - 0.01), 800, 80))


def overs_at(ref, n, timeout):
    """Wait for n overs on the stand-in reflector: a list of packet lists."""
    out, start = [], 0
    for _ in range(n):
        a0 = ref.wait("audio", timeout, start=start)
        fl = ref.wait("flush", timeout, start=a0[0]) if a0 else None
        if not fl:
            break
        out.append([d for _, d in ref.all("audio", a0[0], fl[0])])
        start = fl[0] + 1
    return out


# --------------------------------------------------------------------------
# scenario 1: receiver + transmitter

def scenario_split(exes, tmp, wavs, check):
    rx_exe, tx_exe, _ = exes
    ref = Reflector(tmp, proto=2, auth_key="e2e-secret")
    radio_rx, radio_tx = Sa818(), Sa818()
    port, hp_rx, hp_tx = free_port(), free_port(), free_port()
    rx_conf = f"""[uplink]
listen_port = {port}
auth_key = {KEY}

[radio]
freq_rx = 431.200
ctcss_rx = 131.8

[gate]
mode = on

[audio]
alsa_device = file:pause:4000,{wavs['quiet']},pause:3000,{wavs['loud']}

[status]
http_port = {hp_rx}
bind = 127.0.0.1
"""
    tx_conf = f"""[reflector]
host = 127.0.0.1:{ref.port}
callsign = {CALL}
auth_key = e2e-secret
default_tg = 8

[receiver]
host = 127.0.0.1:{port}
auth_key = {KEY}
status_port = {hp_rx}

[radio]
freq_tx = 438.800
bandwidth = narrow
ctcss_tx = 131.8

[tx]
delay_ms = 200
hang_ms = 1000
timeout_s = 60
test_s = 2

[repeater]
net_call = true

[ident]
callsign = {CALL}
interval_min = 0

[audio]
alsa_device = file:pause:1000
tx_record = {tmp}/tx.wav

[status]
http_port = {hp_tx}
bind = 127.0.0.1
"""
    rx = Node(rx_exe, tmp, "receiver", rx_conf, T0, radio_rx)
    tx = Node(tx_exe, tmp, "transmitter", tx_conf, T0, radio_tx)
    try:
        e = ref.wait("auth", 6)
        check(e is not None and e[2] == (CALL, True), "the transmitter logs in on the reflector")
        time.sleep(1.5)
        st = tx.status()
        check(st["role"] == "transmitter" and st["receiver"]["connected"] and st["receiver"]["codec"] == "S16",
              f"...and on its receiver, over the RemoteTrx link ({st['receiver']['peer']}, "
              f"protocol {st['receiver']['proto']}, {st['receiver']['codec']})")
        check(rx.status()["uplink"]["connected"], "the receiver shows the transmitter connected")
        rr = tx.status().get("remote_receiver") or {}
        check(rr.get("role") == "receiver" and rr.get("uplink", {}).get("connected") is True,
              "the transmitter passes the receive site's status on: one place to ask for the whole repeater")
        grp = radio_tx.first("AT+DMOSETGROUP")
        check(grp is not None and "438.8000" in grp,
              f"SA818 of the transmitter: 438.800 MHz narrow  [{grp}]")
        grp = radio_rx.first("AT+DMOSETGROUP")
        check(grp is not None and "431.2000" in grp, f"SA818 of the receiver: 431.200 MHz  [{grp}]")

        seen = []                                   # the transmitter's own signal reading, while it runs
        stop = threading.Event()

        def watch():
            while not stop.is_set():
                try:
                    g = tx.status()["gate"]
                    seen.append((g["cos"], g["rssi"], g["rssi_lo"], g["rssi_hi"]))
                except Exception:               # noqa: BLE001
                    pass
                time.sleep(0.15)
        threading.Thread(target=watch, daemon=True).start()
        ov = overs_at(ref, 2, 22)
        stop.set()
        check(len(ov) == 2, f"both overs reach the talkgroup ({len(ov)} of 2)")
        on = [r for c, r, _, _ in seen if c]
        check(on and min(on) >= 1 and all(r == -1 for c, r, _, _ in seen if not c) and seen[0][2:] == (0, 100),
              f"the transmitter shows the signal strength its receiver reports ({max(on) if on else '-'} of 100 during an over, none without)")
        for i, pk in enumerate(ov):
            check(2.6 <= len(pk) * 0.02 <= 3.15,
                  f"over {i + 1} is on the talkgroup, up to where the gate was at the unkey: {len(pk) * 0.02:.2f} s of 3.00 s")
        if len(ov) < 2 or len(ov[0]) < 100:
            check(False, "no audio to replay — the rest of this scenario cannot run")
            return
        time.sleep(3.0)
        ref.talk(8, "ON7AAA", ov[0][:50])
        time.sleep(5.5)
        tx.proc.send_signal(signal.SIGUSR1)
        time.sleep(4.5)

        st = tx.status()
        nh = {o["call"]: o["outcome"] for o in st["net_history"]}
        check(nh.get("ON7AAA") == "transmitted", "an over from the talkgroup is transmitted")
        check(st["totals"]["passed"] == 2 and len(st["history"]) == 2 and all(o["tg"] == 8 for o in st["history"]),
              "the transmitter's status page lists both overs from the receiver, sent to TG 8")
        page = tx.page()
        check(b"TailChaser" in page and b"id=cup" in page, "its status page is served")
        check(not ref.all("udp-bad") and not ref.all("session-error"), "no protocol errors")
    finally:
        rc_tx = tx.stop()
        rc_rx = rx.stop()
        ref.close()
    check(rc_tx == 0 and rc_rx == 0, f"clean exit of both on SIGTERM (exit codes {rc_tx}, {rc_rx})")

    rec = Recording(f"{tmp}/tx.wav")
    b = rec.bursts
    for i, (s, e_) in enumerate(b):
        check.note(f"transmission {i + 1}: {s:6.2f} s .. {e_:6.2f} s  ({e_ - s:.2f} s)")
    if not check(len(b) == 4, f"four transmissions: two repeated overs, one from the talkgroup, the test (got {len(b)})"):
        return
    check(rec.audio_while_unkeyed() == 0, "not one sample of audio without PTT")
    lv = []
    for i in (0, 1):
        s, e_ = b[i]
        g = groups(rec, s, e_)
        t_beep, beep_len, hz = beep_of(rec, s, e_)
        lv.append(speech_db(rec, s, t_beep - 0.1) if t_beep else -120.0)
        check(len(g) >= 2 and blop(rec, t_beep, beep_len),
              f"over {i + 1}: blop after it — {pitch(rec, (t_beep or 0) + 0.01, (t_beep or 0) + beep_len - 0.01):.0f} Hz, {beep_len * 1000:.0f} ms")
        # The transmitter runs behind its receiver (0.2 s of TX delay, the queue).
        # When the station unkeys, what still waits is not sent: it would hear it back.
        said = [x for x in g if t_beep is None or x[1] <= t_beep]
        on_air = said[-1][1] - said[0][0] if said else 0.0
        gap = (t_beep or 0) - (said[-1][1] if said else 0)
        check(t_beep is not None and 2.2 <= on_air <= 2.78 and gap < 0.3,
              f"...the station does not hear the end of its own over back: {on_air:.2f} s of it on the air, "
              f"the beep {gap * 1000:.0f} ms behind — what the transmitter still had waiting at the unkey is dropped")
        check(t_beep is not None and near(e_ - (t_beep + beep_len), 1.0, 0.25),
              f"...and the carrier stays up for the hang time ({e_ - (t_beep or 0) - beep_len:.2f} s, 1 s set)")
        check(near(e_ - s, 0.2 + 3.0 + 0.2 + 0.07 + 1.0, 0.45),
              f"...{e_ - s:.2f} s keyed in all (TX delay + 3 s of audio + beep + hang)")
    check(abs(lv[0] - lv[1]) <= 3.0 and all(abs(x + 20) <= 4.0 for x in lv),
          f"a station 22 dB weaker than the next is transmitted as loud: {lv[0]:.1f} and {lv[1]:.1f} dBFS")
    s, e_ = b[2]
    g = groups(rec, s, e_)
    t_beep, beep_len, hz = beep_of(rec, s, e_)
    who = next((w for w in (rec.morse(s + t, e_, 800, 34)[0] for t in (1.3, 1.4, 1.5, 1.6, 1.7, 1.2)) if w == "ON7AAA"),
               rec.morse(s + 1.4, e_, 800, 34)[0])
    check(who == "ON7AAA" and not bleep_blop(rec, t_beep, beep_len),
          f"the talkgroup over of a station that is not on a repeater ends with its callsign in Morse (\"{who}\"), and no beep")
    check(near(e_ - s, 0.2 + 1.0 + 0.2 + 2.25 + 1.0, 0.5),
          f"...{e_ - s:.2f} s keyed (TX delay + 1 s of audio + the Morse + hang)")
    s, e_ = b[3]
    g = groups(rec, s, e_)
    check(len(g) == 1 and near(g[0][1] - g[0][0], 2.0, 0.1) and near(pitch(rec, g[0][0] + 0.2, g[0][0] + 0.8), 1000, 20)
          and near(rec.peak(g[0][0] + 0.2, g[0][0] + 0.8), 32767 * 10 ** (-20 / 20), 300),
          f"SIGUSR1: a test transmission — {g[0][1] - g[0][0]:.1f} s of tones, 1000 Hz first, at -20 dBFS")


# --------------------------------------------------------------------------
# scenario 2: duplex

def scenario_duplex(exes, tmp, wavs, check):
    exe = exes[2]
    ref = Reflector(tmp, proto=2, auth_key="e2e-secret")
    radio = Sa818()
    hp = free_port()
    conf = f"""[reflector]
host = 127.0.0.1:{ref.port}
callsign = {CALL}
auth_key = e2e-secret
default_tg = 8

[radio]
freq_rx = 431.200
freq_tx = 438.800
bandwidth = narrow
ctcss_rx = 131.8
ctcss_tx = 131.8

[tx]
delay_ms = 200
hang_ms = 1000
timeout_s = 60

[repeater]
net_call = true

[ident]
callsign = {CALL}
interval_min = 0

[gate]
mode = on

[audio]
alsa_device = file:pause:3000,{wavs['loud']},pause:6000,{wavs['loud']}
tx_record = {tmp}/tx.wav

[status]
http_port = {hp}
bind = 127.0.0.1
"""
    node = Node(exe, tmp, "duplex", conf, T0, radio)
    try:
        e = ref.wait("auth", 6)
        check(e is not None and e[2] == (CALL, True), "the repeater logs in on the reflector")
        grp = radio.first("AT+DMOSETGROUP")
        check(grp is not None and "438.8000,431.2000" in grp,
              f"SA818: transmit 438.800, receive 431.200 MHz  [{grp}]")
        ov = overs_at(ref, 1, 12)
        if not ov or len(ov[0]) < 100:
            check(False, "the first over never reached the talkgroup — the rest cannot run")
            return
        check(near(len(ov[0]) * 0.02, 3.0, 0.15), f"over 1 is on the talkgroup whole: {len(ov[0]) * 0.02:.2f} s")
        m = ref.mark()
        # a talkgroup over while the input is quiet...
        sleep_to = node.t0 + 10.5
        time.sleep(max(0.0, sleep_to - time.monotonic()))
        ref.talk(8, "ON0OST", ov[0][:50])
        # ...and one that a station on the input keys up into (at 16 s)
        time.sleep(max(0.0, node.t0 + 14.8 - time.monotonic()))
        ref.talk(8, "ON7BBB", ov[0])
        ov2 = [d for _, d in ref.all("audio", m)]
        time.sleep(4.0)
        ov2 = [d for _, d in ref.all("audio", m)]
        st = node.status()
        nh = {o["call"]: o["outcome"] for o in st["net_history"]}
        check(st["role"] == "duplex" and nh.get("ON0OST") == "transmitted",
              "a talkgroup over on a quiet input is transmitted")
        check(nh.get("ON7BBB") in ("partly", "channel busy"),
              f"a station keys up during a talkgroup over: the station wins ({nh.get('ON7BBB')})")
        check(near(len(ov2) * 0.02, 3.0, 0.15),
              f"...and is heard on the talkgroup whole ({len(ov2) * 0.02:.2f} s of 3.00 s)")
        check(st["totals"]["passed"] == 2, "two overs passed the gate")
    finally:
        rc = node.stop()
        ref.close()
    check(rc == 0, f"clean exit on SIGTERM (exit code {rc})")

    rec = Recording(f"{tmp}/tx.wav")
    b = rec.bursts
    for i, (s, e_) in enumerate(b):
        check.note(f"transmission {i + 1}: {s:6.2f} s .. {e_:6.2f} s  ({e_ - s:.2f} s)")
    if not check(len(b) == 3, f"three transmissions (got {len(b)})"):
        return
    check(rec.audio_while_unkeyed() == 0, "not one sample of audio without PTT")
    s, e_ = b[0]
    t_beep, beep_len, hz = beep_of(rec, s, e_)
    g = groups(rec, s, e_)
    check(len(g) >= 2 and blop(rec, t_beep, beep_len),
          f"over 1 is repeated, with a blop after it ({beep_len * 1000:.0f} ms)")
    if t_beep:
        first = g[0][0]
        check(near(t_beep - first, 3.2, 0.35),
              f"...to its last word: {t_beep - first:.2f} s from the first sound to the beep")
        check(abs(speech_db(rec, s, t_beep - 0.1) + 20) <= 4.0,
              f"...at the normalised level ({speech_db(rec, s, t_beep - 0.1):.1f} dBFS)")
    s, e_ = b[2]
    t_beep, beep_len, hz = beep_of(rec, s, e_)
    check(blop(rec, t_beep, beep_len) and near(e_ - (t_beep + beep_len), 1.0, 0.25),
          "the transmission the station keyed into ends with their blop and the hang time")
    s, e_ = b[1]
    t_beep, beep_len, hz = beep_of(rec, s, e_)
    # the repeater it came from, in Morse after the over — and then no beep
    who = next((w for w in (rec.morse(s + t, e_, 800, 34)[0] for t in (1.3, 1.4, 1.5, 1.6, 1.7, 1.2)) if w == "OST"),
               rec.morse(s + 1.4, e_, 800, 34)[0])
    check(who == "OST", f"the talkgroup over ended with the repeater it came from, in Morse: ON0OST is \"{who}\"")
    first = groups(rec, s, e_)[0][0] if groups(rec, s, e_) else e_
    check(first - s < 0.45, f"...and its speech did not wait for that: it began {first - s:.2f} s after the carrier (TX delay 0.2 s)")
    check(not bleep_blop(rec, t_beep, beep_len),
          f"...and no bleep blop after it: the Morse says it came over the network (last sound: {tones(rec, t_beep, beep_len)})")

# --------------------------------------------------------------------------
# scenario 3: the carrier after the identification on a quiet channel

def scenario_ident_hang(exes, tmp, wavs, check):
    exe = exes[2]
    ref = Reflector(tmp, proto=2, auth_key="e2e-secret")
    radio = Sa818()
    hp = free_port()
    conf = f"""[reflector]
host = 127.0.0.1:{ref.port}
callsign = {CALL}
auth_key = e2e-secret
default_tg = 8

[radio]
freq_rx = 431.200
freq_tx = 438.800
bandwidth = narrow
ctcss_rx = 131.8
ctcss_tx = 131.8

[tx]
delay_ms = 200
hang_ms = 3000
timeout_s = 60

[ident]
callsign = {CALL}
interval_min = 1

[gate]
mode = on

[audio]
alsa_device = file:pause:1000,{wavs['loud']},pause:30000
tx_record = {tmp}/tx.wav

[status]
http_port = {hp}
bind = 127.0.0.1
"""
    # the minute strikes 20 s after the start: the over and its hang time are done by then
    node = Node(exe, tmp, "duplex", conf, MINUTE - 20, radio)
    try:
        time.sleep(max(0.0, node.t0 + 25.0 - time.monotonic()))
        st = node.status()
        check(st["ident"]["count"] == 1, f"one identification ({st['ident']['count']})")
    finally:
        rc = node.stop()
        ref.close()
    check(rc == 0, f"clean exit on SIGTERM (exit code {rc})")

    rec = Recording(f"{tmp}/tx.wav")
    b = rec.bursts
    for i, (s, e_) in enumerate(b):
        check.note(f"transmission {i + 1}: {s:6.2f} s .. {e_:6.2f} s  ({e_ - s:.2f} s)")
    if not check(len(b) == 2, f"two transmissions: the over, the identification (got {len(b)})"):
        return
    s, e_ = b[0]
    t_beep, beep_len, hz = beep_of(rec, s, e_)
    hang = e_ - (t_beep + beep_len) if t_beep else 0.0
    check(t_beep is not None and near(hang, 3.0, 0.3),
          f"after an over the carrier stays up [tx] hang_ms: {hang:.2f} s of 3.00 s")
    s, e_ = b[1]
    text = rec.morse(s, e_, 600, 24)[0]
    g = groups(rec, s, e_)
    after = e_ - g[-1][1] if g else e_ - s
    check(text == CALL and after < 0.35,
          f"the identification on a quiet channel (\"{text}\"): the carrier drops {after:.2f} s after "
          f"the Morse ([ident] hang_ms: 0 when not set)")


def main():
    exes = sys.argv[1:4]
    if len(exes) != 3:
        exes = ["./tailchaser-receiver", "./tailchaser-transmitter", "./tailchaser-duplex"]
    keep = "--keep" in sys.argv
    crypto_selftest()
    tmp = tempfile.mkdtemp(prefix="tailchaser-e2e-rpt-")
    base = voice(3.0)
    top = max(abs(x) for x in base)
    wavs = {}
    for name, peak in (("quiet", 0.04), ("loud", 0.5)):
        wavs[name] = f"{tmp}/{name}.wav"
        write_wav(wavs[name], [x * peak / top for x in base])

    runs = []
    for title, fn, sub in (("a split repeater: tailchaser-receiver + tailchaser-transmitter", scenario_split, "split"),
                           ("tailchaser-duplex: receiver and transmitter in one", scenario_duplex, "duplex"),
                           ("tailchaser-duplex: the carrier after the identification on a quiet channel",
                            scenario_ident_hang, "identhang")):
        os.mkdir(f"{tmp}/{sub}")
        check = Checks(title)

        def run(fn=fn, check=check, sub=sub):
            try:
                fn(exes, f"{tmp}/{sub}", wavs, check)
            except Exception as e:                      # noqa: BLE001 — report, don't hide
                import traceback
                check(False, f"the test itself failed: {e!r}")
                check.note(traceback.format_exc().replace("\n", "\n       "))
        th = threading.Thread(target=run)
        th.start()
        runs.append((check, th, sub))

    failed = 0
    for check, th, sub in runs:
        th.join()
        print(check.title)
        print("\n".join(l for l in check.lines if l.strip() not in ("ok", "FAIL")))
        if check.failed:
            failed += check.failed
            for name in ("receiver.log", "transmitter.log", "duplex.log"):
                path = f"{tmp}/{sub}/{name}"
                if os.path.exists(path):
                    print(f"\n--- {name} ---\n" + open(path).read())
        print()
    if failed:
        print(f"{failed} check(s) FAILED   (files kept in {tmp})")
        sys.exit(1)
    print("all checks passed")
    if keep:
        print(f"files kept in {tmp}")
    else:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
