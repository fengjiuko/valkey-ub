# Copyright (c) Valkey Contributors
# SPDX-License-Identifier: BSD-3-Clause
"""Run a private TCP deployment test. Starts two owned processes; no device I/O.

Uses generated ACL credentials and a fresh temporary data directory, never an
existing server/configuration. This is an application smoke test, not preflight.
"""
import argparse
import hashlib
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server", type=Path)
    parser.add_argument("cli", type=Path)
    args = parser.parse_args()
    server, cli = str(args.server.resolve()), str(args.cli.resolve())
    if os.name != "posix" or os.geteuid() == 0:
        parser.error("Run as a non-root Linux/Unix user")
    for binary in (server, cli):
        if not os.path.isfile(binary) or not os.access(binary, os.X_OK):
            parser.error("Not an executable file: " + binary)
        result = subprocess.run([binary, "--version"], check=True, capture_output=True, timeout=10)
        print(result.stdout.decode().strip(), flush=True)

    password = secrets.token_hex(32)
    environment = dict(os.environ, VALKEYCLI_AUTH=password)
    key = "valkey:deploy-baseline:" + secrets.token_hex(12)
    with tempfile.TemporaryDirectory(prefix="valkey-tcp-deployment-") as directory:
        root = Path(directory)
        acl = root / "users.acl"
        acl.write_text("user default off\nuser deployment-test on #" +
                       hashlib.sha256(password.encode()).hexdigest() +
                       " ~valkey:deploy-baseline:* +ping +set +get +del +save +info\n")
        acl.chmod(0o600)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        command = [server, "--bind", "127.0.0.1", "--port", str(port),
                   "--urma-port", "0", "--rdma-port", "0", "--cluster-enabled", "no",
                   "--protected-mode", "yes", "--daemonize", "no",
                   "--supervised", "no", "--save", "", "--appendonly", "no",
                   "--dir", directory, "--dbfilename", "baseline.rdb",
                   "--pidfile", str(root / "server.pid"), "--aclfile", str(acl), "--logfile", ""]
        client = [cli, "-h", "127.0.0.1", "-p", str(port), "--raw", "-e"]

        def call(*arguments, value=None, credentials=True, env=environment):
            argv = client + (["--user", "deployment-test"] if credentials else [])
            if value is not None:
                argv += ["-x"]
            return subprocess.run(argv + list(arguments), input=value, env=env,
                                  capture_output=True, timeout=10)

        def expect(expected, *arguments, value=None):
            result = call(*arguments, value=value)
            if result.returncode != 0 or result.stdout != expected:
                raise RuntimeError("Command failed: " + arguments[0] + " " +
                                   repr((result.returncode, result.stdout, result.stderr)))

        def info(section):
            result = call("INFO", section)
            if result.returncode != 0:
                raise RuntimeError("INFO failed: " + section)
            return dict(line.split(":", 1) for line in result.stdout.decode().splitlines()
                        if ":" in line and not line.startswith("#"))

        def stop(process):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                    raise RuntimeError("Owned server failed to stop gracefully")
            if process.returncode != 0:
                raise RuntimeError("Owned server exit code: " + str(process.returncode))

        run_ids = []
        value = bytes(range(256)) * 256 + b"\x00\r\n\xff"
        for cycle in (1, 2):
            log_path = root / ("server-%d.log" % cycle)
            with log_path.open("w+") as log:
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 15
                    while True:
                        if process.poll() is not None:
                            raise RuntimeError("Server exited before readiness")
                        ready = call("PING")
                        if ready.returncode == 0 and ready.stdout == b"PONG\n":
                            break
                        if time.monotonic() >= deadline:
                            raise RuntimeError("Authenticated readiness timed out")
                        time.sleep(0.05)
                    status = info("server")
                    if status.get("process_id") != str(process.pid):
                        raise RuntimeError("Connected process is not the owned test server")
                    run_ids.append(status["run_id"])
                    urma = info("urma")
                    for field in ("urma_listener_enabled", "urma_transports", "urma_buffer_used",
                                  "urma_initialization_failures"):
                        if urma.get(field) != "0":
                            raise RuntimeError("URMA is not inactive: " + field)
                    if cycle == 1:
                        no_auth = dict(os.environ)
                        no_auth.pop("VALKEYCLI_AUTH", None)
                        no_auth.pop("REDISCLI_AUTH", None)
                        rejected = call("PING", credentials=False, env=no_auth)
                        wrong = call("PING", env=dict(environment, VALKEYCLI_AUTH="wrong"))
                        if (rejected.returncode == 0 or b"NOAUTH" not in rejected.stdout + rejected.stderr
                                or wrong.returncode == 0
                                or b"WRONGPASS" not in wrong.stdout + wrong.stderr):
                            raise RuntimeError("Authentication rejection was not enforced")
                        expect(b"OK\n", "SET", key, "text", "EX", "3600")
                        expect(b"text\n", "GET", key)
                        expect(b"1\n", "DEL", key)
                        expect(b"OK\n", "SET", key, value=b"")
                        expect(b"\n", "GET", key)
                        expect(b"OK\n", "SET", key, value=value)
                        expect(value + b"\n", "GET", key)
                        for _ in range(10):
                            expect(b"PONG\n", "PING")
                        expect(b"OK\n", "SAVE")
                        if (root / "baseline.rdb").stat().st_size == 0:
                            raise RuntimeError("SAVE produced an empty RDB")
                        print("PASS: ACL/auth, text/empty/binary SET/GET/DEL, 10 reconnects, SAVE", flush=True)
                    else:
                        if run_ids[0] == run_ids[1]:
                            raise RuntimeError("Restart did not change run_id")
                        expect(value + b"\n", "GET", key)
                        expect(b"1\n", "DEL", key)
                        print("PASS: new process restored exact binary Value from RDB", flush=True)
                    print("PASS: cycle=%d owned_pid=%d port=%d urma_compiled=%s URMA inactive" %
                          (cycle, process.pid, port, urma.get("urma_compiled")), flush=True)
                except Exception:
                    log.flush()
                    print(log_path.read_text(), flush=True)
                    raise
                finally:
                    stop(process)
    if root.exists():
        raise RuntimeError("Private test directory was not removed")
    print("PASS: both owned processes exited cleanly; private test directory removed", flush=True)


if __name__ == "__main__":
    main()
