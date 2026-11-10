#!/usr/bin/env python3
"""Hardware-free tests for the standalone RDMA baseline collector."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "utils/rdma_baseline.py"
SPEC = importlib.util.spec_from_file_location("rdma_baseline", SCRIPT)
baseline = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(baseline)

REPORT = """====== GET ======
123456 requests completed in 30.00 seconds
Summary:
  throughput summary: 4115.20 requests per second
  latency summary (msec):
          avg       min       p50       p95       p99       max
        1.000     0.100     0.500     2.000     3.000     5.000
"""


class BaselineTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="rdma-baseline-test-")
        self.folder = Path(self.temp.name).resolve()
        self.assertEqual(self.folder.parent, Path(tempfile.gettempdir()).resolve())
        self.addCleanup(self.temp.cleanup)

    def test_report_maps_percentiles_and_throughput(self):
        result = baseline.parse_report(REPORT)
        self.assertEqual(result["requests"], 123456)
        self.assertEqual(result["qps"], 4115.2)
        self.assertEqual([result[key] for key in ("p50_ms", "p95_ms", "p99_ms")], [.5, 2., 3.])

    def test_incomplete_short_and_empty_reports_are_rejected(self):
        for text in ("PONG", REPORT.replace("30.00 seconds", "2.00 seconds"),
                     REPORT.replace("123456 requests", "0 requests"),
                     REPORT.replace("4115.20 requests per second", "0.00 requests per second")):
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                baseline.parse_report(text)

    def test_nonzero_exit_is_preserved_and_rejected(self):
        log = self.folder / "failed.log"
        with self.assertRaisesRegex(RuntimeError, "exit=7"):
            baseline.launch_logged([sys.executable, "-c", "import sys; sys.exit(7)"], log, 10)
        self.assertEqual(log.with_suffix(".exit").read_text().strip(), "7")

    def test_successful_command_preserves_output_and_exit(self):
        log = self.folder / "success.log"
        baseline.launch_logged([sys.executable, "-c", "print('finished')"], log, 10)
        self.assertEqual(log.read_text().strip(), "finished")
        self.assertEqual(log.with_suffix(".exit").read_text().strip(), "0")

    def test_timeout_terminates_the_launched_child(self):
        original_popen = subprocess.Popen
        children = []

        def track_child(*args, **kwargs):
            child = original_popen(*args, **kwargs)
            children.append(child)
            return child

        with mock.patch.object(baseline.subprocess, "Popen", side_effect=track_child):
            with self.assertRaises(subprocess.TimeoutExpired):
                baseline.launch_logged([sys.executable, "-c", "import time; time.sleep(30)"],
                                       self.folder / "timeout.log", .1)
        self.assertEqual(len(children), 1)
        self.assertIsNotNone(children[0].poll())

    def test_proc_stat_handles_spaces_and_parentheses_in_process_name(self):
        stat = "123 (odd ) name) " + " ".join(["R"] + [str(field) for field in range(4, 53)])

        def proc_contents(path, *unused, **kwargs):
            return "cpu 1 2 3 4 5 6 7 8\n" if path == Path("/proc/stat") else stat

        with mock.patch.object(Path, "read_text", autospec=True, side_effect=proc_contents), \
                mock.patch.object(baseline.os, "sysconf", create=True, return_value=4096):
            row = baseline.read_process(123)
        self.assertEqual((row["user_ticks"], row["system_ticks"], row["start_ticks"]), (14, 15, 22))
        self.assertEqual(row["rss_bytes"], 24 * 4096)
        self.assertEqual(row["host_cpu_ticks"], list(range(1, 9)))

    def test_source_sha_mismatch_is_rejected_before_host_queries(self):
        source = self.folder / "source"
        source.mkdir()
        (self.folder / "source-sha.txt").write_text("wrong-sha\n")
        with self.assertRaisesRegex(RuntimeError, "source-sha.txt"):
            baseline.metadata(self.folder, source, "client")

    def test_nonempty_instance_is_rejected_without_launching_benchmark(self):
        settings = {"port": "0", "rdma-port": "16381", "urma-port": "0", "io-threads": "1",
                    "appendonly": "no", "save": "", "maxmemory": "0", "cluster-enabled": "no"}

        def respond(command):
            if command[-1] == "PING":
                return "PONG"
            if command[-2:] == ["INFO", "server"]:
                return "run_id:existing-instance"
            if command[-3:-1] == ["CONFIG", "GET"]:
                name = command[-1]
                return name + "\n" + settings[name]
            if command[-1] == "DBSIZE":
                return "99"
            raise AssertionError("unexpected query: %r" % command)

        with mock.patch.object(baseline, "query", side_effect=respond), \
                mock.patch.object(baseline, "launch_logged", side_effect=AssertionError("benchmark must not run")) as launch:
            with self.assertRaisesRegex(RuntimeError, "EMPTY"):
                baseline.client(self.folder, self.folder)
            launch.assert_not_called()

    def test_help_needs_no_linux_or_rdma_device(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "--help"], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0)
        self.assertIn(b"--source", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
