#!/usr/bin/env python3
"""Check terminal metrics against real SEND report shapes and failed-pair evidence."""
import csv
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "utils/send_perftest_report.py"
SPEC = importlib.util.spec_from_file_location("send_perftest_report", SCRIPT)
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)

UB_LAT = """ bytes iterations t_min[us] t_max[us] t_median[us] t_avg[us] t_stdev[us] 99%[us] 99.9%[us] 99.99%[us] 99.999%[us]
256 100000 1.38 12.08 1.44 1.47 0.15 1.70 2.61 7.30 12.08
"""
RDMA_LAT = """ #bytes #iterations t_min[usec] t_max[usec] t_typical[usec] t_avg[usec] t_stdev[usec] 99%[usec] 99.9%[usec]
256 100000 1.00 12.00 1.50 1.60 0.20 2.00 3.00
"""
UB_BW = """ bytes iterations BW peak[MB/sec] BW average[MB/sec] MsgRate[Mpps]
4096 64882500 0.00 16896.53 4.325511
"""


class SendReportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="send-report-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def fixture(self, name, backend, rows):
        folder = self.root / name
        folder.mkdir()
        (folder / "settings.tsv").write_text(
            f"backend\t{backend}\nchannels_bw\t1\nprofile\tbaseline\n", encoding="utf-8")
        manifest = []
        for phase, op, size, round_number, code, text in rows:
            tag = f"{phase}-{op}-{size}-r{round_number}"
            manifest.append([tag, phase, op, size, round_number, code, code])
            for role in ("server", "client"):
                (folder / f"{tag}-{role}.exit").write_text(str(code), encoding="utf-8")
            (folder / f"{tag}-client.log").write_text(text, encoding="utf-8")
        with (folder / "results.tsv").open("w", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream, delimiter="\t")
            writer.writerow(["case", "phase", "op", "bytes", "round", "server_exit", "client_exit"])
            writer.writerows(manifest)
        with (folder / "plan.tsv").open("w", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream, delimiter="\t")
            writer.writerow(["case", "phase", "op", "bytes", "round"])
            writer.writerows(row[:5] for row in manifest if row[1] == "measured")
        return folder

    def test_maps_both_latency_headers(self):
        ub = report.parse_report(UB_LAT, "send_lat", 256)
        rdma = report.parse_report(RDMA_LAT, "send_lat", 256)
        self.assertEqual([ub[k] for k in ("median_us", "avg_us", "p99_us", "p999_us")],
                         [1.44, 1.47, 1.70, 2.61])
        self.assertEqual(rdma["median_us"], 1.50)

    def test_legacy_binary_mb_and_decimal_gbit_normalize(self):
        binary = report.parse_report(UB_BW, "send_bw", 4096)
        decimal = report.parse_report(UB_BW.replace("MB/sec", "Gb/sec").replace("16896.53", "141.738367"),
                                      "send_bw", 4096)
        self.assertAlmostEqual(binary["bandwidth_mib_s"], 16896.53, places=2)
        self.assertAlmostEqual(decimal["bandwidth_mib_s"], 16896.53, places=2)

    def test_headerless_mismatched_units_and_truncated_rows_rejected(self):
        for text in ("256 100000 1.44", UB_LAT.replace("[us]", "[ms]"),
                     UB_LAT.replace(" 12.08\n", "\n")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                report.parse_report(text, "send_lat", 256)
        with self.assertRaises(ValueError):
            report.parse_report(UB_BW.replace("16896.53", "1000.00"), "send_bw", 4096)

    def test_failed_pair_with_result_is_excluded_and_warmup_is_ignored(self):
        folder = self.fixture("partial", "UB", [
            ("warmup", "send_bw", 4096, 0, 0, UB_BW),
            ("measured", "send_bw", 4096, 1, 0, UB_BW),
            ("measured", "send_bw", 4096, 2, 255, UB_BW)])
        (folder / "FAILED.txt").write_text("runner_exit=1", encoding="utf-8")
        settings, records, planned = report.collect(folder)
        self.assertEqual([row["valid"] for row in records], [True, False])
        self.assertNotIn("bandwidth_mib_s", records[1])
        self.assertAlmostEqual(records[0]["effective_seconds"], 15, places=3)
        rendered = report.render(folder, settings, records, planned)
        self.assertIn("valid=1 invalid=1 attempted=2", rendered)
        self.assertIn("FAILED/PARTIAL", rendered)

    def test_exit_file_disagreement_and_missing_log_rejected(self):
        folder = self.fixture("missing", "RDMA", [
            ("measured", "send_lat", 256, 1, 0, RDMA_LAT),
            ("measured", "send_lat", 256, 2, 0, RDMA_LAT)])
        (folder / "measured-send_lat-256-r1-client.exit").write_text("7", encoding="utf-8")
        (folder / "measured-send_lat-256-r2-client.log").unlink()
        _, records, _ = report.collect(folder)
        self.assertTrue(all(not row["valid"] for row in records))

    def test_cli_prints_both_backends_and_writes_metrics(self):
        rdma = self.fixture("rdma", "RDMA", [("measured", "send_lat", 256, 1, 0, RDMA_LAT)])
        ub = self.fixture("ub", "UB", [("measured", "send_bw", 4096, 1, 0, UB_BW)])
        result = subprocess.run([sys.executable, str(SCRIPT), "--write", str(rdma), str(ub)],
                                capture_output=True, text=True, check=True)
        self.assertIn("=== RDMA SEND:", result.stdout)
        self.assertIn("=== UB SEND:", result.stdout)
        for folder in (rdma, ub):
            self.assertTrue((folder / "metrics.csv").is_file())
            self.assertTrue((folder / "summary.txt").is_file())


if __name__ == "__main__":
    unittest.main()
