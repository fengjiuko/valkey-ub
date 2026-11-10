#!/usr/bin/env python3
"""Run the first 202 -> 204 RDMA baseline; Python standard library only."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time

SHA = "aff2d3350e57e1c222d28743b486a940b0ac992c"
HOST, PORT, KEYS, SIZE = "192.168.17.204", 16381, 10000, 256
WARMUP, DURATION, REPEATS = 5, 30, 5


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def query(command):
    return subprocess.check_output(command, stderr=subprocess.STDOUT, timeout=15).decode().strip()


def info_fields(text):
    return dict(line.split(":", 1) for line in text.splitlines() if ":" in line and not line.startswith("#"))


def metadata(out, source, role):
    if (source.parent / "source-sha.txt").read_text().strip() != SHA:
        raise RuntimeError("source-sha.txt does not match the validated SHA")
    data = {"role": role, "source_sha": SHA, "source": str(source), "wall_time_ns": time.time_ns(),
            "hostname": os.uname().nodename, "kernel": list(os.uname()), "affinity_policy": "OS default; no pinning",
            "collector_affinity": sorted(os.sched_getaffinity(0)), "clock_ticks": os.sysconf("SC_CLK_TCK"),
            "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), "binaries": {}}
    for binary in ("src/valkey-server", "src/valkey-cli", "src/valkey-benchmark", "tests/rdma/rdma-test"):
        path = source / binary
        if path.exists():
            data["binaries"][binary] = hashlib.sha256(path.read_bytes()).hexdigest()
    settings = source / "src/.make-settings"
    if settings.exists():
        shutil.copyfile(str(settings), str(out / "build-settings.txt"))
    for name, cmd in {"cpu": ["lscpu"], "rdma": ["ibv_devinfo"],
                      "route": ["ip", "route", "get", "192.168.17.204"],
                      "link": ["ip", "-s", "link", "show", "enp40s0f0np0"]}.items():
        try:
            (out / (name + ".txt")).write_text(query(cmd), encoding="utf-8")
        except (OSError, subprocess.SubprocessError) as error:
            data[name + "_error"] = str(error)
    save_json(out / "metadata.json", data)


def counters(out, label):
    paths = list(Path("/sys/class/infiniband/rocep40s0f0/ports/1/counters").glob("*"))
    paths += list(Path("/sys/class/infiniband/rocep40s0f0/ports/1/hw_counters").glob("*"))
    paths += list(Path("/sys/class/net/enp40s0f0np0/statistics").glob("*"))
    values = {"wall_time_ns": time.time_ns()}
    for path in paths:
        try:
            values[str(path)] = path.read_text().strip()
        except OSError:
            pass
    save_json(out / (label + "-device-counters.json"), values)


def read_process(pid):
    # Fields after the LAST ')' start at field 3 (state); comm can contain spaces.
    fields = Path("/proc/%d/stat" % pid).read_text().rsplit(")", 1)[1].split()
    return {"wall_time_ns": time.time_ns(), "monotonic_ns": time.monotonic_ns(),
            "user_ticks": int(fields[11]), "system_ticks": int(fields[12]),
            "start_ticks": int(fields[19]), "rss_bytes": int(fields[21]) * os.sysconf("SC_PAGE_SIZE"),
            "host_cpu_ticks": [int(x) for x in Path("/proc/stat").read_text().splitlines()[0].split()[1:]]}


class CpuSamples:
    def __init__(self, pid, path):
        self.pid, self.path = pid, path
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def run(self):
        previous = None
        try:
            with self.path.open("w", encoding="utf-8") as output:
                while True:
                    try:
                        row = read_process(self.pid)
                    except (OSError, ValueError, IndexError):
                        break
                    if previous and row["start_ticks"] != previous["start_ticks"]:
                        break
                    if previous:
                        seconds = (row["monotonic_ns"] - previous["monotonic_ns"]) / 1e9
                        ticks = row["user_ticks"] + row["system_ticks"] - previous["user_ticks"] - previous["system_ticks"]
                        row["cpu_percent_one_core"] = 100 * ticks / os.sysconf("SC_CLK_TCK") / seconds
                    output.write(json.dumps(row) + "\n")
                    output.flush()
                    previous = row
                    if self.stop_event.wait(1):
                        break
        except OSError as error:
            print("CPU collector failed: %s" % error, file=sys.stderr, flush=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *unused):
        self.stop_event.set()
        self.thread.join()


def stop_process(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def launch_logged(command, log, timeout, cpu_path=None):
    process = None
    with log.open("wb") as output:
        try:
            process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
            if cpu_path:
                with CpuSamples(process.pid, cpu_path):
                    rc = process.wait(timeout=timeout)
            else:
                rc = process.wait(timeout=timeout)
        finally:
            if process is not None:
                stop_process(process)
    log.with_suffix(".exit").write_text(str(rc) + "\n")
    if rc:
        raise RuntimeError("command exit=%d; see %s" % (rc, log))


def cli_command(source):
    return [str(source / "src/valkey-cli"), "--rdma", "-h", HOST, "-p", str(PORT), "--raw"]


def server(source, out):
    data = out / "data"
    data.mkdir()
    command = [str(source / "src/valkey-server"), "--port", "0", "--rdma-bind", HOST,
               "--rdma-port", str(PORT), "--urma-port", "0", "--protected-mode", "no",
               "--cluster-enabled", "no", "--appendonly", "no", "--save", "",
               "--maxmemory", "0", "--io-threads", "1", "--dir", str(data),
               "--daemonize", "no", "--logfile", "", "--loglevel", "notice"]
    save_json(out / "server-command.json", command)
    process = None
    try:
        with (out / "server.log").open("wb") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            save_json(out / "server-process.json", {"pid": process.pid, "wall_time_ns": time.time_ns()})
            with CpuSamples(process.pid, out / "server-cpu.jsonl"):
                for unused in range(15):
                    if process.poll() is not None:
                        raise RuntimeError("server exited; inspect server.log")
                    try:
                        if query(cli_command(source) + ["PING"]) == "PONG":
                            break
                    except (OSError, subprocess.SubprocessError):
                        pass
                    time.sleep(1)
                else:
                    raise RuntimeError("server did not become ready")
                print("READY: %s:%d; leave this terminal open. Ctrl+C after client finishes." % (HOST, PORT), flush=True)
                rc = process.wait()
                if rc:
                    raise RuntimeError("server exited with %d" % rc)
    finally:
        if process is not None:
            stop_process(process)
        counters(out, "after")


def parse_report(text):
    throughput = re.search(r"throughput summary:\s+([\d.]+) requests per second", text)
    latency = re.search(r"latency summary \(msec\):\s+avg\s+min\s+p50\s+p95\s+p99\s+max\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)", text)
    completed = re.search(r"(\d+) requests completed in ([\d.]+) seconds", text)
    if not throughput or not latency or not completed:
        raise RuntimeError("benchmark report incomplete; do not treat sample as valid")
    if not 29 <= float(completed[2]) <= 33:
        raise RuntimeError("unexpected measured duration: %s" % completed[2])
    if int(completed[1]) <= 0 or float(throughput[1]) <= 0:
        raise RuntimeError("benchmark did not complete any successful work")
    return {"qps": float(throughput[1]), "requests": int(completed[1]), "seconds": float(completed[2]),
            **dict(zip(("avg_ms", "min_ms", "p50_ms", "p95_ms", "p99_ms", "max_ms"), map(float, latency.groups())))}


def client(source, out):
    cli = cli_command(source)
    if query(cli + ["PING"]) != "PONG":
        raise RuntimeError("RDMA PING failed")
    server_info = info_fields(query(cli + ["INFO", "server"]))
    run_id = server_info["run_id"]
    save_json(out / "server-identity.json", server_info)
    # Refuse existing data or settings that would invalidate this baseline.
    for name, expected in {"port": "0", "rdma-port": str(PORT), "urma-port": "0", "io-threads": "1",
                           "appendonly": "no", "save": "", "maxmemory": "0", "cluster-enabled": "no"}.items():
        actual = query(cli + ["CONFIG", "GET", name]).splitlines()
        value = actual[1] if len(actual) > 1 else ""
        if not actual or actual[0] != name or value != expected:
            raise RuntimeError("unexpected server setting %s: %r" % (name, actual))
    if query(cli + ["DBSIZE"]) != "0":
        raise RuntimeError("expected a fresh EMPTY dedicated performance instance; refusing to erase data")
    common = [str(source / "src/valkey-benchmark"), "--rdma", "-h", HOST, "-p", str(PORT),
              "--threads", "1", "-c", "32", "-P", "1", "-d", str(SIZE), "-r", str(KEYS),
              "--seed", "20261110", "--precision", "3"]
    config = {"source_sha": SHA, "host": HOST, "port": PORT, "value_bytes": SIZE, "keys": KEYS,
              "connections": 32, "pipeline": 1, "client_threads": 1, "server_io_threads": 1,
              "warmup_seconds": WARMUP, "measurement_seconds": DURATION, "repeats_per_operation": REPEATS,
              "cpu_sampling": "1 second; 100% = one logical CPU; raw samples include warmup",
              "cpu_window_note": "warmup cutoff timestamps are approximate; use interior steady-state samples",
              "affinity_policy": "OS default; no pinning; reproduce this policy for initial comparison"}
    save_json(out / "workload.json", config)
    fill = common + ["-n", str(KEYS), "--sequential", "SET", "rdma-base:__rand_int__", "__data__"]
    print("Prefilling and checking all 10000 keys...", flush=True)
    launch_logged(fill, out / "prefill.log", 120)
    check = "for i=0,9999 do if server.call('STRLEN',string.format('rdma-base:%012d',i))~=256 then return i end end return 'OK'"
    if query(cli + ["DBSIZE"]) != str(KEYS) or query(cli + ["EVAL", check, "0"]) != "OK":
        raise RuntimeError("prefill verification failed")
    rows = []
    with (out / "rounds.jsonl").open("w", encoding="utf-8") as events:
        for round_number in range(1, REPEATS + 1):
            for op in (("SET", "GET") if round_number % 2 else ("GET", "SET")):
                label = "round-%d-%s" % (round_number, op.lower())
                before_text = query(cli + ["INFO", "all"])
                before = info_fields(before_text)
                (out / (label + "-before.info")).write_text(before_text)
                if before.get("run_id") != run_id or before.get("rdb_bgsave_in_progress") != "0":
                    raise RuntimeError("server restarted or background save is active")
                save_json(out / (label + "-clock-before.json"), {"client_ns": time.time_ns(), "server_time": query(cli + ["TIME"]), "client_after_ns": time.time_ns()})
                command = common + ["--warmup", str(WARMUP), "--duration", str(DURATION), op, "rdma-base:__rand_int__"]
                if op == "SET":
                    command += ["__data__"]
                save_json(out / (label + "-command.json"), command)
                start = time.time_ns()
                print("%s: warmup 5s + measure 30s" % label, flush=True)
                launch_logged(command, out / (label + ".log"), 65, out / (label + "-cpu.jsonl"))
                end = time.time_ns()
                if len((out / (label + "-cpu.jsonl")).read_text().splitlines()) < 20:
                    raise RuntimeError("client CPU sampling incomplete")
                after_text = query(cli + ["INFO", "all"])
                (out / (label + "-after.info")).write_text(after_text)
                after = info_fields(after_text)
                if after.get("run_id") != run_id:
                    raise RuntimeError("server restarted during measurement")
                for field in ("keyspace_misses", "evicted_keys", "total_error_replies", "rdb_saves"):
                    if int(after.get(field, 0)) != int(before.get(field, 0)):
                        raise RuntimeError("invalid sample: %s changed" % field)
                if op == "GET" and int(after["keyspace_hits"]) <= int(before["keyspace_hits"]):
                    raise RuntimeError("GET sample has no cache hits")
                row = {"round": round_number, "operation": op, **parse_report((out / (label + ".log")).read_text())}
                rows.append(row)
                events.write(json.dumps({**row, "start_ns": start, "end_ns": end,
                                         "approx_measurement_start_ns": start + WARMUP * 10**9}) + "\n")
                events.flush()
                with (out / "results.csv").open("w", newline="") as output:
                    writer = csv.DictWriter(output, fieldnames=list(rows[0]))
                    writer.writeheader()
                    writer.writerows(rows)
                print("%s: QPS=%.2f p50=%.3f p95=%.3f p99=%.3f ms" %
                      (label, row["qps"], row["p50_ms"], row["p95_ms"], row["p99_ms"]), flush=True)
                time.sleep(2)
    summary = {}
    for op in ("SET", "GET"):
        samples = [row for row in rows if row["operation"] == op]
        summary[op] = {field: {"median": statistics.median(row[field] for row in samples),
                              "min": min(row[field] for row in samples), "max": max(row[field] for row in samples)}
                       for field in ("qps", "p50_ms", "p95_ms", "p99_ms")}
    save_json(out / "summary.json", summary)
    counters(out, "after")
    (out / "COMPLETE").write_text("10 valid samples; CPU traces need steady-state window analysis\n")
    print(json.dumps(summary, indent=2), flush=True)
    print("COMPLETE: client measurement finished; now Ctrl+C the performance server only.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("role", choices=("server", "client"))
    parser.add_argument("--source", type=Path, required=True,
                        help="built source snapshot; parent must contain source-sha.txt")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("run on the Linux test hosts; --help is available on other platforms")
    source = args.source.resolve()
    base = Path.home() / "valkey-validation"
    base.mkdir(exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="rdma-perf-%s-" % args.role, dir=str(base)))
    print("EVIDENCE=%s" % out, flush=True)
    def interrupted(signum, frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupted)
    try:
        metadata(out, source, args.role)
        counters(out, "before")
        (server if args.role == "server" else client)(source, out)
    except KeyboardInterrupt:
        print("Stopped; evidence preserved: %s" % out, flush=True)
        return 130
    except Exception as error:
        (out / "FAILED.txt").write_text(str(error) + "\n")
        print("FAILED: %s\nEVIDENCE=%s" % (error, out), file=sys.stderr, flush=True)
        return 1
    finally:
        archive = shutil.make_archive(str(out), "gztar", root_dir=str(out.parent), base_dir=out.name)
        print("ARCHIVE=%s" % archive, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
