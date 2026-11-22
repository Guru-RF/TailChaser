#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""A receive site whose receiver is an SDR on the USB port.

    python3 tests/usbsdr.py ./tailchaser-receiver ./iqgen

tailchaser-receiver with [sdr] type = rtlsdr / hydrasdr. Stand-ins for
rtl_sdr and hydrasdr_rx (on the PATH) note how they were called and give the
samples of iqgen: 6 s of an empty channel, 3 s of a carrier with a tone.
"""
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sdr import free_port  # noqa: E402

failed = 0


def check(ok, what):
    global failed
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        failed += 1


def run(exe, tmp, kind, extra, out):
    hp = free_port()
    conf = f"{tmp}/{kind}.conf"
    with open(conf, "w") as f:
        f.write(f"""[sdr]
type = {kind}
freq = 431.200
{extra}
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
    env = dict(os.environ, PATH=f"{tmp}/bin:{os.environ['PATH']}")
    proc = subprocess.Popen([exe, "-c", conf], stderr=open(f"{tmp}/{kind}.log", "w"), env=env)
    seen, t0 = [], time.monotonic()
    try:
        while time.monotonic() - t0 < 12.5:
            time.sleep(0.1)
            try:
                seen.append((time.monotonic() - t0, json.load(urllib.request.urlopen(f"http://127.0.0.1:{hp}/status.json", timeout=1))))
            except OSError:
                pass
    finally:
        proc.terminate()
        proc.wait(timeout=5)
    out[kind] = (seen, open(f"{tmp}/{kind}.log").read())


def judge(kind, seen, log, what):
    st = seen[-1][1] if seen else {}
    check(seen and st["role"] == "receiver" and st["sdr"]["local"] and any(s["sdr"]["listening"] for _, s in seen),
          f"{kind}: tailchaser-receiver listens with it ({st.get('sdr', {}).get('state', 'no status')})")
    opens = [t for t, s in seen if s["gate"]["cos"]]
    dur = opens[-1] - opens[0] if opens else 0.0
    spans = sum(1 for (_, a), (_, b) in zip(seen, seen[1:]) if b["gate"]["cos"] and not a["gate"]["cos"])
    check(spans == 1 and 2.5 <= dur <= 3.4 and not seen[-1][1]["gate"]["cos"],
          f"{kind}: the squelch opens once, for the carrier, and closes after it ({dur:.1f} s of 3.0, {spans} time(s)){what}")
    lev = max((s["gate"]["rssi"] for _, s in seen if s["gate"]["cos"]), default=-1)
    idle = [s["noise"]["level_db"] for _, s in seen if not s["gate"]["cos"] and s["sdr"]["listening"]][5:]
    check(lev >= 80 and idle and -34 <= sum(idle) / len(idle) <= -26,
          f"{kind}: a full-quieting signal is strong ({lev} of 100); an empty channel hisses as a web SDR does "
          f"({sum(idle) / max(1, len(idle)):.0f} dBFS)")
    check(seen and st["sdr"]["underruns"] <= 1 and st["sdr"]["buffer_ms"] <= 150,
          f"{kind}: the audio keeps coming, and is not late ({st['sdr']['buffer_ms']} ms in store)")


def main():
    exe = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./tailchaser-receiver")
    iqgen = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "./iqgen")
    tmp = tempfile.mkdtemp(prefix="tailchaser-usbsdr-")
    os.mkdir(f"{tmp}/bin")
    for tool, args in (("rtl_sdr", "u8 240000 60000"), ("hydrasdr_rx", "s16 2500000 625000 131.8")):
        with open(f"{tmp}/bin/{tool}", "w") as f:
            f.write(f"#!/bin/sh\necho \"$@\" > {tmp}/{tool}.args\nexec {iqgen} {args}\n")
        os.chmod(f"{tmp}/bin/{tool}", 0o755)
    print("a receiver on the USB port: RTL-SDR, HydraSDR RFOne")
    out = {}
    th = [threading.Thread(target=run, args=(exe, tmp, "rtlsdr", "gain = 32.8\nppm = 2\ndevice = 1", out)),
          threading.Thread(target=run, args=(exe, tmp, "hydrasdr", "bandwidth = wide\n[ctcss]\ndetect = true\ntone = 131.8", out))]
    for t in th: t.start()
    for t in th: t.join()
    judge("rtlsdr", *out["rtlsdr"], "")
    args = open(f"{tmp}/rtl_sdr.args").read().strip() if os.path.exists(f"{tmp}/rtl_sdr.args") else ""
    check(args == "-f 431140000 -s 240000 -g 32.8 -p 2 -d 1 -",
          f"rtlsdr: rtl_sdr is tuned 60 kHz beside the channel, with the gain, the correction and the device  [{args}]")
    judge("hydrasdr", *out["hydrasdr"], " — with the CTCSS tone as well")
    args = open(f"{tmp}/hydrasdr_rx.args").read().strip() if os.path.exists(f"{tmp}/hydrasdr_rx.args") else ""
    check(args == "-r - -t 2 -f 430.575000 -a 2500000 -g 12",
          f"hydrasdr: hydrasdr_rx gives 16-bit samples at 2.5 MS/s, 625 kHz beside the channel  [{args}]")
    seen = out["hydrasdr"][0]
    check(any(s["ctcss"]["open"] for _, s in seen), "hydrasdr: the tone squelch works on what the discriminator gives")

    # a tool that is not there, or a radio that is not plugged in: said, and tried again
    os.remove(f"{tmp}/bin/rtl_sdr")
    with open(f"{tmp}/bin/rtl_sdr", "w") as f:
        f.write("#!/bin/sh\necho 'No supported devices found.' >&2\nexit 1\n")
    os.chmod(f"{tmp}/bin/rtl_sdr", 0o755)
    hp = free_port()
    with open(f"{tmp}/none.conf", "w") as f:
        f.write(f"[sdr]\ntype = rtlsdr\nfreq = 431.200\n[uplink]\nlisten_port = {free_port()}\nauth_key = k\n"
                f"[status]\nhttp_port = {hp}\nbind = 127.0.0.1\nadmin = false\n")
    proc = subprocess.Popen([exe, "-c", f"{tmp}/none.conf"], stderr=open(f"{tmp}/none.log", "w"),
                            env=dict(os.environ, PATH=f"{tmp}/bin:{os.environ['PATH']}"))
    time.sleep(1.5)
    try:
        st = json.load(urllib.request.urlopen(f"http://127.0.0.1:{hp}/status.json", timeout=2))
    except OSError:
        st = {"sdr": {"state": "no status", "listening": True}}
    proc.terminate()
    proc.wait(timeout=5)
    check(not st["sdr"]["listening"] and "No supported devices found" in st["sdr"]["state"],
          f"no radio on the port: the status says what the tool said ({st['sdr']['state']})")
    if failed:
        print(f"{failed} check(s) FAILED   (files kept in {tmp})")
        return 1
    print("all checks passed")
    subprocess.run(["rm", "-rf", tmp])
    return 0


if __name__ == "__main__":
    sys.exit(main())
