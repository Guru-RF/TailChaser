#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""The admin portal and the remote API, against a running tailchaser-duplex.

    python3 tests/admin.py ./tailchaser-duplex

No hardware, no reflector: the node runs on a silent file and never logs in.
"""
import json
import os
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

failed = 0


def check(ok, what):
    global failed
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        failed += 1


class Client:
    def __init__(self, base):
        self.base, self.cookie = base, ""

    def call(self, path, data=None, headers=None, marker=True, raw=None):
        h = dict(headers or {})
        if self.cookie:
            h["Cookie"] = self.cookie
        body = None
        if raw is not None:
            body = raw.encode()
        elif data is not None:
            body = urllib.parse.urlencode(data).encode()
        if body is not None and marker:
            h["X-TailChaser"] = "1"
        req = urllib.request.Request(self.base + path, data=body, headers=h)
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                code, text, hdr = r.status, r.read().decode(), r.headers
        except urllib.error.HTTPError as e:
            code, text, hdr = e.code, e.read().decode(), e.headers
        c = hdr.get("Set-Cookie")
        if c:
            self.cookie = c.split(";")[0]
        try:
            return code, json.loads(text)
        except ValueError:
            return code, text


def start(exe, conf, log):
    proc = subprocess.Popen([exe, "-c", conf], stderr=open(log, "a"))
    return proc


def wait_up(base):
    for _ in range(50):
        try:
            urllib.request.urlopen(base + "/status.json", timeout=1).read()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "./tailchaser-duplex"
    tmp = tempfile.mkdtemp(prefix="tailchaser-admin-")
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    conf, state, log = f"{tmp}/duplex.conf", f"{tmp}/admin.conf", f"{tmp}/node.log"
    text = f"""[reflector]
host = 127.0.0.1:1
callsign = ON0TST
auth_key = secret-link-key
default_tg = 8
[radio]
type = none
cos_line = none
ptt_line = 16
[ctcss]
detect = true
tone = 79.7
[ident]
callsign = ON0TST
interval_min = 0
[tx]
hang_ms = 500
[audio]
alsa_device = file:pause:120000
[status]
http_port = {port}
bind = 127.0.0.1
admin_file = {state}
"""
    with open(conf, "w") as f:
        f.write(text)
    base = f"http://127.0.0.1:{port}"
    print("the admin portal and the remote API")
    proc = start(exe, conf, log)
    try:
        check(wait_up(base), "the node is up")
        c = Client(base)
        code, page = c.call("/admin")
        check(code == 200 and "TailChaser" in page and "Log in" in page, "/admin serves the page")
        code, r = c.call("/admin/api/session")
        check(code == 200 and r["auth"] is False and r["must_change"] is True and r["conf"] == "",
              "nobody is logged in; the path of the configuration is not told to strangers")
        code, r = c.call("/admin/api/config")
        check(code == 401, "the configuration is not readable without a login")
        code, r = c.call("/api/v1/status")
        check(code == 401, "the API wants a key")
        code, r = c.call("/admin/api/login", {"user": "admin", "pass": "admin"}, marker=False)
        check(code == 403, "a POST that does not come from the admin page is refused (request forgery)")
        code, r = c.call("/admin/api/login", {"user": "admin", "pass": "wrong"})
        check(code == 401, "a wrong password is refused")
        code, r = c.call("/admin/api/login", {"user": "admin", "pass": "admin"})
        check(code == 429, "...and the next try has to wait")
        time.sleep(1.2)
        code, r = c.call("/admin/api/login", {"user": "admin", "pass": "admin"})
        check(code == 200 and r["ok"] and r["must_change"] is True, "admin / admin logs in on a new node — and has to change it")
        code, r = c.call("/admin/api/block", {"tx": 1})
        check(code == 403, "with the first password nothing else is open")
        code, r = c.call("/admin/api/password", {"old": "admin", "new": "short"})
        check(r["ok"] is False, "a new password of 5 characters is refused")
        code, r = c.call("/admin/api/password", {"old": "admin", "new": "a-better-password"})
        check(r["ok"] is True, "the password is changed")
        code, r = c.call("/admin/api/session")
        check(r["auth"] is True and r["must_change"] is False and r["conf"] == conf, "the session goes on, the portal is open")
        other = Client(base)
        code, r = other.call("/admin/api/login", {"user": "admin", "pass": "admin"})
        check(code == 401, "admin / admin no longer works")
        time.sleep(1.2)

        # ---- blocks ----
        code, r = c.call("/admin/api/block", {"tx": 1})
        check(r["ok"] and r["block"] == {"rx": False, "tx": True}, "the transmitter is blocked from the portal")
        time.sleep(0.6)
        st = json.load(urllib.request.urlopen(base + "/status.json", timeout=3))
        check(st["block"] == {"rx": False, "tx": True}, "...and the status page says so")
        proc.send_signal(signal.SIGUSR1)
        time.sleep(1.0)
        check("BLOCKED (admin) — not keyed" in open(log).read(), "a transmission that is asked for is not keyed while it is blocked")

        # ---- API keys ----
        code, r = c.call("/admin/api/keys", {"label": "display", "scope": "read"})
        read_key = r.get("key", "")
        code, r2 = c.call("/admin/api/keys", {"label": "VFO knob", "scope": "control"})
        ctl_key = r2.get("key", "")
        check(read_key.startswith("tc_") and ctl_key.startswith("tc_") and read_key != ctl_key, "two API keys are made")
        code, r = c.call("/admin/api/keys")
        check([k["label"] for k in r["keys"]] == ["display", "VFO knob"] and "key" not in r["keys"][0],
              "they are listed by name; the keys themselves are not shown again")
        api = Client(base)
        code, r = api.call("/api/v1/status", headers={"Authorization": "Bearer " + read_key})
        check(code == 200 and r["role"] == "duplex" and r["block"]["tx"] is True, "a read key reads the status")
        code, r = api.call("/api/v1/block", {"tx": 0}, headers={"Authorization": "Bearer " + read_key}, marker=False)
        check(code == 403, "...but may not block or release")
        code, r = api.call("/api/v1/block", {"rx": 1, "tx": 0}, headers={"X-API-Key": ctl_key}, marker=False)
        check(code == 200 and r == {"rx": True, "tx": False}, "a control key blocks the receiver and releases the transmitter")
        code, r = api.call("/api/v1/block", raw='{"rx": false}', headers={"Authorization": "Bearer " + ctl_key}, marker=False)
        check(code == 200 and r == {"rx": False, "tx": False}, "...with a little JSON as well")
        code, r = api.call("/api/v1/block?tx=1", raw="", headers={"Authorization": "Bearer " + ctl_key}, marker=False)
        check(code == 200 and r == {"rx": False, "tx": True}, "...or in the address")
        code, r = api.call("/api/v1/status", headers={"Authorization": "Bearer tc_" + "0" * 48})
        check(code == 401, "a key that was never made is refused")
        kept = open(state).read()
        check(stat.S_IMODE(os.stat(state).st_mode) == 0o600 and "a-better-password" not in kept
              and read_key not in kept and ctl_key not in kept and "block_tx=1" in kept,
              "what is kept: readable by the service only, no password and no key in the clear, the block")

        # ---- configuration ----
        code, got = c.call("/admin/api/config")
        check(code == 200 and got == text, "the configuration is shown as it is")
        code, r = c.call("/admin/api/config", {"conf": text.replace("hang_ms = 500", "hang_ms = minus-five")})
        check(r["ok"] is False and "hang_ms" in r.get("said", "") and open(conf).read() == text,
              "a configuration the program refuses is not saved, and it says why")
        new = text.replace("hang_ms = 500", "hang_ms = 700")
        code, r = c.call("/admin/api/config", {"conf": new})
        check(r["ok"] is True and open(conf).read() == new and open(state + ".previous.conf").read() == text,
              "a good one is saved, the one before is kept")

        # ---- restart ----
        code, r = c.call("/admin/api/restart", {})
        rc = None
        try:
            rc = proc.wait(timeout=25)       # the test tones (not keyed) have to end first
        except subprocess.TimeoutExpired:
            pass
        check(r["ok"] and rc == 0, "restart: the program stops cleanly at a quiet moment (the service starts it again)")
        proc = start(exe, conf, log)
        check(wait_up(base), "...and is up again")
        time.sleep(0.6)
        st = json.load(urllib.request.urlopen(base + "/status.json", timeout=3))
        check(st["block"] == {"rx": False, "tx": True} and st["tx"].get("hang_ms", 700) == 700,
              "the block is still there after the restart")
        c2 = Client(base)
        code, r = c2.call("/admin/api/login", {"user": "admin", "pass": "a-better-password"})
        check(code == 200 and r["must_change"] is False, "the new password still works")
        code, r = api.call("/api/v1/block", headers={"Authorization": "Bearer " + ctl_key})
        check(code == 200 and r["tx"] is True, "...and so does the API key")
        code, r = c2.call("/admin/api/keys/delete", {"id": 2})
        code, r = api.call("/api/v1/block", headers={"Authorization": "Bearer " + ctl_key})
        check(code == 401, "a deleted key is refused")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    if failed:
        print(f"{failed} check(s) FAILED   (files kept in {tmp})")
        return 1
    print("all checks passed")
    subprocess.run(["rm", "-rf", tmp])
    return 0


if __name__ == "__main__":
    sys.exit(main())
