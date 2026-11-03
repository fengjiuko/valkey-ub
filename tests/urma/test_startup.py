# Copyright (c) Valkey Contributors
# SPDX-License-Identifier: BSD-3-Clause
"""No-device startup checks for both normal and BUILD_URMA builds."""
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="valkey-urma-startup-") as directory:
    base = [binary, "--save", "", "--appendonly", "no", "--dir", directory]
    cases = [
        (["--port", "0", "--urma-port", "6380", "--save", "3600 1"],
         ("requires a Linux BUILD_URMA=yes", "fork/persistence are not qualified")),
        (["--port", "0", "--urma-port", "6380"],
         ("requires a Linux BUILD_URMA=yes", "URMA requires urma-device")),
        (["--port", "0", "--urma-port", "6380", "--urma-device", "missing-test-device",
          "--urma-allow-insecure-bootstrap", "yes", "--io-threads", "2"],
         ("requires a Linux BUILD_URMA=yes", "URMA requires urma-device", "requires io-threads 1")),
        (["--port", "0", "--urma-port", "6380", "--urma-device", "missing-test-device",
          "--urma-allow-insecure-bootstrap", "yes"],
         ("requires a Linux BUILD_URMA=yes", "urma_init failed", "Device is unavailable")),
    ]
    for args, expected in cases:
        result = subprocess.run(base + args, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=10)
        if result.returncode == 0 or not any(message in result.stdout for message in expected):
            raise RuntimeError(result.stdout)
        if "Ready to accept connections" in result.stdout:
            raise RuntimeError("Invalid URMA configuration accepted")
    print("Four fail-closed startup cases: PASS")

    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    with (Path(directory) / "server.log").open("w+") as log:
        process = subprocess.Popen(base + ["--bind", "127.0.0.1", "--port", str(port)],
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while True:
                if process.poll() is not None:
                    log.seek(0)
                    raise RuntimeError(log.read())
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.2) as client:
                        client.sendall(b"*1\r\n$4\r\nPING\r\n")
                        reply = b""
                        while len(reply) < 7:
                            data = client.recv(7 - len(reply))
                            if not data:
                                raise RuntimeError("Unexpected EOF")
                            reply += data
                        if reply != b"+PONG\r\n":
                            raise RuntimeError(repr(reply))
                        break
                except (ConnectionRefusedError, TimeoutError):
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.02)
            print("Default TCP startup and PING without URMA device: PASS")
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
