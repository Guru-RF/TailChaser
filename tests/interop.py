#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""Two tailchaser-simplex nodes talking through a REAL SvxReflector.

    python3 tests/interop.py ./tailchaser-simplex IMAGE

IMAGE is a Docker image whose entrypoint is `svxreflector` and which reads
/etc/svxlink/svxreflector.conf (protocol 3: TLS, certificates). The reflector
is started in a container with a throw-away configuration; nothing else on the
machine is touched.

tests/e2e_simplex.py plays the reflector itself, so it can only show that tailchaser-simplex
does what its author believes a reflector expects. This one asks a reflector:

  * node A logs in with a password over TLS, node B asks for a certificate,
    the "sysop" signs it (CA SIGN on the reflector's command PTY), B logs in
    with it — and again, later, from the stored certificate;
  * a voice on A's radio comes out of B's transmitter, and the other way
    round: encrypted UDP audio in both directions, through the real server;
  * the reflector's own status page lists both nodes, on their talkgroup.

Not part of `make check`: it needs Docker and a reflector image. About 40 s.
"""
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import e2e  # noqa: E402  (the generators, the node wrapper, the recording analysis)

TG = 1745


def node_conf(port, call, tmp, playlist, record, http, auth_key=None):
    return f"""[reflector]
host = 127.0.0.1:{port}
callsign = {call}
{"auth_key = " + auth_key if auth_key else ""}
email = sysop@example.org
default_tg = {TG}
pki_dir = {tmp}/pki-{call}
location = "Interop test"
sysop = ON0TST

[radio]
freq = 145.2375

[tx]
delay_ms = 300

[ident]
interval_min = 0

[gate]
mode = on

[audio]
alsa_device = file:{playlist}
tx_record = {record}

[status]
http_port = {http}
bind = 127.0.0.1
"""


def wait_for(what, timeout, step=0.2):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            v = what()
            if v:
                return v
        except Exception:               # noqa: BLE001 — not there yet
            pass
        time.sleep(step)
    return None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    exe, image = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="tailchaser-interop-")
    os.chmod(tmp, 0o755)
    check = e2e.Checks(f"two nodes through a real SvxReflector ({image})")
    port, hport = e2e.free_port(), e2e.free_port()
    with open(f"{tmp}/svxreflector.conf", "w") as f:
        f.write(f"""[GLOBAL]
TIMESTAMP_FORMAT="%c"
LISTEN_PORT=5300
TG_FOR_V1_CLIENTS=999
HTTP_SRV_PORT=8080
COMMAND_PTY=/dev/shm/reflector_ctrl

[SERVER_CERT]
COMMON_NAME=reflector-interop

[USERS]
ON0TST-A=Testers

[PASSWORDS]
Testers=interop-pass
""")
    os.chmod(f"{tmp}/svxreflector.conf", 0o644)
    name = f"tailchaser-interop-{os.getpid()}"
    subprocess.run(["docker", "run", "-d", "--rm", "--name", name,
                    "-p", f"127.0.0.1:{port}:5300/tcp", "-p", f"127.0.0.1:{port}:5300/udp",
                    "-p", f"127.0.0.1:{hport}:8080/tcp",
                    "-v", f"{tmp}/svxreflector.conf:/etc/svxlink/svxreflector.conf:ro", image],
                   check=True, capture_output=True)

    def refl_status():
        return json.load(urllib.request.urlopen(f"http://127.0.0.1:{hport}/status", timeout=2))

    def refl_log():
        p = subprocess.run(["docker", "logs", name], capture_output=True, text=True)
        return p.stdout + p.stderr

    e2e.write_wav(f"{tmp}/voice.wav", e2e.voice(3.0))
    radio = e2e.Sa818()
    nodes = []
    try:
        version = subprocess.run(["docker", "exec", name, "svxreflector", "--version"],
                                 capture_output=True, text=True)
        check.note("reflector: " + (version.stdout + version.stderr).strip().splitlines()[0])
        check(wait_for(refl_status, 20) is not None, "the reflector is up")

        # ---- B: no certificate yet -> request -> the sysop signs -> login ----
        hb = e2e.free_port()
        b = e2e.Node(exe, tmp, "nodeB1", node_conf(port, "ON0TST-B", tmp, "pause:600000",
                                                   f"{tmp}/b1.wav", hb), e2e.HOUR + 600, radio)
        nodes.append(b)
        check(wait_for(lambda: "certificate request sent" in b.logtext(), 10),
              "B has no certificate: the reflector asks for a signing request, B sends it")
        sign = subprocess.run(["docker", "exec", name, "sh", "-c",
                               'echo "CA SIGN ON0TST-B" > /dev/shm/reflector_ctrl'], capture_output=True, text=True)
        check(sign.returncode == 0, "the sysop signs it (CA SIGN on the reflector's command PTY)")
        t0 = time.monotonic()
        ok = wait_for(lambda: b.status()["link"]["connected"], 45)
        check(ok, f"B receives its certificate and logs in with it ({time.monotonic() - t0:.0f} s after the signature)")
        check("client certificate loaded" in b.logtext() and b.status()["link"]["proto"] == 3,
              "...over TLS, protocol 3")

        # ---- A: password over TLS; then A talks, B transmits ----
        ha = e2e.free_port()
        a = e2e.Node(exe, tmp, "nodeA", node_conf(port, "ON0TST-A", tmp, f"pause:2500,{tmp}/voice.wav",
                                                  f"{tmp}/a.wav", ha, auth_key="interop-pass"),
                     e2e.HOUR + 600, radio)
        nodes.append(a)
        check(wait_for(lambda: a.status()["link"]["connected"], 10), "A logs in with a password, over TLS")
        st = wait_for(lambda: (lambda s: s if {"ON0TST-A", "ON0TST-B"} <= set(s.get("nodes", {})) else None)(refl_status()), 5)
        nodesinfo = (st or {}).get("nodes", {})
        check(st is not None, f"the reflector lists both nodes ({', '.join(sorted(nodesinfo)) or 'none'})")
        na = nodesinfo.get("ON0TST-A", {})
        check(na.get("tg") in (TG, str(TG)) and na.get("sw") == "TailChaser"
              and na.get("nodeLocation") == "Interop test",
              f"...on talkgroup {TG}, with the node information they sent (sw={na.get('sw')}, tg={na.get('tg')})")
        ok = wait_for(lambda: [o for o in b.status()["net_history"] if o["call"] == "ON0TST-A"], 15)
        check(ok and ok[0]["outcome"] == "transmitted" and ok[0]["tg"] == TG and near(ok[0]["ms"], 3000, 200),
              f"a voice on A's radio: B hears ON0TST-A on TG {TG} for {ok[0]['ms'] / 1000 if ok else 0:.2f} s and transmits it")
        time.sleep(1.0)
        check(b.stop() == 0, "B stops cleanly")
        rec = e2e.Recording(f"{tmp}/b1.wav")
        if check(len(rec.bursts) == 1, f"B keyed its transmitter once ({len(rec.bursts)})"):
            s, e_ = rec.bursts[0]
            on, last = rec.span(s, e_)
            check(on is not None and near(on - s, 0.30, 0.07) and last - on > 2.6 and rec.audio_while_unkeyed() == 0,
                  f"...carrier, 300 ms later the voice, {last - on:.2f} s of it (decrypted, decoded), nothing without PTT")

        # ---- B again, from the stored certificate; B talks, A transmits ----
        hb2 = e2e.free_port()
        b2 = e2e.Node(exe, tmp, "nodeB2", node_conf(port, "ON0TST-B", tmp, f"pause:2500,{tmp}/voice.wav",
                                                    f"{tmp}/b2.wav", hb2), e2e.HOUR + 600, radio)
        nodes.append(b2)
        check(wait_for(lambda: b2.status()["link"]["connected"], 10)
              and "certificate request sent" not in b2.logtext(),
              "B, restarted, logs in from its stored certificate: no new request")
        ok = wait_for(lambda: [o for o in a.status()["net_history"] if o["call"] == "ON0TST-B"], 15)
        check(ok and ok[0]["outcome"] == "transmitted" and near(ok[0]["ms"], 3000, 200),
              f"a voice on B's radio: A hears ON0TST-B for {ok[0]['ms'] / 1000 if ok else 0:.2f} s and transmits it")
        time.sleep(1.0)
        sa, sb = a.status(), b2.status()
        check(sa["link"]["connects"] == 1 and sb["link"]["connects"] == 1
              and sa["totals"]["net_blocked"] == 0 and sb["totals"]["net_blocked"] == 0,
              "one login each, no reconnects, nothing dropped")
        check(a.stop() == 0 and b2.stop() == 0, "both stop cleanly")
        rec = e2e.Recording(f"{tmp}/a.wav")
        if check(len(rec.bursts) == 1, f"A keyed its transmitter once ({len(rec.bursts)})"):
            s, e_ = rec.bursts[0]
            on, last = rec.span(s, e_)
            check(on is not None and near(on - s, 0.30, 0.07) and last - on > 2.6 and rec.audio_while_unkeyed() == 0,
                  f"...with {last - on:.2f} s of voice behind the TX delay")
        log = refl_log()
        bad = [l for l in log.splitlines() if "cipher" in l.lower() or "decrypt" in l.lower()
               or "illegal" in l.lower() or "unknown message" in l.lower()]
        check(not bad, "the reflector logged no protocol or cipher complaints" + (": " + bad[0] if bad else ""))
    except Exception as e:                      # noqa: BLE001 — report, don't hide
        import traceback
        check(False, f"the test itself failed: {e!r}")
        check.note(traceback.format_exc().replace("\n", "\n       "))
    finally:
        for n in nodes:
            if n.proc.poll() is None:
                n.stop()
        log = refl_log()
        subprocess.run(["docker", "stop", "-t", "2", name], capture_output=True)

    print(check.title)
    print("\n".join(check.lines))
    if check.failed:
        for n in nodes:
            print(f"\n--- {n.name} ---\n{n.logtext()}")
        print("\n--- reflector ---\n" + log)
        print(f"\n{check.failed} check(s) FAILED   (files kept in {tmp})")
        sys.exit(1)
    print("\nall checks passed")
    import shutil
    shutil.rmtree(tmp, ignore_errors=True)


def near(v, want, tol):
    return v is not None and abs(v - want) <= tol


if __name__ == "__main__":
    main()
