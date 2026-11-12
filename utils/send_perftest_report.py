#!/usr/bin/env python3
"""Print header-checked RDMA/URMA SEND reports; failed pairs never supply metrics."""
import argparse
import csv
import math
from pathlib import Path
import re
import statistics

CASE = re.compile(r"(warmup|measured)-send_(lat|bw)-(\d+)-r(\d+)")
ROW = re.compile(r"^\s*\d+\s+\d+\s+[0-9]")
FIELDS = ["case", "op", "bytes", "round", "valid", "error", "median_us", "avg_us",
          "p99_us", "p999_us", "bandwidth_mib_s", "mpps", "iterations", "effective_seconds", "source_unit"]


def parse_report(text, op, size):
    """Recognize the tool's printed header, not a generic numeric-looking line."""
    header = None
    rows = []
    for line in text.splitlines():
        if "iterations" in line and ("t_min[" in line or "BW average[" in line):
            header = line
        elif header and ROW.match(line):
            tokens = line.split()
            if int(tokens[0]) == size:
                rows.append((header, tokens))
    if len(rows) != 1:
        raise ValueError("expected one result row with a recognized header")
    header, tokens = rows[0]
    values = [float(token) for token in tokens]
    if any(not math.isfinite(value) or value < 0 for value in values):
        raise ValueError("non-finite or negative metric")
    result = {"iterations": int(tokens[1])}
    if result["iterations"] <= 0:
        raise ValueError("no completed iterations")
    if op == "send_lat":
        columns = header.split()
        if len(tokens) != len(columns):
            raise ValueError("latency header/result column count mismatch")
        if not all("[us]" in col or "[usec]" in col for col in columns[2:]):
            raise ValueError("unsupported latency unit")
        aliases = {"t_median": "median_us", "t_typical": "median_us", "t_avg": "avg_us",
                   "99%": "p99_us", "99.9%": "p999_us"}
        for index, column in enumerate(columns):
            key = aliases.get(column.split("[")[0])
            if key:
                result[key] = values[index]
        if "median_us" not in result or "avg_us" not in result:
            raise ValueError("missing latency median/average")
    else:
        unit = re.search(r"BW average\[([^\]]+)\]", header)
        if not unit or "MsgRate[Mpps]" not in header or len(values) < 5:
            raise ValueError("unsupported bandwidth header")
        average, mpps = values[3:5]
        if average <= 0 or mpps <= 0:
            raise ValueError("zero bandwidth/message rate")
        source_unit = unit.group(1)
        factors = {"MiB/sec": [2**20], "MB/sec": [10**6, 2**20],
                   "GiB/sec": [2**30], "GB/sec": [10**9, 2**30],
                   "Gb/sec": [10**9 / 8], "Gbit/sec": [10**9 / 8]}
        if source_unit not in factors:
            raise ValueError("unsupported bandwidth unit: " + source_unit)
        payload_rate = mpps * 10**6 * size
        factor = min(factors[source_unit], key=lambda candidate: abs(average * candidate - payload_rate))
        if abs(average * factor - payload_rate) > max(payload_rate * .02, factor * .01):
            raise ValueError("bandwidth/message-rate units inconsistent")
        result.update(bandwidth_mib_s=average * factor / 2**20, mpps=mpps,
                      source_unit=f"{source_unit} (bytes/unit={factor:g})")
    return result


def read_tsv(path):
    with path.open(encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def collect(folder):
    folder = Path(folder)
    settings_file = folder / "settings.tsv"
    settings = {}
    if settings_file.exists():
        with settings_file.open(encoding="utf-8", newline="") as stream:
            settings = dict(csv.reader(stream, delimiter="\t"))
    if not settings:
        settings = {"backend": "UB" if (folder / "send_lat-help.log").exists() else "RDMA",
                    "profile": "legacy", "topology": "not recorded", "completion": "not recorded"}
    results_file = folder / "results.tsv"
    manifest = read_tsv(results_file) if results_file.exists() else []
    plan = read_tsv(folder / "plan.tsv") if (folder / "plan.tsv").exists() else []
    measured = [row for row in manifest if row.get("phase") == "measured"]
    records = []
    seen = set()
    for row in measured:
        tag = row["case"]
        match = CASE.fullmatch(tag)
        if not match or tag in seen:
            raise ValueError("invalid or duplicate case identifier")
        seen.add(tag)
        size, round_number = int(match[3]), int(match[4])
        op = "send_" + match[2]
        if row.get("op") != op or int(row["bytes"]) != size or int(row["round"]) != round_number:
            raise ValueError("case/manifest mismatch")
        record = dict(case=tag, op=op, bytes=size, round=round_number, valid=False, error="")
        try:
            for role, key in (("server", "server_exit"), ("client", "client_exit")):
                code = int(row[key])
                if code != 0:
                    raise ValueError(f"{role} exit={code}")
                actual = int((folder / f"{tag}-{role}.exit").read_text().strip())
                if actual != code:
                    raise ValueError(f"{role} exit file differs from manifest")
            record.update(parse_report((folder / f"{tag}-client.log").read_text(errors="replace"), op, size))
            if op == "send_bw" and settings.get("channels_bw") == "1":
                record["effective_seconds"] = record["iterations"] / (record["mpps"] * 10**6)
            record["valid"] = True
        except (ValueError, OSError) as error:
            record["error"] = str(error)
        records.append(record)
    planned = sum(row.get("phase") == "measured" for row in plan) if plan else None
    return settings, records, planned


def metric_range(records, key):
    values = [row[key] for row in records if key in row]
    if not values:
        return "-"
    return f"{statistics.median(values):.3f}[{min(values):.3f},{max(values):.3f}]"


def render(folder, settings, records, planned):
    valid = [row for row in records if row["valid"]]
    failed = [row for row in records if not row["valid"]]
    pending = "unknown" if planned is None else str(max(0, planned - len(records)))
    text = [f"\n=== {settings.get('backend', '?')} SEND: {folder} ===",
            " ".join(f"{key}={value}" for key, value in settings.items()),
            f"MEASURED valid={len(valid)} invalid={len(failed)} attempted={len(records)} "
            f"planned={planned if planned is not None else 'unknown'} unattempted={pending}",
            "Values: median[min,max] across successful rounds; percentile medians are not pooled percentiles.",
            "Warmup excluded. A failed pair never supplies performance metrics.",
            "LAT bytes channels valid/attempted median_us avg_us p99_us p99.9_us"]
    for op in ("send_lat", "send_bw"):
        if op == "send_bw":
            text.append("BW  bytes channels valid/attempted MiB/s Mpps inferred_seconds(single channel only)")
        sizes = sorted({row["bytes"] for row in records if row["op"] == op})
        for size in sizes:
            attempted = [row for row in records if row["op"] == op and row["bytes"] == size]
            group = [row for row in attempted if row["valid"]]
            channels = "1" if op == "send_lat" else settings.get("channels_bw", "unknown")
            keys = ("median_us", "avg_us", "p99_us", "p999_us") if op == "send_lat" else (
                "bandwidth_mib_s", "mpps", "effective_seconds")
            text.append(f"{size:7} {channels:>8} {len(group):2}/{len(attempted):<2} " +
                        " ".join(metric_range(group, key) for key in keys))
    for row in failed:
        text.append(f"INVALID {row['case']}: {row['error']}")
    if (Path(folder) / "FAILED.txt").exists():
        text.append("RUN STATUS: FAILED/PARTIAL; inspect original logs.")
    elif (Path(folder) / "COMPLETE").exists():
        text.append("RUN STATUS: collector completed; check validity counts above.")
    else:
        text.append("RUN STATUS: in progress or incomplete.")
    text.append("No automatic speedup ratio: confirm topology, transport, profile and actual tool configuration first.")
    return "\n".join(text) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folders", type=Path, nargs="+", help="extracted RDMA and/or UB run directories")
    parser.add_argument("--write", action="store_true", help="also write metrics.csv and summary.txt inside each run")
    args = parser.parse_args(argv)
    for folder in args.folders:
        settings, records, planned = collect(folder)
        report = render(folder, settings, records, planned)
        print(report, end="")
        if args.write:
            with (folder / "metrics.csv").open("w", encoding="utf-8", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=FIELDS)
                writer.writeheader()
                writer.writerows(records)
            (folder / "summary.txt").write_text(report, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
