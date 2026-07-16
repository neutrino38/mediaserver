#!/usr/bin/env python3
"""
Client WebSocket minimal en Python pur (stdlib uniquement) + suite de tests.

Vérifie le serveur d'écho `wstest` (mcu/src/wstest.cpp) : handshake RFC 6455,
écho texte, écho binaire, ping/pong, fermeture propre. Sert de harnais de
non-régression pour les Phases 0 → 2 du refactor WebSocket (cf.
websocket-refactor.md). En Phase 2 (WSS), il suffira d'envelopper le socket dans
ssl.SSLContext (voir --tls, déjà câblé mais inutile en Phase 0/1).

Usage :
    python3 ws_client.py [--host H] [--port P] [--path /echo] [--tls]

Sortie : "PASS"/"FAIL" par test + résumé ; code de sortie != 0 si un test échoue.
"""
import argparse
import base64
import hashlib
import os
import socket
import ssl
import struct
import sys

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

OP_CONT  = 0x0
OP_TEXT  = 0x1
OP_BIN   = 0x2
OP_CLOSE = 0x8
OP_PING  = 0x9
OP_PONG  = 0xA


class WSClient:
    def __init__(self, host, port, path, use_tls=False, timeout=5.0):
        self.host = host
        self.port = port
        self.path = path
        self.use_tls = use_tls
        self.timeout = timeout
        self.sock = None

    def connect(self):
        raw = socket.create_connection((self.host, self.port), timeout=self.timeout)
        if self.use_tls:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            raw = ctx.wrap_socket(raw, server_hostname=self.host)
        raw.settimeout(self.timeout)
        self.sock = raw
        self._handshake()

    def _handshake(self):
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n" % (self.path, self.host, self.port, key)
        )
        self.sock.sendall(req.encode())

        # Read HTTP response headers up to CRLFCRLF
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(1024)
            if not chunk:
                raise RuntimeError("connection closed during handshake")
            buf += chunk
        head = buf.split(b"\r\n\r\n", 1)[0].decode(errors="replace")
        status = head.splitlines()[0]
        if "101" not in status:
            raise RuntimeError("bad upgrade status: %r" % status)

        # Validate Sec-WebSocket-Accept
        expected = base64.b64encode(
            hashlib.sha1((key + GUID).encode()).digest()
        ).decode()
        got = None
        for line in head.splitlines()[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                if k.strip().lower() == "sec-websocket-accept":
                    got = v.strip()
        if got != expected:
            raise RuntimeError("bad Sec-WebSocket-Accept: got %r want %r" % (got, expected))
        # Any leftover bytes after the header belong to the WS stream
        self._rxbuf = buf.split(b"\r\n\r\n", 1)[1]

    # ---- framing ---------------------------------------------------------
    def _send_frame(self, opcode, payload=b"", fin=True):
        b0 = (0x80 if fin else 0) | opcode
        n = len(payload)
        header = bytearray([b0])
        # Client MUST mask (RFC 6455 §5.3)
        if n < 126:
            header.append(0x80 | n)
        elif n <= 0xFFFF:
            header.append(0x80 | 126)
            header += struct.pack("!H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack("!Q", n)
        mask = os.urandom(4)
        header += mask
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def _recv_exact(self, n):
        while len(self._rxbuf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("connection closed while reading %d bytes" % n)
            self._rxbuf += chunk
        out, self._rxbuf = self._rxbuf[:n], self._rxbuf[n:]
        return out

    def _recv_frame(self):
        b0, b1 = self._recv_exact(2)
        fin = bool(b0 & 0x80)
        opcode = b0 & 0x0F
        masked = bool(b1 & 0x80)
        length = b1 & 0x7F
        if length == 126:
            length = struct.unpack("!H", self._recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._recv_exact(8))[0]
        mask = self._recv_exact(4) if masked else None
        payload = self._recv_exact(length) if length else b""
        if mask:  # servers should not mask, but handle it
            payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        return fin, opcode, payload

    def recv_message(self):
        """Reassemble a full message, transparently answering nothing to control
        frames except returning ping/pong/close to the caller."""
        data = b""
        first_op = None
        while True:
            fin, opcode, payload = self._recv_frame()
            if opcode in (OP_PING, OP_PONG, OP_CLOSE):
                return opcode, payload
            if first_op is None:
                first_op = opcode
            data += payload
            if fin:
                return first_op, data

    # ---- high level ------------------------------------------------------
    def send_text(self, s):
        self._send_frame(OP_TEXT, s.encode())

    def send_binary(self, b):
        self._send_frame(OP_BIN, b)

    def send_ping(self, b=b""):
        self._send_frame(OP_PING, b)

    def send_close(self):
        self._send_frame(OP_CLOSE, b"")

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


# ---- test suite ----------------------------------------------------------
class Runner:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def check(self, name, cond, detail=""):
        if cond:
            self.passed += 1
            print("  PASS  %s" % name)
        else:
            self.failed += 1
            print("  FAIL  %s  %s" % (name, detail))


def run_tests(host, port, path, use_tls):
    r = Runner()

    # T1 — handshake succeeds on a fresh connection
    try:
        c = WSClient(host, port, path, use_tls)
        c.connect()
        r.check("handshake", True)
    except Exception as e:
        r.check("handshake", False, str(e))
        return r

    # T2 — small text echo, 1:1
    try:
        c.send_text("hello")
        op, data = c.recv_message()
        r.check("echo-text-small", op == OP_TEXT and data == b"hello",
                "op=%d data=%r" % (op, data))
    except Exception as e:
        r.check("echo-text-small", False, str(e))

    # T3 — UTF-8 text echo
    try:
        msg = "héllo — accents ✓ 日本語"
        c.send_text(msg)
        op, data = c.recv_message()
        r.check("echo-text-utf8", op == OP_TEXT and data == msg.encode(),
                "data=%r" % data)
    except Exception as e:
        r.check("echo-text-utf8", False, str(e))

    # T4 — medium text (>125 bytes → 16-bit length on the server's echo frame)
    try:
        msg = "A" * 300
        c.send_text(msg)
        op, data = c.recv_message()
        r.check("echo-text-16bit-len", op == OP_TEXT and data == msg.encode(),
                "len=%d" % len(data))
    except Exception as e:
        r.check("echo-text-16bit-len", False, str(e))

    # T5 — three consecutive messages preserve order
    try:
        ok = True
        for i in range(3):
            c.send_text("msg-%d" % i)
        for i in range(3):
            op, data = c.recv_message()
            ok = ok and op == OP_TEXT and data == ("msg-%d" % i).encode()
        r.check("echo-order-3", ok)
    except Exception as e:
        r.check("echo-order-3", False, str(e))

    # T6 — ping/pong (server must return a Pong carrying the ping payload)
    try:
        c.send_ping(b"ping-1234")
        op, data = c.recv_message()
        r.check("ping-pong", op == OP_PONG and data == b"ping-1234",
                "op=%d data=%r" % (op, data))
    except Exception as e:
        r.check("ping-pong", False, str(e))

    # T7 — clean close: send Close, expect the socket to end
    try:
        c.send_close()
        closed = False
        try:
            for _ in range(5):
                op, data = c.recv_message()
                if op == OP_CLOSE:
                    closed = True
                    break
        except Exception:
            closed = True  # connection dropped == closed
        r.check("close", closed)
    except Exception as e:
        r.check("close", False, str(e))
    finally:
        c.close()

    # T8 — concurrence : N connexions simultanées servies par le poll() unique
    try:
        N = 15
        conns = []
        for i in range(N):
            cc = WSClient(host, port, path, use_tls)
            cc.connect()
            conns.append(cc)
        for i, cc in enumerate(conns):
            cc.send_text("c%d" % i)
        ok = True
        for i, cc in enumerate(conns):
            op, data = cc.recv_message()
            ok = ok and op == OP_TEXT and data == ("c%d" % i).encode()
        for cc in conns:
            cc.close()
        r.check("concurrency-%d" % N, ok)
    except Exception as e:
        r.check("concurrency", False, str(e))

    # T10 — backpressure : rafale de messages sans lire, puis lecture de tous les
    # échos dans l'ordre. Force des write() partiels côté serveur → valide le
    # tamponnage de sortie (pendingOut) de la Phase 4.
    try:
        d = WSClient(host, port, path, use_tls)
        d.connect()
        N = 100
        payload = "X" * 500
        for i in range(N):
            d.send_text("%04d-%s" % (i, payload))
        ok = True
        for i in range(N):
            op, data = d.recv_message()
            ok = ok and op == OP_TEXT and data == ("%04d-%s" % (i, payload)).encode()
        d.close()
        r.check("backpressure-%dx%d" % (N, len(payload)), ok)
    except Exception as e:
        r.check("backpressure", False, str(e))

    # T9 — isolation : un client qui ne lit pas sa réponse ne doit pas bloquer
    # les autres (clé du modèle mono-thread : écritures non bloquantes).
    try:
        a = WSClient(host, port, path, use_tls)
        a.connect()
        b = WSClient(host, port, path, use_tls)
        b.connect()
        # 'a' envoie mais on ne lira jamais son écho (lecteur « lent »/bloqué)
        a.send_text("for-a")
        # 'b' doit fonctionner normalement
        b.send_text("for-b")
        op, data = b.recv_message()
        r.check("slow-client-isolation",
                op == OP_TEXT and data == b"for-b", "data=%r" % data)
        a.close()
        b.close()
    except Exception as e:
        r.check("slow-client-isolation", False, str(e))

    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9001)
    ap.add_argument("--path", default="/echo")
    ap.add_argument("--tls", action="store_true")
    args = ap.parse_args()

    print("WebSocket test client → %s://%s:%d%s"
          % ("wss" if args.tls else "ws", args.host, args.port, args.path))
    r = run_tests(args.host, args.port, args.path, args.tls)
    print("---")
    print("Result: %d passed, %d failed" % (r.passed, r.failed))
    return 1 if r.failed else 0


if __name__ == "__main__":
    sys.exit(main())
