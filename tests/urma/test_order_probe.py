# Copyright (c) Valkey Contributors
# SPDX-License-Identifier: BSD-3-Clause
"""Hardware-free subprocess tests; binary is linked only to the SDK double."""
import os
from pathlib import Path
import subprocess
import sys
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


class OrderProbeTests(unittest.TestCase):
    def run_probe(self, scenario="", args=None):
        env = dict(os.environ, ORDER_PROBE_MOCK_FAILURE=scenario)
        return subprocess.run([BINARY] + (args if args is not None else ["mock", "0"]),
                              env=env, capture_output=True, text=True, timeout=5)

    def test_both_order_modes_can_be_diagnosed_with_zero_reported_capability(self):
        result = self.run_probe()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("rc_order=0", result.stdout)
        self.assertIn("order=default requested=0 resources=created", result.stdout)
        self.assertIn("order=OT requested=1 resources=created", result.stdout)
        self.assertIn("mock.uninit", result.stdout)

    def test_ot_receive_rejection_does_not_hide_default_success(self):
        result = self.run_probe("ot-jfr")
        self.assertEqual(result.returncode, 1)
        self.assertIn("order=default requested=0 resources=created", result.stdout)
        self.assertIn("order=OT requested=1 failed_stage=create_jfr", result.stdout)
        self.assertIn("mock.uninit", result.stdout)

    def test_ot_jetty_rejection_cleans_its_receive_queue(self):
        result = self.run_probe("ot-jetty")
        self.assertEqual(result.returncode, 1)
        self.assertIn("order=OT requested=1 failed_stage=create_jetty", result.stdout)
        self.assertIn("order=OT cleanup=urma_delete_jfr status=0", result.stdout)
        self.assertIn("mock.uninit", result.stdout)

    def test_early_failure_releases_event_and_context(self):
        result = self.run_probe("jfc")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout.count("failed_stage=create_jfc"), 2)
        self.assertIn("mock.delete_context", result.stdout)
        self.assertIn("mock.uninit", result.stdout)

    def test_deletion_failure_does_not_delete_live_dependencies(self):
        result = self.run_probe("delete-jetty")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Cleanup incomplete", result.stderr)
        self.assertNotIn("cleanup=urma_delete_jfr", result.stdout)
        self.assertNotIn("mock.delete_context", result.stdout)
        self.assertNotIn("mock.uninit", result.stdout)

    def test_invalid_arguments_do_not_initialize_sdk(self):
        for args in ([], ["mock", "-1"], ["mock", "4294967296"], ["mock", "bad"]):
            with self.subTest(args=args):
                result = self.run_probe(args=args)
                self.assertEqual(result.returncode, 2)
                self.assertNotIn("MOCK SDK", result.stdout)

    def test_non_native_device_is_rejected(self):
        result = self.run_probe("not-ub")
        self.assertEqual(result.returncode, 1)
        self.assertIn("not native UB", result.stderr)
        self.assertIn("mock.uninit", result.stdout)

    def test_context_failure_is_reported_and_sdk_is_closed(self):
        result = self.run_probe("context")
        self.assertEqual(result.returncode, 1)
        self.assertIn("urma_create_context failed", result.stderr)
        self.assertIn("mock.uninit", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
