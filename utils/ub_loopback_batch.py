#!/usr/bin/env python3
"""Run isolated UB SEND cases, continue after failures, then retest failures once."""
import argparse
import csv
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import send_perftest_report as report

HERE = Path(__file__).resolve().parent
ATTEMPT_FIELDS = ["stage", "case", "op", "bytes", "round", "control_port",
                  "collector_exit", "run", "valid", "error"]


def plan_cases(sizes, ops, rounds):
    cases = []
    for number in range(1, rounds + 1):
        ordered = ops[::-1] if number % 2 == 0 else ops
        for size in sizes:
            for op in ordered:
                cases.append(dict(case=f"measured-{op}-{size}-r{number}",
                                  op=op, bytes=size, round=number))
    return cases


def read_case(folder, case, collector_exit):
    record = dict(case, valid=False, error="")
    try:
        _, records, planned = report.collect(folder)
        if len(records) != 1 or planned != 1:
            raise ValueError("expected exactly one planned and attempted formal pair; inspect warmup logs")
        source = records[0]
        if source["op"] != case["op"] or source["bytes"] != case["bytes"]:
            raise ValueError("child result does not match requested case")
        record.update(source)
        record.update(case)  # Restore the logical round, not the child's always-one round.
        if collector_exit != 0:
            record["valid"] = False
            record["error"] = f"collector exit={collector_exit}; " + (
                source["error"] or "formal result passed; inspect console for collection/archive failure")
    except (OSError, ValueError, KeyError) as error:
        record["error"] = f"collector exit={collector_exit}; {error}"
    return record if record["valid"] else dict(case, valid=False, error=record["error"])


def run_case(case, stage, root, port, bash):
    folder = root / stage
    console = folder / f"{case['case']}-console.log"
    env = os.environ.copy()
    env.update(UB_OUTPUT_PARENT=folder.as_posix(), UB_PORT=str(port), SEND_ROUNDS="1",
               SEND_SIZES=str(case["bytes"]), SEND_OPS=case["op"], SEND_PROFILE="baseline")
    print(f"BATCH stage={stage} case={case['case']} control_port={port}", flush=True)
    run = None
    with console.open("w", encoding="utf-8") as stream:
        child = subprocess.Popen([bash, str(HERE / "ub_loopback_perftest.sh")], env=env,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, encoding="utf-8", errors="replace")
        try:
            for line in child.stdout:
                stream.write(line)
                stream.flush()
                print(line, end="", flush=True)
                if line.startswith("RUN="):
                    run = Path(line[4:].strip())
            code = child.wait()
        finally:
            child.stdout.close()
            if child.poll() is None:
                # The collector owns its children and its EXIT trap performs cleanup.
                child.terminate()
                try:
                    child.wait(timeout=135)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
    record = read_case(run, case, code) if run else dict(
        case, valid=False, error=f"collector exit={code}; no RUN directory reported")
    attempt = dict(case, stage=stage, control_port=port, collector_exit=code,
                   run=str(run) if run else "", valid=record["valid"], error=record["error"])
    return record, attempt


def write_stage(root, stage, settings, records, planned, finished):
    folder = root / stage
    folder.mkdir(exist_ok=True)
    if finished:
        failed = any(not row["valid"] for row in records) or len(records) != planned
        (folder / ("FAILED.txt" if failed else "COMPLETE")).write_text(
            f"planned={planned} attempted={len(records)}\n", encoding="utf-8")
    text = report.render(folder, dict(settings, stage=stage), records, planned).replace(
        "MEASURED valid=", "CASES valid=")
    print(text, end="", flush=True)
    (folder / "summary.txt").write_text(text, encoding="utf-8")
    with (folder / "metrics.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=report.FIELDS)
        writer.writeheader()
        writer.writerows(records)


def execute(root, cases, settings, start_port, bash, retry=True):
    records = {"initial": [], "retry": []}
    retry_cases = []
    finished = False
    port = start_port
    with (root / "attempts.tsv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=ATTEMPT_FIELDS, delimiter="\t")
        writer.writeheader()
        stream.flush()
        try:
            for stage in ("initial", "retry"):
                (root / stage).mkdir(exist_ok=True)
                selected = cases if stage == "initial" else retry_cases if retry else []
                for case in selected:
                    record, attempt = run_case(case, stage, root, port, bash)
                    port += 2  # Child reserves one warmup port and one formal port, even on failure.
                    records[stage].append(record)
                    writer.writerow(attempt)
                    stream.flush()
                    if stage == "initial" and not record["valid"]:
                        retry_cases.append(case)
            finished = True
        finally:
            with (root / "failed-initial.tsv").open("w", encoding="utf-8", newline="") as failed:
                fields = ["case", "op", "bytes", "round"]
                output = csv.DictWriter(failed, fieldnames=fields, delimiter="\t")
                output.writeheader()
                output.writerows(retry_cases)
            write_stage(root, "initial", settings, records["initial"], len(cases), finished)
            write_stage(root, "retry", settings, records["retry"], len(retry_cases) if retry else 0, finished)
            if finished:
                (root / "FINISHED").write_text("All scheduled stages finished; failures remain recorded.\n",
                                                encoding="utf-8")
    return 1 if retry_cases else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sizes", nargs="+", type=int, default=[2, 64, 256, 1024, 4096])
    parser.add_argument("--ops", nargs="+", choices=["send_lat", "send_bw"], default=["send_lat", "send_bw"])
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--port", type=int, default=22000)
    parser.add_argument("--no-retry", action="store_true", help="only run the initial stage")
    parser.add_argument("--bash", default="bash", help="Bash executable used to launch the collector")
    args = parser.parse_args(argv)
    if not 1 <= args.rounds <= 20 or any(not 1 <= size <= 4096 for size in args.sizes):
        parser.error("rounds must be 1..20 and CTP sizes must be 1..4096")
    if len(set(args.sizes)) != len(args.sizes) or len(set(args.ops)) != len(args.ops):
        parser.error("duplicate sizes or operations")
    cases = plan_cases(args.sizes, args.ops, args.rounds)
    reserved = 2 * len(cases) * (1 if args.no_retry else 2)
    if not 1024 <= args.port or args.port + reserved - 1 > 65000:
        parser.error("control port range does not fit all initial and possible retry pairs")
    if not shutil.which(args.bash):
        parser.error("Bash executable not found")
    root = Path(tempfile.mkdtemp(prefix="ub-batch-", dir=os.environ.get("UB_OUTPUT_PARENT", str(Path.home())))).resolve()
    print(f"BATCH_ROOT={root}", flush=True)
    settings = dict(backend="UB", variant="RM/CTP", profile="baseline", topology="loopback",
                    completion=os.environ.get("SEND_MODE", "poll"),
                    channels_bw=os.environ.get("SEND_CHANNELS", "1"),
                    tx_depth_bw=os.environ.get("SEND_TX_DEPTH", "128"),
                    rx_depth=os.environ.get("SEND_RX_DEPTH", "512"),
                    cq_mod_bw=os.environ.get("SEND_CQ_MOD", "100"), inline="0", rounds=str(args.rounds))
    for source in (Path(__file__).resolve(), HERE / "ub_loopback_perftest.sh", HERE / "send_perftest_report.py"):
        shutil.copy2(source, root / source.name)
    with (root / "tool.sha256").open("w", encoding="utf-8") as stream:
        for source in sorted(root.iterdir()):
            if source.is_file() and source.suffix in (".py", ".sh"):
                stream.write(f"{hashlib.sha256(source.read_bytes()).hexdigest()}  {source.name}\n")
    with (root / "plan.tsv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=["case", "op", "bytes", "round"], delimiter="\t")
        writer.writeheader()
        writer.writerows(cases)
    try:
        return execute(root, cases, settings, args.port, args.bash, not args.no_retry)
    finally:
        archive = shutil.make_archive(str(root), "gztar", root_dir=root.parent, base_dir=root.name)
        print(f"BATCH_ARCHIVE={archive}\nBATCH_ROOT={root}", flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
