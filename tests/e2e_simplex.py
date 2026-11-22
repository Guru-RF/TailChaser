#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
"""End-to-end test of tailchaser-simplex without any hardware.

    python3 tests/e2e_simplex.py ./tailchaser-simplex

Nodes are started with file audio: a playlist of generated WAV files, each
played in real time as one squelch opening, while everything the node would
transmit is recorded (audio + PTT). This script plays the rest of the world:

  * an SvxReflector — protocol 3 with TLS, certificate enrollment and
    AES-GCM-encrypted UDP audio for the first node, protocol 2 with a password
    and plain UDP for the second, and for the third the reflector of a node
    that was migrated from SvxLink: it arrives with a key and a certificate;
  * the SA818, on a pty, to see what the radio chip is told;
  * the clock: the nodes are started a few seconds before the hour / minute.

It then checks who was told what: the talkgroup only hears a voice, the
transmitter is keyed for talkgroup audio with the TX delay in front, never over
a station that is talking, the callsign goes out in Morse on the clock, DTMF
selects a talkgroup, the time-out cuts a long transmission, a certificate is
reused, renewed, and asked for again when it has expired.

Standard library plus the `openssl` command. The three scenarios run side by
side and take about 45 seconds: the audio plays in real time.

This reflector is a stand-in written from the same reading of the protocol as
the node. tests/interop.py runs two nodes through a real svxreflector.
"""
import array
import calendar
import hashlib
import hmac
import json
import math
import os
import pty
import queue
import random
import re
import socket
import ssl
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import tty
import urllib.request
import wave

FS = 16000
CALL = "ON0TST-S"

# --------------------------------------------------------------------------
# test signals


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(s * 32767))))
                               for s in samples))


def noise(secs, level=0.1):
    rnd = random.Random(1)
    return [rnd.gauss(0, level) for _ in range(int(secs * FS))]


def voice(secs):
    """A crude talker: a glottal pulse train with a wandering pitch, through two
    formant resonators, in syllables."""
    rnd = random.Random(2)
    out, phase = [], 0.0
    res = []
    for fc, bw in ((700.0, 110.0), (1250.0, 140.0)):
        r = math.exp(-math.pi * bw / FS)
        res.append([2 * r * math.cos(2 * math.pi * fc / FS), -r * r, 0.0, 0.0])
    for i in range(int(secs * FS)):
        t = i / FS
        f0 = 125 + 28 * math.sin(2 * math.pi * 0.7 * t) + 9 * math.sin(2 * math.pi * 2.3 * t)
        phase += f0 / FS
        x = 0.0
        if phase >= 1.0:
            phase -= 1.0
            x = 1.0
        y = 0.0
        for r in res:
            v = x + r[0] * r[2] + r[1] * r[3]
            r[3], r[2] = r[2], v
            y += v
        env = max(0.0, math.sin(2 * math.pi * 1.6 * t)) ** 0.5
        out.append(0.12 * y * env + rnd.gauss(0, 0.002))
    return out


def dtmf(digits):
    lo = {"1": 697, "2": 697, "3": 697, "4": 770, "5": 770, "6": 770,
          "7": 852, "8": 852, "9": 852, "*": 941, "0": 941, "#": 941}
    hi = {"1": 1209, "4": 1209, "7": 1209, "*": 1209, "2": 1336, "5": 1336,
          "8": 1336, "0": 1336, "3": 1477, "6": 1477, "9": 1477, "#": 1477}
    out = [0.0] * int(0.15 * FS)
    for d in digits:
        out += [0.15 * (math.sin(2 * math.pi * lo[d] * i / FS) +
                        math.sin(2 * math.pi * hi[d] * i / FS)) for i in range(int(0.2 * FS))]
        out += [0.0] * int(0.2 * FS)
    return out


# --------------------------------------------------------------------------
# AES-128-GCM, written out: the standard library has no AES, and an
# implementation that shares nothing with the daemon's is the better witness.

def _make_sbox():
    sbox = [0] * 256
    p = q = 1
    while True:
        p = (p ^ (p << 1) ^ (0x1B if p & 0x80 else 0)) & 0xFF
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        rot = lambda v, n: ((v << n) | (v >> (8 - n))) & 0xFF
        sbox[p] = q ^ rot(q, 1) ^ rot(q, 2) ^ rot(q, 3) ^ rot(q, 4) ^ 0x63
        if p == 1:
            break
    sbox[0] = 0x63
    return sbox


SBOX = _make_sbox()
M2 = [((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1 for a in range(256)]
M3 = [M2[a] ^ a for a in range(256)]
SHIFT = [0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11]


def aes_expand(key):
    w = [list(key[i:i + 4]) for i in range(0, 16, 4)]
    rcon = 1
    for i in range(4, 44):
        t = list(w[i - 1])
        if i % 4 == 0:
            t = [SBOX[t[1]] ^ rcon, SBOX[t[2]], SBOX[t[3]], SBOX[t[0]]]
            rcon = M2[rcon]
        w.append([a ^ b for a, b in zip(w[i - 4], t)])
    return [sum(w[4 * r:4 * r + 4], []) for r in range(11)]


def aes_block(rk, block):
    s = [b ^ k for b, k in zip(block, rk[0])]
    for r in range(1, 10):
        s = [SBOX[s[i]] for i in SHIFT]
        t = []
        for c in range(0, 16, 4):
            a0, a1, a2, a3 = s[c:c + 4]
            t += [M2[a0] ^ M3[a1] ^ a2 ^ a3, a0 ^ M2[a1] ^ M3[a2] ^ a3,
                  a0 ^ a1 ^ M2[a2] ^ M3[a3], M3[a0] ^ a1 ^ a2 ^ M2[a3]]
        s = [x ^ k for x, k in zip(t, rk[r])]
    return bytes(SBOX[s[i]] ^ k for i, k in zip(SHIFT, rk[10]))


def gf_mult(x, y):
    z, v = 0, x
    for i in range(127, -1, -1):
        if (y >> i) & 1:
            z ^= v
        v = (v >> 1) ^ 0xE1000000000000000000000000000000 if v & 1 else v >> 1
    return z


class Gcm:
    def __init__(self, key):
        self.rk = aes_expand(key)
        self.h = int.from_bytes(aes_block(self.rk, bytes(16)), "big")

    def _ctr(self, iv, data):
        out = bytearray()
        for i in range(0, len(data), 16):
            ks = aes_block(self.rk, iv + (2 + i // 16).to_bytes(4, "big"))
            out += bytes(a ^ b for a, b in zip(data[i:i + 16], ks))
        return bytes(out)

    def _tag(self, iv, aad, ct):
        y = 0
        for data in (aad, ct):
            for i in range(0, len(data), 16):
                y = gf_mult(y ^ int.from_bytes(data[i:i + 16].ljust(16, b"\0"), "big"), self.h)
        y = gf_mult(y ^ ((len(aad) * 8) << 64 | (len(ct) * 8)), self.h)
        return (y ^ int.from_bytes(aes_block(self.rk, iv + b"\0\0\0\1"), "big")).to_bytes(16, "big")

    def seal(self, iv, aad, pt):
        ct = self._ctr(iv, pt)
        return ct, self._tag(iv, aad, ct)

    def open(self, iv, aad, ct, tag):
        if not hmac.compare_digest(self._tag(iv, aad, ct)[:len(tag)], tag):
            return None
        return self._ctr(iv, ct)


def crypto_selftest():
    """FIPS-197 appendix C.1 and the GCM specification's test case 4."""
    rk = aes_expand(bytes(range(16)))
    assert aes_block(rk, bytes.fromhex("00112233445566778899aabbccddeeff")).hex() == \
        "69c4e0d86a7b0430d8cdb78070b4c55a", "AES known-answer test"
    g = Gcm(bytes.fromhex("feffe9928665731c6d6a8f9467308308"))
    pt = bytes.fromhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
                       "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39")
    aad = bytes.fromhex("feedfacedeadbeeffeedfacedeadbeefabaddad2")
    ct, tag = g.seal(bytes.fromhex("cafebabefacedbaddecaf888"), aad, pt)
    assert ct.hex() == ("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
                        "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091"), "GCM ciphertext"
    assert tag.hex() == "5bc94fbc3221a5db94fae95ae7121a47", "GCM tag"


# --------------------------------------------------------------------------
# the SvxReflector stand-in

def s_(text):                           # a protocol string: [u16 length][bytes]
    b = text.encode() if isinstance(text, str) else text
    return struct.pack(">H", len(b)) + b


class Link:
    """Length-prefixed frames on a TCP (later TLS) socket."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def send(self, mtype, body=b""):
        self.sock.sendall(struct.pack(">IH", len(body) + 2, mtype) + body)

    def recv(self, timeout):
        """(type, body); (None, None) when the peer is gone; TimeoutError."""
        deadline = time.monotonic() + timeout
        while True:
            if len(self.buf) >= 4:
                n = struct.unpack(">I", self.buf[:4])[0]
                if len(self.buf) >= 4 + n:
                    frame, self.buf = self.buf[4:4 + n], self.buf[4 + n:]
                    return struct.unpack(">H", frame[:2])[0], frame[2:]
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError
            self.sock.settimeout(left)
            try:
                chunk = self.sock.recv(65536)
            except (socket.timeout, ssl.SSLWantReadError):
                raise TimeoutError
            except (ConnectionError, ssl.SSLError, OSError):
                return None, None
            if not chunk:
                return None, None
            self.buf += chunk


class Reflector:
    def __init__(self, tmp, proto=3, auth_key=None, port=0):
        self.tmp, self.proto, self.auth_key = tmp, proto, auth_key
        self.tcp = socket.socket()
        self.tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.tcp.bind(("127.0.0.1", port))
        self.tcp.listen(4)
        self.port = self.tcp.getsockname()[1]
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(("127.0.0.1", self.port))
        self.udp.settimeout(0.2)
        self.client_id = 0x2A17
        self.events = []
        self.cond = threading.Condition()
        self.outq = queue.SimpleQueue()
        self.stop = False
        self.gcm = self.iv_rand = self.peer = None
        self.tx_counter = 0
        self.seen_initial = False
        self.signed = 0
        if proto == 3:
            self._make_ca()
        for fn in (self._accept_loop, self._udp_loop):
            threading.Thread(target=fn, daemon=True).start()

    # ---- bookkeeping ----
    def ev(self, kind, data=None):
        with self.cond:
            self.events.append((time.monotonic(), kind, data))
            self.cond.notify_all()

    def mark(self):
        with self.cond:
            return len(self.events)

    def wait(self, kind, timeout, start=0, pred=None):
        """First event of `kind` at index >= start: (index, time, data), or None."""
        deadline = time.monotonic() + timeout
        with self.cond:
            i = start
            while True:
                while i < len(self.events):
                    t, k, d = self.events[i]
                    if k == kind and (pred is None or pred(d)):
                        return i, t, d
                    i += 1
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                self.cond.wait(left)

    def all(self, kind, start=0, end=None):
        with self.cond:
            return [(t, d) for t, k, d in self.events[start:end] if k == kind]

    # ---- certificates ----
    def _ossl(self, *args):
        subprocess.run(("openssl",) + args, check=True, capture_output=True)

    def _san(self):
        p = f"{self.tmp}/san.cnf"
        with open(p, "w") as f:
            f.write("subjectAltName=DNS:localhost,IP:127.0.0.1\n")
        return p

    def _make_ca(self):
        t = self.tmp
        if not os.path.exists(f"{t}/srv.pem"):          # a kept directory: the same CA again
            self._ossl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{t}/ca.key",
                       "-out", f"{t}/ca.pem", "-subj", "/CN=Test Reflector CA", "-days", "2",
                       "-addext", "basicConstraints=critical,CA:TRUE")
            self._ossl("req", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{t}/srv.key",
                       "-out", f"{t}/srv.csr", "-subj", "/CN=localhost")
            self._ossl("x509", "-req", "-in", f"{t}/srv.csr", "-CA", f"{t}/ca.pem", "-CAkey",
                       f"{t}/ca.key", "-CAcreateserial", "-out", f"{t}/srv.pem", "-days", "2",
                       "-extfile", self._san())
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(f"{t}/srv.pem", f"{t}/srv.key")
        self.ctx.load_verify_locations(f"{t}/ca.pem")
        self.ctx.verify_mode = ssl.CERT_OPTIONAL
        self.ca_pem = open(f"{t}/ca.pem").read()

    def sign_file(self, csr_path, out_path):
        """Sign a request; returns the certificate with the CA behind it — the
        chain a reflector hands out — and its serial number."""
        t = self.tmp
        self._ossl("x509", "-req", "-in", csr_path, "-CA", f"{t}/ca.pem", "-CAkey",
                   f"{t}/ca.key", "-CAcreateserial", "-out", out_path, "-days", "2")
        serial = subprocess.run(("openssl", "x509", "-in", out_path, "-noout", "-serial"),
                                capture_output=True, text=True).stdout.strip().split("=")[-1]
        return open(out_path).read() + self.ca_pem, serial

    def sign_expired(self, csr_path, out_path):
        """The same, but a certificate whose year was 2020."""
        t = self.tmp
        os.makedirs(f"{t}/ca-db/new", exist_ok=True)
        open(f"{t}/ca-db/index.txt", "w").close()
        with open(f"{t}/ca-db/serial", "w") as f:
            f.write("2020\n")
        with open(f"{t}/ca-db/ca.cnf", "w") as f:
            f.write(f"""[ca]
default_ca = test
[test]
database = {t}/ca-db/index.txt
new_certs_dir = {t}/ca-db/new
serial = {t}/ca-db/serial
default_md = sha256
unique_subject = no
policy = any
[any]
commonName = supplied
""")
        self._ossl("ca", "-batch", "-notext", "-config", f"{t}/ca-db/ca.cnf", "-cert", f"{t}/ca.pem",
                   "-keyfile", f"{t}/ca.key", "-in", csr_path, "-out", out_path,
                   "-startdate", "20200101000000Z", "-enddate", "20210101000000Z")
        return open(out_path).read() + self.ca_pem

    def _sign(self, csr_pem):
        t = self.tmp
        self.signed += 1
        with open(f"{t}/node.csr", "w") as f:
            f.write(csr_pem)
        subj = subprocess.run(("openssl", "req", "-in", f"{t}/node.csr", "-noout", "-subject",
                               "-verify"), capture_output=True, text=True).stdout
        pub = subprocess.run(("openssl", "req", "-in", f"{t}/node.csr", "-noout", "-pubkey"),
                             capture_output=True, text=True).stdout
        self.ev("csr-pubkey", pub)
        chain, _ = self.sign_file(f"{t}/node.csr", f"{t}/node.pem")
        return subj, chain

    # ---- TCP ----
    def _accept_loop(self):
        while not self.stop:
            try:
                conn, _ = self.tcp.accept()
            except OSError:
                return
            try:
                self._session(conn)
            except Exception as e:                      # a broken test, not a broken node
                self.ev("session-error", repr(e))
            finally:
                try:
                    conn.close()
                except OSError:
                    pass
                self.ev("disconnect")

    def _expect(self, link, want):
        while True:
            t, body = link.recv(5.0)
            if t == 1:                                   # heartbeats may come at any time
                continue
            if t != want:
                raise RuntimeError(f"expected message {want}, got {t}")
            return body

    def _session(self, conn):
        link = Link(conn)
        ver = struct.unpack(">HH", self._expect(link, 5))
        self.ev("protover", ver)
        if self.proto == 2:
            if ver[0] != 2:
                link.send(6, struct.pack(">HH", 2, 0))           # "the most I speak is 2.0"
                ver = struct.unpack(">HH", self._expect(link, 5))
                self.ev("protover", ver)
        else:
            link.send(19, struct.pack(">H", len(self.ca_pem)) + s_(b"\0" * 32))     # CAInfo
            self._expect(link, 20)
            link.send(21, s_(self.ca_pem) + s_(b"sig") + s_(""))                    # the CA bundle
            self._expect(link, 14)
            link.send(15)
            assert not link.buf, "bytes arrived before the TLS handshake"
            conn.settimeout(10)
            link.sock = self.ctx.wrap_socket(conn, server_side=True)
            self.ev("tls", link.sock.version())
            cert = link.sock.getpeercert()
            if not cert:
                link.send(16)                                                        # send me a CSR
                csr = self._expect(link, 17)
                pem = csr[2:2 + struct.unpack(">H", csr[:2])[0]].decode()
                subj, crt = self._sign(pem)
                self.ev("csr", subj)
                link.send(18, s_(crt) + s_(""))
                try:
                    while link.recv(3.0)[0] is not None:                             # it reconnects
                        pass
                except TimeoutError:
                    pass
                return
            self.ev("cert-serial", cert.get("serialNumber", "").lstrip("0").upper())
            self.ev("cert-login", dict(x[0] for x in cert["subject"]).get("commonName"))

        challenge = os.urandom(20)
        link.send(10, s_(challenge))
        body = self._expect(link, 11)
        n = struct.unpack(">H", body[:2])[0]
        callsign = body[2:2 + n].decode()
        m = struct.unpack(">H", body[2 + n:4 + n])[0]
        digest = body[4 + n:4 + n + m]
        ok = True
        if self.auth_key is not None:
            ok = hmac.compare_digest(digest, hmac.new(self.auth_key.encode(), challenge,
                                                      hashlib.sha1).digest())
        self.ev("auth", (callsign, ok))
        if not ok:
            link.send(13, s_("Access denied"))
            return
        link.send(12)
        link.send(100, struct.pack(">HH", 0, self.client_id) + struct.pack(">H", 0) +
                  struct.pack(">H", 1) + s_("OPUS"))
        self.seen_initial = False
        self.tx_counter = 0
        if self.proto == 3:
            body = self._expect(link, 111)
            n = struct.unpack(">H", body[:2])[0]
            self.iv_rand = body[2:2 + n]
            body = body[2 + n:]
            n = struct.unpack(">H", body[:2])[0]
            self.gcm = Gcm(body[2:2 + n])
            body = body[2 + n:]
            n = struct.unpack(">H", body[:2])[0]
            self.ev("nodeinfo", body[2:2 + n].decode())
            link.send(114)                              # "use your own cipher for my direction too"
        self.ev("up")
        while not self.stop:
            try:
                t, body = link.recv(0.02)
            except TimeoutError:
                t = "idle"
            while not self.outq.empty():
                link.send(*self.outq.get())
            if t is None:
                return
            if t == 1:
                self.ev("tcp-hb")
            elif t == 106:
                self.ev("select", struct.unpack(">I", body[:4])[0])
            elif t == 107:
                n = struct.unpack(">H", body[:2])[0]
                self.ev("monitor", list(struct.unpack(f">{n}I", body[2:2 + 4 * n])))
            elif t == 112:                              # MsgSignalStrengthValues: (id, siglev, enabled, open, active)
                n = struct.unpack(">H", body[:2])[0]
                self.ev("siglev", [(chr(body[2 + 3 * i]), body[3 + 3 * i], bool(body[4 + 3 * i] & 1),
                                    bool(body[4 + 3 * i] & 2), bool(body[4 + 3 * i] & 4)) for i in range(n)]
                        if len(body) == 2 + 3 * n else None)
            elif t == 113:                              # MsgTxStatus: (id, transmitting)
                n = struct.unpack(">H", body[:2])[0]
                self.ev("txstatus", [(chr(body[2 + 2 * i]), bool(body[3 + 2 * i] & 1)) for i in range(n)]
                        if len(body) == 2 + 2 * n else None)
            elif t != "idle":
                self.ev("msg", t)

    def send_tcp(self, mtype, body=b""):
        self.outq.put((mtype, body))

    # ---- UDP ----
    def _udp_loop(self):
        while not self.stop:
            try:
                data, addr = self.udp.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            if self.proto == 2:
                if len(data) < 6:
                    continue
                mtype, cid, _seq = struct.unpack(">HHH", data[:6])
                payload = data[6:]
                if cid != self.client_id:
                    self.ev("udp-bad", "client id")
                    continue
            else:
                if self.gcm is None:
                    continue
                pt = None
                for aad_len in (4, 6):                  # the very first packet also names the client
                    aad, tag, ct = data[:aad_len], data[aad_len:aad_len + 8], data[aad_len + 8:]
                    iv = self.iv_rand + struct.pack(">H", self.client_id) + aad[:4]
                    pt = self.gcm.open(iv, aad, ct, tag)
                    if pt is not None:
                        break
                if pt is None or len(pt) < 2:
                    self.ev("udp-bad", len(data))
                    continue
                if aad_len == 6:
                    self.ev("udp-initial", struct.unpack(">IH", aad))
                mtype, payload = struct.unpack(">H", pt[:2])[0], pt[2:]
            self.peer = addr
            if mtype == 1:
                self.ev("udp-hb")
            elif mtype == 101:
                n = struct.unpack(">H", payload[:2])[0]
                self.ev("audio", payload[2:2 + n])
            elif mtype == 102:
                self.ev("flush")

    def send_udp(self, mtype, payload=b""):
        if self.peer is None:
            return
        if self.proto == 2:
            self.tx_counter += 1
            pkt = struct.pack(">HHH", mtype, 0, self.tx_counter & 0xFFFF) + payload
        else:
            self.tx_counter += 1
            aad = struct.pack(">I", self.tx_counter)
            ct, tag = self.gcm.seal(self.iv_rand + b"\0\0" + aad, aad, struct.pack(">H", mtype) + payload)
            pkt = aad + tag[:8] + ct
        self.udp.sendto(pkt, self.peer)

    def talk(self, tg, call, packets):
        """Somebody talks on the talkgroup: the packets go out in real time."""
        self.send_tcp(104, struct.pack(">I", tg) + s_(call))
        t0 = time.monotonic()
        for i, p in enumerate(packets):
            self.send_udp(101, s_(p))
            delay = t0 + (i + 1) * 0.02 - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        self.send_udp(102)
        self.send_tcp(105, struct.pack(">I", tg) + s_(call))
        return t0

    def close(self):
        self.stop = True
        for s in (self.tcp, self.udp):
            try:
                s.close()
            except OSError:
                pass


# --------------------------------------------------------------------------
# the SA818 stand-in

class Sa818:
    def __init__(self):
        self.master, self.slave = pty.openpty()
        tty.setraw(self.slave)
        self.path = os.ttyname(self.slave)
        self.lines = []
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        buf = b""
        while True:
            try:
                chunk = os.read(self.master, 256)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                cmd = line.strip().decode(errors="replace")
                if not cmd:
                    continue
                self.lines.append(cmd)
                if cmd == "AT+DMOCONNECT":
                    reply = "+DMOCONNECT:0"
                elif cmd == "AT+VERSION":
                    reply = "+VERSION:SA818_V5.0"
                elif cmd.startswith("AT+DMOSETGROUP"):
                    reply = "+DMOSETGROUP:0"
                elif cmd.startswith("AT+SETFILTER"):
                    reply = "+DMOSETFILTER:0"
                elif cmd.startswith("AT+DMOSETVOLUME"):
                    reply = "+DMOSETVOLUME:0"
                elif cmd == "RSSI?":
                    reply = "RSSI:137"
                else:
                    continue
                os.write(self.master, (reply + "\r\n").encode())

    def first(self, prefix):
        return next((l for l in self.lines if l.startswith(prefix)), None)


# --------------------------------------------------------------------------
# what went to the transmitter

class Recording:
    def __init__(self, path):
        with wave.open(path, "rb") as w:
            assert w.getnchannels() == 2 and w.getframerate() == FS and w.getsampwidth() == 2
            data = array.array("h")
            data.frombytes(w.readframes(w.getnframes()))
        if sys.byteorder == "big":
            data.byteswap()
        self.audio, self.ptt = data[0::2], data[1::2]
        nf = len(self.audio) // 320
        self.keyed = [self.ptt[f * 320] > 16000 for f in range(nf)]
        self.rms = [math.sqrt(sum(x * x for x in self.audio[f * 320:(f + 1) * 320]) / 320)
                    for f in range(nf)]
        self.bursts, start = [], None
        for f, k in enumerate(self.keyed):
            if k and start is None:
                start = f
            elif not k and start is not None:
                self.bursts.append((start * 0.02, f * 0.02))
                start = None
        if start is not None:
            self.bursts.append((start * 0.02, nf * 0.02))

    def span(self, t0, t1, thr=120.0):
        """First and last moment between t0 and t1 with audio on the channel."""
        on = [f for f in range(int(t0 / 0.02), min(int(t1 / 0.02), len(self.rms))) if self.rms[f] > thr]
        return (on[0] * 0.02, (on[-1] + 1) * 0.02) if on else (None, None)

    def audio_while_unkeyed(self):
        return sum(1 for f, k in enumerate(self.keyed) if not k and self.rms[f] > 0)

    def tone_hz(self, t0, t1):
        """Frequency of the loudest steady tone between t0 and t1 (zero crossings)."""
        f0, f1 = int(t0 / 0.02), min(int(t1 / 0.02), len(self.rms))
        top = max(self.rms[f0:f1], default=0)
        cross = frames = 0
        for f in range(f0, f1):
            if top > 0 and self.rms[f] > 0.9 * top:
                seg = self.audio[f * 320:(f + 1) * 320]
                cross += sum(1 for a, b in zip(seg, seg[1:]) if (a < 0) != (b < 0))
                frames += 1
        return cross / (2 * 0.02 * frames) if frames else 0.0

    def peak(self, t0, t1):
        return max((abs(x) for x in self.audio[int(t0 * FS):int(t1 * FS)]), default=0)

    def morse(self, t0, t1, freq, wpm):
        """Decode Morse keyed on a tone of `freq` Hz."""
        blk = FS // 100                                  # 10 ms
        w = 2 * math.cos(2 * math.pi * freq / FS)
        env = []
        for i in range(int(t0 * FS), min(int(t1 * FS), len(self.audio)) - blk, blk):
            q1 = q2 = 0.0
            for x in self.audio[i:i + blk]:
                q1, q2 = w * q1 - q2 + x, q1
            env.append(q1 * q1 + q2 * q2 - w * q1 * q2)
        if not env or max(env) <= 0:
            return "", None
        thr = 0.25 * max(env)
        dot = 120.0 / wpm                                # blocks per dot
        runs = []
        for v in env:
            on = v > thr
            if runs and runs[-1][0] == on:
                runs[-1][1] += 1
            else:
                runs.append([on, 1])
        first_on = next((i for i, v in enumerate(env) if v > thr), 0)
        text = sym = ""
        for on, n in runs:
            if on:
                sym += "." if n < 2 * dot else "-"
            elif sym:
                if n >= 2 * dot:
                    text += MORSE.get(sym, "?")
                    sym = ""
                if n >= 5 * dot:
                    text += " "
        if sym:
            text += MORSE.get(sym, "?")
        return text.strip(), t0 + first_on * 0.01


MORSE = {v: k for k, v in {
    "A": ".-", "B": "-...", "C": "-.-.", "D": "-..", "E": ".", "F": "..-.", "G": "--.",
    "H": "....", "I": "..", "J": ".---", "K": "-.-", "L": ".-..", "M": "--", "N": "-.",
    "O": "---", "P": ".--.", "Q": "--.-", "R": ".-.", "S": "...", "T": "-", "U": "..-",
    "V": "...-", "W": ".--", "X": "-..-", "Y": "-.--", "Z": "--..", "0": "-----",
    "1": ".----", "2": "..---", "3": "...--", "4": "....-", "5": ".....", "6": "-....",
    "7": "--...", "8": "---..", "9": "----.", "/": "-..-."}.items()}


# --------------------------------------------------------------------------
# a node under test

class Node:
    def __init__(self, exe, tmp, name, conf, fake_epoch, radio):
        self.name, self.tmp = name, tmp
        self.conf = f"{tmp}/{name}.conf"
        self.log_path = f"{tmp}/{name}.log"
        self.http = None
        m = re.search(r"http_port = (\d+)", conf)
        if m:
            self.http = int(m.group(1))
        with open(self.conf, "w") as f:
            f.write(conf)
        env = dict(os.environ, TZ="UTC", TAILCHASER_FAKE_EPOCH=str(fake_epoch),
                   TAILCHASER_BENCH_RADIO=radio.path)
        self.log = open(self.log_path, "w")
        self.t0 = time.monotonic()
        self.proc = subprocess.Popen([exe, "-c", self.conf], stderr=self.log, env=env)

    def status(self):
        return json.load(urllib.request.urlopen(f"http://127.0.0.1:{self.http}/status.json", timeout=3))

    def page(self):
        return urllib.request.urlopen(f"http://127.0.0.1:{self.http}/", timeout=3).read()

    def stop(self):
        self.proc.terminate()
        try:
            rc = self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            rc = -9
        self.log.close()
        return rc

    def logtext(self):
        return open(self.log_path).read()


class Checks:
    def __init__(self, title):
        self.title, self.lines, self.failed = title, [], 0

    def __call__(self, ok, what):
        self.lines.append(("  ok   " if ok else "  FAIL ") + what)
        if not ok:
            self.failed += 1
        return ok

    def note(self, text):
        self.lines.append("       " + text)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def near(v, want, tol):
    return v is not None and abs(v - want) <= tol


def sleep_until(t):
    d = t - time.monotonic()
    if d > 0:
        time.sleep(d)


# --------------------------------------------------------------------------
# scenario 1: protocol 3 — certificate, encrypted audio, the hourly identification

HOUR = calendar.timegm((2026, 3, 14, 12, 0, 0))


def keygen_allowance():
    """Whole seconds to put in front of scenario 1 on a slow machine. The node
    makes its RSA key before it can log in, and the first over of the scenario
    has to come after the login: 3.5 s is plenty on a desktop, and not enough
    on a Pi Zero 2 W, where a key takes 2-5 s."""
    worst = 0.0
    for _ in range(2):
        t = time.monotonic()
        subprocess.run(["openssl", "genrsa", "2048"], capture_output=True, check=False)
        worst = max(worst, time.monotonic() - t)
    return max(0, math.ceil(3.0 * worst - 1.5))


def scenario_cert(exe, tmp, wavs, check):
    ref = Reflector(tmp, proto=3)
    radio = Sa818()
    hp = free_port()
    slow = keygen_allowance()
    if slow:
        check.note(f"(slow machine: {slow} s more before the first over, for the node to make its key)")
    playlist = ",".join([f"pause:{3500 + slow * 1000}", wavs["voice"], "pause:3000", wavs["noise"], wavs["dtmf"],
                         "pause:2500", wavs["voice2"]])
    conf = f"""[reflector]
host = 127.0.0.1
port = {ref.port}
callsign = {CALL}
email = sysop@example.org
default_tg = 1745
monitor_tgs = 1745++, 9, 23
tg_select_timeout_s = 4
pki_dir = {tmp}/pki
location = "Testville"      # a comment after a value
sysop = ON0TST
latitude = 50.8467
longitude = 4.3525
locator = JO20eu

[location]
power_w = 25
antenna = X50 on the roof
antenna_height_m = 12

[radio]
freq = 145.450
ctcss_rx = 88.5
ctcss_tx = 123.0
squelch = 2
volume = 5

[tx]
delay_ms = 400
hang_ms = 0
timeout_s = 60

[ident]
callsign = ON0TST-S
interval_min = 60

[gate]
mode = on
hold_s = 2

[audio]
alsa_device = file:{playlist}
tx_record = {tmp}/tx1.wav

[status]
http_port = {hp}
bind = 127.0.0.1
"""
    node = Node(exe, tmp, "node1", conf, HOUR - 34 - slow, radio)
    node.t0 += slow             # everything below is timed from the start of the scenario proper
    try:
        # ---- enrollment and login ----
        e = ref.wait("csr", 6 + slow)
        check(e is not None and re.search(r"CN\s*=\s*" + CALL, e[2] or "") is not None,
              f"no certificate yet: the node sends a signing request for CN={CALL}")
        e = ref.wait("cert-login", 6)
        check(e is not None and e[2] == CALL,
              "it stores the signed certificate, reconnects and presents it (TLS client auth)")
        tls = ref.wait("tls", 1)
        check(tls is not None and tls[2] == "TLSv1.2", f"TLS 1.2 on the control channel ({tls and tls[2]})")
        e = ref.wait("auth", 3)
        check(e is not None and e[2] == (CALL, True), "login with its callsign")
        e = ref.wait("nodeinfo", 3)
        info = json.loads(e[2]) if e else {}
        check(info.get("sw") == "TailChaser" and info.get("nodeLocation") == "Testville"
              and info.get("sysop") == "ON0TST" and info.get("qth", [{}])[0].get("pos", {}).get("loc") == "JO20eu",
              "node information is valid JSON with location, sysop and position")
        q = info.get("qth", [{}])[0]
        rx, tx = q.get("rx", {}).get("A", {}), q.get("tx", {}).get("A", {})
        check(isinstance(q.get("pos", {}).get("lat"), float) and rx.get("freq") == 145.45 and tx.get("freq") == 145.45
              and rx.get("sqlType") == "CTCSS" and rx.get("ctcssFreq") == [88.5] and tx.get("ctcssFreq") == 123.0
              and info.get("toneToTalkgroup") == {"88.5": 1745},
              f"...and the station: {rx.get('freq')} MHz, tone {rx.get('ctcssFreq')} in / {tx.get('ctcssFreq')} out, "
              f"which tone opens the talkgroup")
        check(tx.get("pwr") == 25 and tx.get("ant") == [{"comment": "X50 on the roof", "height": 12}]
              and rx.get("ant") == tx.get("ant") and info.get("hidden") is False,
              f"...and what [location] adds: {tx.get('pwr')} W, antenna {tx.get('ant')}")
        e = ref.wait("udp-initial", 3)
        check(e is not None and e[2] == (0, ref.client_id),
              "the first UDP packet is encrypted with the negotiated key and names the client")
        e = ref.wait("select", 3)
        check(e is not None and e[2] == 1745, "home talkgroup 1745 is selected")
        e = ref.wait("monitor", 3)
        check(e is not None and e[2] == [9, 23], f"talkgroups 9 and 23 are monitored ({e and e[2]})")
        mode = stat.S_IMODE(os.stat(f"{tmp}/pki/{CALL}.key").st_mode)
        check(mode == 0o600, f"the private key is readable by the node only (mode {mode:o})")
        grp = radio.first("AT+DMOSETGROUP")
        check(grp is not None and grp.endswith("0,145.4500,145.4500,0018,2,0008"),
              f"SA818: 145.450 MHz, TX tone 123.0 Hz (18), squelch 2, RX tone 88.5 Hz (8)  [{grp}]")
        check(radio.first("AT+DMOSETVOLUME") == "AT+DMOSETVOLUME=5", "SA818: volume 5")

        # ---- a voice on the radio goes to the talkgroup ----
        a0 = ref.wait("audio", 8 + slow)
        time.sleep(0.8)                 # the SA818 is asked for its RSSI every 300 ms
        g = node.status()["gate"] if a0 else {}
        check(g.get("cos") and g.get("open") and g.get("rssi") == 137
              and (g.get("rssi_lo"), g.get("rssi_hi")) == (79, 100),
              "status while it receives: squelch and gate open, RSSI 137 on a scale of 79..100")
        fl = ref.wait("flush", 6, start=a0[0]) if a0 else None
        packets = [d for _, d in ref.all("audio", a0[0], fl[0])] if fl else []
        check(near(len(packets) * 0.02, 3.0, 0.2),
              f"the spoken over reaches the talkgroup whole: {len(packets) * 0.02:.2f} s of 3.00 s, then the end marker")
        if len(packets) < 100:
            check(False, "no audio to replay — the rest of this scenario cannot run")
            return
        ref.wait("siglev", 1.5, start=fl[0], pred=lambda d: d and not d[0][3])
        sl = [d for _, d in ref.all("siglev")]
        check(len(sl) > 0 and None not in sl and sl[0] == [("A", 0, True, False, False)],
              f"the reflector is told about the receiver as an SvxLink node does: first that it is there and closed ({sl[:1]})")
        heard = [d[0] for d in sl if d and d[0][3]]
        check(any(d == ("A", 100, True, True, True) for d in heard) and sl and sl[-1] == [("A", 0, True, False, False)],
              f"...then, during the over, its signal (100: RSSI 137 on 79..100), squelch open and listened to — and closed again after it ({sl[1:]})")
        check(len(sl) <= 12, f"...a change at once, the signal at most twice a second ({len(sl)} messages for a 3 s over)")

        # ---- the talkgroup talks: the transmitter is keyed ----
        time.sleep(0.3)
        live = {}
        threading.Timer(1.5, lambda: live.update(node.status())).start()
        t_b = ref.talk(1745, "ON4TST", packets)
        check(live.get("tx", {}).get("keyed") and live["tx"]["why"] == "talkgroup audio"
              and live["link"]["talker"] == "ON4TST" and live["gate"]["rssi"] == -1,
              "status while it transmits: on the air with talkgroup audio, ON4TST talking, no signal reading")
        txs = [d for _, d in ref.all("txstatus")]
        check(txs[:1] == [[("A", False)]] and [("A", True)] in txs,
              f"the reflector is told when the transmitter comes on ({txs[:3]})")

        # ---- talkgroup audio during a noise burst waits, and is then sent whole ----
        deadline = node.t0 + 16
        while time.monotonic() < deadline and not node.status()["gate"]["cos"]:
            time.sleep(0.04)
        m_noise = ref.mark()
        t_c = ref.talk(1745, "ON4TST", packets[:50])

        # ---- DTMF 9123# selects talkgroup 23; later it falls back ----
        e = ref.wait("select", 8, start=m_noise, pred=lambda tg: tg == 23)
        check(e is not None, "DTMF 9123# selects talkgroup 23")
        m = ref.wait("monitor", 2, start=e[0]) if e else None
        check(m is not None and m[2] == [9, 1745],
              f"...and the home talkgroup is monitored meanwhile ({m and m[2]})")
        e = ref.wait("select", 7, start=e[0] + 1 if e else 0, pred=lambda tg: tg == 1745)
        check(e is not None, "after 4 s of silence the node is back on its home talkgroup")

        # ---- a station is talking: talkgroup audio is not transmitted over it ----
        a1 = ref.wait("audio", 8, start=m_noise)
        check(a1 is not None and not ref.all("audio", fl[0], a1[0] if a1 else None),
              "noise and DTMF never reached the talkgroup")
        t_d = ref.talk(1745, "ON5BSY", packets[:75])
        fl2 = ref.wait("flush", 6, start=a1[0]) if a1 else None
        packets2 = [d for _, d in ref.all("audio", a1[0], fl2[0])] if fl2 else []
        n2 = len(packets2)
        check(near(n2 * 0.02, 2.8, 0.1), f"the second spoken over reaches the talkgroup too ({n2 * 0.02:.2f} s of 2.80 s)")
        last_second = packets2[-50:] if n2 >= 50 else packets[:50]

        # ---- the hour strikes ----
        sleep_until(node.t0 + 38.2)
        st = node.status()
        check(st["ident"]["count"] == 1 and abs(st["ident"]["last"] - HOUR) <= 1,
              "the identification went out when the clock struck the hour")

        # ---- somebody talks on a monitored talkgroup: follow them ----
        m_tg = ref.mark()
        ref.send_tcp(104, struct.pack(">I", 9) + s_("ON6MON"))
        e = ref.wait("select", 2, start=m_tg, pred=lambda tg: tg == 9)
        check(e is not None, "a talker on monitored talkgroup 9 makes the node select it")
        t_e = time.monotonic()
        for i, p in enumerate(last_second):                     # the END of that second over
            ref.send_udp(101, s_(p))
            sleep_until(t_e + (i + 1) * 0.02)
        ref.send_udp(102)
        ref.send_tcp(105, struct.pack(">I", 9) + s_("ON6MON"))
        e = ref.wait("select", 7, start=e[0] + 1 if e else m_tg, pred=lambda tg: tg == 1745)
        check(e is not None, "...and returns to the home talkgroup afterwards")

        time.sleep(0.5)                 # the status page is refreshed five times a second
        st = node.status()
        tot = st["totals"]
        check((tot["openings"], tot["passed"], tot["blocked"]) == (4, 2, 2),
              f"squelch openings: {tot['openings']} seen, {tot['passed']} relayed, {tot['blocked']} blocked (want 4/2/2)")
        check(tot["blocked_noise"] == 1 and tot["blocked_tone"] == 1 and tot["dtmf_commands"] == 1,
              "the blocked ones are named: one noise, one tone (the DTMF command)")
        check(tot["dtmf_digits"] == 5,
              f"five digits were heard, the command's — none in the two spoken overs ({tot['dtmf_digits']})")
        nh = {o["call"]: o for o in st["net_history"]}
        check(tot["net_overs"] == 4 and tot["net_blocked"] == 1 and
              nh.get("ON5BSY", {}).get("outcome") == "channel busy",
              f"talkgroup overs: {tot['net_overs']} heard, the one during the local over not transmitted")
        check(nh.get("ON6MON", {}).get("tg") == 9 and nh.get("ON6MON", {}).get("outcome") == "transmitted",
              "the over on talkgroup 9 is logged with its station and talkgroup")
        check(st["link"]["connected"] and st["link"]["tg"] == 1745 and st["link"]["connects"] == 1
              and st["link"]["proto"] == 3, "status: connected, protocol 3, one login")
        page = node.page()
        check(b"TailChaser" in page and b"id=smb" in page and b"id=tx" in page,
              "the status page is served, with its signal meter and transmitter card")
        check(len(ref.all("tcp-hb")) >= 5 and not ref.all("udp-bad") and not ref.all("session-error"),
              "heartbeats kept coming; every UDP packet authenticated")
    finally:
        rc = node.stop()
        ref.close()
    check(rc == 0, f"clean exit on SIGTERM (exit code {rc})")

    # ---- what was put on the air ----
    rec = Recording(f"{tmp}/tx1.wav")
    b = rec.bursts
    for i, (s, e_) in enumerate(b):
        check.note(f"transmission {i + 1}: {s:6.2f} s .. {e_:6.2f} s  ({e_ - s:.2f} s)")
    if not check(len(b) == 5, f"five transmissions: three overs, the DTMF answer, the identification (got {len(b)})"):
        return node
    check(rec.audio_while_unkeyed() == 0, "not one sample of audio without PTT")
    off = t_b - node.t0 - b[0][0]              # recording time 0 in test time

    s, e_ = b[0]
    on, last = rec.span(s, e_)
    check(near(on - s if on else None, 0.40, 0.07), f"carrier first: audio starts {on - s:.2f} s after PTT (TX delay 400 ms)")
    check(near(e_ - s, 3.44, 0.2) and last - on > 2.3,
          f"the over is transmitted whole ({e_ - s:.2f} s keyed, {last - on:.2f} s of voice) and the carrier drops with it")

    s, e_ = b[1]
    wait = s + off - (t_c - node.t0)
    on, last = rec.span(s, e_)
    check(0.3 <= wait <= 2.0, f"with the squelch open on noise the node waits ({wait:.2f} s) instead of keying over it")
    check(on is not None and last - on > 0.85, f"...and then sends the over from its first syllable ({last - on:.2f} s of 1.00 s)")

    s, e_ = b[2]
    text, _ = rec.morse(s, e_, 600, 24)
    check(text == "23", f"the DTMF command is answered in Morse: \"{text}\"")

    s, e_ = b[3]
    text, t_cw = rec.morse(s, e_, 600, 24)
    check(text == "ON0TST", f"the identification in Morse reads \"{text}\" — ON0TST-S without what follows the dash")
    check(32.6 <= s + off <= 34.4, f"keyed on the hour: {s + off:.2f} s after start (the hour struck between 33 and 34)")
    check(near(t_cw - s, 0.40, 0.07) and near(e_ - s, 0.40 + 61 * 0.05 + 0.04, 0.15),
          f"24 wpm: {e_ - s:.2f} s keyed for 61 dot lengths behind the TX delay")
    pk = rec.peak(s, e_)
    check(near(pk, 32767 * 10 ** (-12 / 20), 500), f"tone level -12 dBFS (peak {pk})")
    hz = rec.tone_hz(s, e_)
    check(near(hz, 600, 40), f"tone pitch 600 Hz (measured {hz:.0f} Hz)")

    s, e_ = b[4]
    on, last = rec.span(s, e_)
    check(on is not None and last - on > 0.85, f"the over from talkgroup 9 is on the air ({last - on:.2f} s)")
    # what was replayed there is the last second of the over that ended on a
    # syllable: its last word must be in it, right up to the end
    quiet = (s + 0.40 + 1.00) - last if last else 9
    check(quiet < 0.09, f"the last word of an over is not cut off: voice until {quiet * 1000:.0f} ms before its end")
    busy_from, busy_to = t_d - node.t0 - off, t_d - node.t0 - off + 1.5
    check(not any(s < busy_to and e_ > busy_from for s, e_ in b),
          "no carrier while the local station was talking")
    return node


# --------------------------------------------------------------------------
# scenario 2: protocol 2 — password, plain UDP; identification over audio; time-out

MINUTE = calendar.timegm((2026, 3, 14, 12, 17, 0))


def scenario_password(exe, tmp, wavs, check):
    ref = Reflector(tmp, proto=2, auth_key="e2e-secret")
    radio = Sa818()
    hp = free_port()
    conf = f"""[reflector]
host = 127.0.0.1:{ref.port}
callsign = {CALL}
auth_key = e2e-secret
default_tg = 7

[radio]
freq_rx = 433.500
freq_tx = 433.500
bandwidth = wide
ctcss_tx = 79.7
squelch = 3

[tx]
duck_db = 0
delay_ms = 200
hang_ms = 300
timeout_s = 5

[ident]
callsign = on0tst
interval_min = 1
wpm = 30
pitch_hz = 2200
level_db = -9

[gate]
mode = off

[audio]
alsa_device = file:pause:1000,{wavs['voice']}
tx_record = {tmp}/tx2.wav

[status]
http_port = {hp}
bind = 127.0.0.1
"""
    node = Node(exe, tmp, "node2", conf, MINUTE - 8, radio)
    try:
        e = ref.wait("auth", 6)
        vers = [d for _, d in ref.all("protover")]
        check(vers == [(3, 0), (2, 0)], f"an old reflector: the node steps down from protocol 3 to 2 ({vers})")
        check(e is not None and e[2] == (CALL, True), "password login: HMAC-SHA1 of the challenge")
        e = ref.wait("select", 3)
        check(e is not None and e[2] == 7, "talkgroup 7 selected")
        grp = radio.first("AT+DMOSETGROUP")
        check(grp is not None and grp.endswith("1,433.5000,433.5000,0005,3,0000"),
              f"SA818: 433.500 MHz wide, TX tone 79.7 Hz (5), no RX tone  [{grp}]")

        a0 = ref.wait("audio", 6)
        fl = ref.wait("flush", 6, start=a0[0]) if a0 else None
        packets = [d for _, d in ref.all("audio", a0[0], fl[0])] if fl else []
        check(near(len(packets) * 0.02, 3.0, 0.1),
              f"gate off: the opening is relayed as it is ({len(packets) * 0.02:.2f} s), over plain UDP")
        if len(packets) < 100:
            check(False, "no audio to replay — the rest of this scenario cannot run")
            return
        st = node.status()
        check(st["history"] and st["history"][0]["rssi"] == 137 and st["history"][0]["tg"] == 7,
              "signal strength is read from the SA818 while the squelch is open")

        # an over during which the minute strikes
        time.sleep(1.0)
        t_e = ref.talk(7, "ON7AAA", packets)
        # one that goes on for too long
        time.sleep(3.5)
        t_f = ref.talk(7, "ON7BBB", packets * 3)
        # and a normal one after it
        time.sleep(1.0)
        ref.talk(7, "ON7CCC", packets[:50])
        time.sleep(2.0)

        st = node.status()
        nh = {o["call"]: o["outcome"] for o in st["net_history"]}
        check(st["tx"]["timeouts"] == 1 and nh.get("ON7BBB") == "timed out",
              "the long over is cut by the transmit time-out and logged as such")
        check(nh.get("ON7AAA") == "transmitted" and nh.get("ON7CCC") == "transmitted",
              "the overs before and after it are transmitted")
        check(st["ident"]["count"] == 1 and st["link"]["proto"] == 2, "status: one identification, protocol 2")
        check(not ref.all("udp-bad") and not ref.all("session-error"), "no protocol errors")
    finally:
        rc = node.stop()
        ref.close()
    check(rc == 0, f"clean exit on SIGTERM (exit code {rc})")

    rec = Recording(f"{tmp}/tx2.wav")
    b = rec.bursts
    for i, (s, e_) in enumerate(b):
        check.note(f"transmission {i + 1}: {s:6.2f} s .. {e_:6.2f} s  ({e_ - s:.2f} s)")
    if not check(len(b) == 3, f"three transmissions (got {len(b)})"):
        return node
    check(rec.audio_while_unkeyed() == 0, "not one sample of audio without PTT")
    off = t_e - node.t0 - b[0][0]

    s, e_ = b[0]
    on, _ = rec.span(s, e_)
    text, t_cw = rec.morse(s, e_, 2200, 30)
    check(text == "ON0TST", f"the minute strikes during an over: the callsign is sent over the audio — \"{text}\"")
    check(t_cw is not None and 6.9 <= t_cw + off <= 8.4 and on is not None and t_cw - on > 0.5,
          f"...starting on the minute ({t_cw + off:.2f} s after start), {t_cw - on:.2f} s into the over")
    check(e_ > t_cw + 61 * 0.04, "the carrier stays up until the callsign is complete")

    s, e_ = b[1]
    check(near(e_ - s, 5.0, 0.12), f"time-out: the carrier drops after {e_ - s:.2f} s (limit 5 s)")
    check(near(s + off, t_f - node.t0, 0.15) and b[2][0] + off > t_f - node.t0 + 9.0,
          "...and stays down until that over has ended")

    s, e_ = b[2]
    check(near(e_ - s, 0.2 + 1.0 + 0.04 + 0.3, 0.15),
          f"TX delay 200 ms + 1.00 s of audio + hang 300 ms = {e_ - s:.2f} s keyed")
    return node


# --------------------------------------------------------------------------
# scenario 3: a node that was migrated — its key and certificate were made by
# another program (SvxLink), and certificates do not last for ever

CALL_M = "ON0TST-M"


def scenario_migrated(exe, tmp, wavs, check):
    ref = Reflector(tmp, proto=3)
    radio = Sa818()
    pki = f"{tmp}/pki"
    os.mkdir(pki)
    key, csr, crt = (f"{pki}/{CALL_M}.{ext}" for ext in ("key", "csr", "crt"))
    ref._ossl("genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", key)
    os.chmod(key, 0o600)
    ref._ossl("req", "-new", "-key", key, "-subj", f"/CN={CALL_M}", "-out", csr)
    pub = subprocess.run(("openssl", "pkey", "-in", key, "-pubout"),
                         capture_output=True, text=True, check=True).stdout
    key_bytes = open(key, "rb").read()
    chain, serial = ref.sign_file(csr, f"{tmp}/first.pem")
    with open(crt, "w") as f:
        f.write(chain)
    conf = f"""[reflector]
host = 127.0.0.1
port = {ref.port}
callsign = on0tst-m
default_tg = 77
pki_dir = {pki}

[radio]
freq = 145.2375

[ident]
interval_min = 0

[audio]
alsa_device = file:pause:1000

[status]
http_port = 0
"""

    # ---- a certificate that is good: used as it is ----
    node = Node(exe, tmp, "node3a", conf, HOUR + 600, radio)
    rc = None
    try:
        e = ref.wait("cert-login", 6)
        check(e is not None and e[2] == CALL_M and not ref.all("csr"),
              "key + certificate chain written by another program: the node logs in with them, no new request")
        e = ref.wait("cert-serial", 1)
        check(e is not None and e[2] == serial.lstrip("0").upper(), "...presenting that very certificate")
        e = ref.wait("select", 4)
        check(e is not None and e[2] == 77, "logged in, talkgroup 77 selected")

        # a certificate for somebody else's key is refused
        m = ref.mark()
        ref.send_tcp(18, s_(open(f"{tmp}/srv.pem").read() + ref.ca_pem) + s_(""))
        time.sleep(0.8)
        check(not ref.all("disconnect", m) and "does not belong to this node" in node.logtext()
              and open(crt).read() == chain,
              "a certificate that does not belong to its key is ignored: the stored one is kept")

        # the reflector renews the certificate and hands it over during the session
        renewed, serial2 = ref.sign_file(csr, f"{tmp}/renewed.pem")
        m = ref.mark()
        ref.send_tcp(18, s_(renewed) + s_(""))
        e = ref.wait("cert-serial", 6, start=m)
        check(e is not None and e[2] == serial2.lstrip("0").upper() and serial2 != serial,
              "a renewed certificate sent during the session is stored, and the node logs in again with it")
        e = ref.wait("select", 4, start=m)
        check(e is not None and e[2] == 77 and open(crt).read() == renewed, "...back on its talkgroup")
    finally:
        rc = node.stop()
    check(rc == 0, f"clean exit on SIGTERM (exit code {rc})")
    check("valid until" in node.logtext(), "the log says until when the certificate is valid")

    # ---- a certificate that has expired: asked for again, with the same key ----
    expired = ref.sign_expired(csr, f"{tmp}/expired.pem")
    for name, drop_csr, what in (
            ("node3b", False, "an expired certificate is not presented: the node sends its request again"),
            ("node3c", True, "...and when the request file is gone too, a new request is made for the SAME key")):
        with open(crt, "w") as f:
            f.write(expired)
        if drop_csr:
            os.remove(csr)
        m = ref.mark()
        node = Node(exe, tmp, name, conf, HOUR + 600, radio)
        try:
            e = ref.wait("csr-pubkey", 6, start=m)
            check(e is not None and e[2] == pub and "it expired on" in node.logtext(), what)
            e = ref.wait("cert-login", 6, start=m)
            check(e is not None and e[2] == CALL_M and ref.wait("select", 4, start=m) is not None,
                  "   the reflector issues a new certificate and the node logs in with it")
        finally:
            rc = node.stop()
        check(rc == 0 and open(key, "rb").read() == key_bytes,
              "   clean exit; the private key was not touched")
    check(not ref.all("session-error") and not ref.all("udp-bad"), "no protocol errors")
    check("reflector certificate verified" in node.logtext(),
          "the reflector's certificate was checked against the CA bundle kept from the first connection")
    # somebody else answers on the reflector's address: a CA this node does not know
    ca_file = f"{pki}/ca-bundle.crt"
    ours = open(ca_file).read()
    ref._ossl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{tmp}/other.key",
              "-out", ca_file, "-subj", "/CN=Somebody Else", "-days", "2",
              "-addext", "basicConstraints=critical,CA:TRUE")
    other = open(ca_file).read()
    m = ref.mark()
    node = Node(exe, tmp, "node3d", conf, HOUR + 600, radio)
    try:
        e = ref.wait("auth", 5, start=m)
        check(e is None and "does not check against the CA bundle" in node.logtext(),
              "a reflector whose certificate is not from the CA this node knows is refused: no login")
        check(open(ca_file).read() == other and os.path.exists(ca_file + ".offered"),
              "...and the CA bundle it offers is put aside, not taken over")
    finally:
        node.stop()
    with open(ca_file, "w") as f:
        f.write(ours)
    ref.close()
    return node


# --------------------------------------------------------------------------

def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "./tailchaser-simplex"
    keep = "--keep" in sys.argv
    crypto_selftest()
    tmp = tempfile.mkdtemp(prefix="tailchaser-e2e-")
    wavs = {}
    # voice2 stops in the middle of a syllable: a talker who lets go of the PTT on the last word
    for name, samples in (("voice", voice(3.0)), ("voice2", voice(2.8)), ("noise", noise(1.0)),
                          ("dtmf", dtmf("9123#"))):
        wavs[name] = f"{tmp}/{name}.wav"
        write_wav(wavs[name], samples)

    runs = []
    for title, fn, sub in (("protocol 3: certificate, encrypted audio, hourly identification", scenario_cert, "a"),
                           ("protocol 2: password, identification over audio, time-out", scenario_password, "b"),
                           ("a migrated node: existing key and certificate, renewal, expiry", scenario_migrated, "c")):
        os.mkdir(f"{tmp}/{sub}")
        check = Checks(title)
        box = {}

        def run(fn=fn, check=check, sub=sub, box=box):
            try:
                box["node"] = fn(exe, f"{tmp}/{sub}", wavs, check)
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
        print("\n".join(check.lines))
        if check.failed:
            failed += check.failed
            for name in ("node1.log", "node2.log", "node3a.log", "node3b.log", "node3c.log"):
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
