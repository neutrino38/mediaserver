#!/usr/bin/env python3
"""
Orchestrateur du harnais de test WebSocket.

- lance le serveur d'écho `bin/debug/wstest` sur un port libre,
- attend que le port accepte les connexions,
- exécute ws_client.py (suite de tests),
- arrête le serveur et renvoie le code de sortie du client.

Usage :
    python3 test/websocket/run.py [--binary PATH] [--port P] [--verbose]

À exécuter depuis la racine du dépôt. Construire d'abord le binaire :
    make -C mcu wstest
"""
import argparse
import os
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def wait_port(host, port, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def ensure_selfsigned_cert():
    """Génère (si absent) un certificat auto-signé pour les tests WSS."""
    cert = os.path.join(HERE, "wstest.crt")
    key = os.path.join(HERE, "wstest.key")
    if os.path.exists(cert) and os.path.exists(key):
        return cert, key
    print("Generating self-signed test certificate...")
    rc = subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", key, "-out", cert, "-days", "3650",
         "-subj", "/CN=localhost"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ).returncode
    if rc != 0 or not (os.path.exists(cert) and os.path.exists(key)):
        return None, None
    return cert, key


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(ROOT, "bin", "debug", "wstest"))
    ap.add_argument("--port", type=int, default=9051)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--tls", action="store_true", help="teste en WSS (TLS)")
    args = ap.parse_args()

    if not os.path.exists(args.binary):
        print("ERROR: server binary not found: %s" % args.binary)
        print("Build it first:  make -C mcu wstest")
        return 2

    server_log = open(os.path.join(HERE, "wstest.server.log"), "wb")
    srv_args = [args.binary, str(args.port)]
    if args.verbose:
        srv_args.append("-d")
    if args.tls:
        cert, key = ensure_selfsigned_cert()
        if not cert:
            print("ERROR: could not generate a self-signed certificate (openssl missing?)")
            return 2
        srv_args += ["--secure", "--cert", cert, "--key", key]
    print("Launching server: %s" % " ".join(srv_args))
    server = subprocess.Popen(srv_args, stdout=server_log, stderr=subprocess.STDOUT)

    try:
        if not wait_port(args.host, args.port):
            print("ERROR: server did not open port %d in time" % args.port)
            return 2

        client_args = [sys.executable, os.path.join(HERE, "ws_client.py"),
                       "--host", args.host, "--port", str(args.port)]
        if args.tls:
            client_args.append("--tls")
        client = subprocess.run(client_args)
        rc = client.returncode
    finally:
        server.send_signal(signal.SIGTERM)
        try:
            server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server.kill()
        server_log.close()

    if rc != 0:
        print("--- server log tail ---")
        with open(os.path.join(HERE, "wstest.server.log"), "r", errors="replace") as f:
            for line in f.readlines()[-30:]:
                sys.stdout.write(line)

    print("Exit code: %d" % rc)
    return rc


if __name__ == "__main__":
    sys.exit(main())
