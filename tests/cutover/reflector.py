#!/usr/bin/env python3
"""The stand-in reflector of tests/e2e_simplex.py as a service, for tests/cutover.sh.

    reflector.py STATE pki CALLSIGN DIR   what SvxLink keeps in its PKI directory
                                          for a node this reflector has signed:
                                          key, request, certificate, CA bundle
    reflector.py STATE serve PORT         be the reflector; logins and requests
                                          are appended to STATE/events.log

STATE holds the reflector's CA, so that a certificate stays good across
restarts of this service.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import e2e_simplex as e2e  # noqa: E402

state, mode = sys.argv[1], sys.argv[2]
os.makedirs(state, exist_ok=True)

if mode == "pki":
    call, d = sys.argv[3], sys.argv[4]
    ref = e2e.Reflector(state, proto=3)                 # only its CA is used
    os.makedirs(d, exist_ok=True)
    key, csr, crt = (f"{d}/{call}.{ext}" for ext in ("key", "csr", "crt"))
    ref._ossl("genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", key)
    os.chmod(key, 0o600)
    ref._ossl("req", "-new", "-key", key, "-subj", f"/CN={call}", "-out", csr)
    chain, serial = ref.sign_file(csr, f"{state}/{call}.pem")
    with open(crt, "w") as f:
        f.write(chain)
    with open(f"{d}/ca-bundle.crt", "w") as f:
        f.write(ref.ca_pem)
    print(serial)
    sys.exit(0)

ref = e2e.Reflector(state, proto=3, port=int(sys.argv[3]))
seen = 0
with open(f"{state}/events.log", "a", buffering=1) as log:
    while True:
        time.sleep(0.2)
        with ref.cond:
            new, seen = ref.events[seen:], len(ref.events)
        for _, kind, data in new:
            if kind not in ("tcp-hb", "udp-hb"):
                log.write(json.dumps([kind, data], default=str) + "\n")
