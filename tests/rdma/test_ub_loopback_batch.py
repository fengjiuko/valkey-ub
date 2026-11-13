#!/usr/bin/env python3
"""Verify failure continuation and separate, bounded UB retests without hardware."""
import csv
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

UTILS = Path(__file__).resolve().parents[2] / "utils"
sys.path.insert(0, str(UTILS))
SPEC = importlib.util.spec_from_file_location("ub_loopback_batch", UTILS / "ub_loopback_batch.py")
batch = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(batch)


class BatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ub-batch-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.settings = dict(backend="UB", channels_bw="1")

    def read(self, name, delimiter="\t"):
        with (self.root / name).open(encoding="utf-8", newline="") as stream:
            return list(csv.DictReader(stream, delimiter=delimiter))

    def test_all_initial_cases_run_before_failed_cases_retested_once(self):
        cases = batch.plan_cases([2, 64], ["send_lat", "send_bw"], 2)
        failures = {cases[0]["case"], cases[-1]["case"]}
        calls = []

        def fake(case, stage, root, port, bash):
            calls.append((stage, case["case"], port))
            valid = case["case"] not in failures or (stage == "retry" and case == cases[0])
            record = dict(case, valid=valid, error="" if valid else "injected error")
            if valid:
                record.update(median_us=1.5) if case["op"] == "send_lat" else record.update(
                    bandwidth_mib_s=100, mpps=1)
            attempt = dict(case, stage=stage, control_port=port, collector_exit=0 if valid else 1,
                           run=f"{root}/{stage}/{case['case']}", valid=valid, error=record["error"])
            return record, attempt

        with patch.object(batch, "run_case", side_effect=fake), redirect_stdout(io.StringIO()):
            code = batch.execute(self.root, cases, self.settings, 22000, "bash")
        self.assertEqual(code, 1)  # Recovery never erases the first-stage failure status.
        self.assertEqual([stage for stage, _, _ in calls], ["initial"] * 8 + ["retry"] * 2)
        self.assertEqual([tag for _, tag, _ in calls[8:]], [cases[0]["case"], cases[-1]["case"]])
        self.assertEqual([port for _, _, port in calls], list(range(22000, 22020, 2)))
        self.assertEqual(len(self.read("attempts.tsv")), 10)
        self.assertEqual(len(self.read("failed-initial.tsv")), 2)
        initial = self.read("initial/metrics.csv", ",")
        retry = self.read("retry/metrics.csv", ",")
        self.assertEqual(sum(row["valid"] == "False" for row in initial), 2)
        self.assertEqual([row["valid"] for row in retry], ["True", "False"])
        self.assertEqual(retry[-1]["round"], "2")
        self.assertTrue((self.root / "FINISHED").exists())

    def test_no_retry_and_all_failed_initial_cases_are_still_attempted(self):
        cases = batch.plan_cases([2, 64], ["send_bw"], 1)

        def fake(case, stage, root, port, bash):
            record = dict(case, valid=False, error="injected error")
            return record, dict(case, stage=stage, control_port=port, collector_exit=1,
                                run="", valid=False, error=record["error"])

        with patch.object(batch, "run_case", side_effect=fake) as runner, redirect_stdout(io.StringIO()):
            self.assertEqual(batch.execute(self.root, cases, self.settings, 22000, "bash", False), 1)
        self.assertEqual(runner.call_count, 2)
        self.assertEqual(len(self.read("retry/metrics.csv", ",")), 0)

    def test_all_pass_needs_no_retest(self):
        cases = batch.plan_cases([64], ["send_lat"], 1)

        def fake(case, stage, root, port, bash):
            record = dict(case, valid=True, error="", median_us=1.5)
            return record, dict(case, stage=stage, control_port=port, collector_exit=0,
                                run="source", valid=True, error="")

        with patch.object(batch, "run_case", side_effect=fake) as runner, redirect_stdout(io.StringIO()):
            self.assertEqual(batch.execute(self.root, cases, self.settings, 22000, "bash"), 0)
        self.assertEqual(runner.call_count, 1)
        self.assertEqual(len(self.read("failed-initial.tsv")), 0)
        self.assertTrue((self.root / "initial/COMPLETE").exists())

    def test_collector_error_cannot_supply_metrics_even_if_formal_pair_passed(self):
        case = dict(case="measured-send_lat-64-r5", op="send_lat", bytes=64, round=5)
        source = dict(case="measured-send_lat-64-r1", op="send_lat", bytes=64, round=1,
                      valid=True, error="", median_us=1.5)
        with patch.object(batch.report, "collect", return_value=({}, [source], 1)):
            good = batch.read_case(self.root, case, 0)
            bad = batch.read_case(self.root, case, 1)
        self.assertEqual(good["round"], 5)
        self.assertTrue(good["valid"])
        self.assertFalse(bad["valid"])
        self.assertNotIn("median_us", bad)

    def test_warmup_failure_is_invalid_and_keeps_reason(self):
        case = dict(case="measured-send_bw-64-r1", op="send_bw", bytes=64, round=1)
        with patch.object(batch.report, "collect", return_value=({}, [], 1)):
            record = batch.read_case(self.root, case, 255)
        self.assertFalse(record["valid"])
        self.assertIn("warmup", record["error"])


if __name__ == "__main__":
    unittest.main()
