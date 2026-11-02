#!/usr/bin/env python3
# Copyright (c) Valkey Contributors
# All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
"""Non-root regression tests; only private temporary fixtures are written."""

import sys

sys.dont_write_bytecode = True

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[2] / "utils/rdma_preflight.py"
SPEC = importlib.util.spec_from_file_location("preflight", SCRIPT)
preflight = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(preflight)


class ReadOnlyPreflightTests(unittest.TestCase):
    def test_mutation_and_application_commands_are_rejected(self):
        commands = [
            ["modprobe", "rdma_rxe"], ["sudo", "rdma", "link", "show"],
            ["rdma", "link", "add", "rxe0", "type", "rxe", "netdev", "eth0"],
            ["rdma", "link", "del", "rxe0"], ["ip", "link", "set", "eth0", "down"],
            ["ldconfig"], ["make"], ["chmod", "666", "/dev/infiniband/rdma_cm"],
            ["valkey-server", "--version"], ["rdma-test", "--help"],
            ["python3", "-c", "import netifaces"],
        ]
        with mock.patch.object(preflight.subprocess, "run") as run:
            for argv in commands:
                with self.subTest(argv=argv):
                    self.assertFalse(preflight.allowed_command(argv))
                    self.assertIsNone(preflight.command(argv)["exit_code"])
            run.assert_not_called()

    def test_only_exact_query_forms_are_allowed(self):
        for argv in (["rdma", "link", "show"], ["ldconfig", "-p"],
                     ["ip", "-j", "address", "show"], ["cc", "--version"],
                     ["git", "-C", "/repo", "rev-parse", "HEAD"],
                     ["pkg-config", "--modversion", "libibverbs"],
                     ["ibv_devinfo", "-d", "mlx5_0", "-i", "1", "-v"]):
            with self.subTest(argv=argv):
                self.assertTrue(preflight.allowed_command(argv))
        self.assertFalse(preflight.allowed_command(["rdma", "link", "show", ";", "reboot"]))
        self.assertFalse(preflight.allowed_command(["ibv_devinfo", "-d", "../bad", "-i", "1", "-v"]))

    def test_permission_denied_is_distinct_from_missing(self):
        with mock.patch.object(Path, "stat", side_effect=PermissionError("denied")):
            self.assertIn("unavailable", preflight.device_access("/test"))
        with mock.patch.object(Path, "stat", side_effect=FileNotFoundError()):
            self.assertEqual("missing", preflight.device_access("/test"))
        with mock.patch.object(Path, "stat"), mock.patch.object(preflight.os, "access", return_value=False):
            self.assertEqual("permission_denied", preflight.device_access("/test"))
        with mock.patch.object(Path, "iterdir", side_effect=PermissionError("denied")):
            entries, error = preflight.directory_entries("/test")
            self.assertEqual([], entries)
            self.assertIn("PermissionError", error)

    def test_accessible_other_device_is_not_selected(self):
        nodes = [Path("/sys/class/infiniband_verbs/uverbs0"),
                 Path("/sys/class/infiniband_verbs/uverbs1")]
        with mock.patch.object(preflight, "directory_entries", return_value=(nodes, None)), \
                mock.patch.object(preflight, "read_text", side_effect=["mlx5_0", "mlx5_1"]):
            selected, error = preflight.selected_uverbs("mlx5_1")
        self.assertEqual([Path("/dev/infiniband/uverbs1")], selected)
        self.assertIsNone(error)

    def test_passive_main_never_executes_inspected_programs_or_verbs_query(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for filename in ("src/rdma.c", "tests/rdma/rdma-test.c",
                             "src/valkey-server", "tests/rdma/rdma-test"):
                path = root / filename
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture\n")
                path.chmod(0o700)
            before = sorted(str(path) for path in root.rglob("*"))
            args = ["rdma_preflight.py", "--repo", str(root), "--stage", "run"]
            queried = []

            def query(argv, timeout=8):
                queried.append(argv)
                return {"argv": argv, "exit_code": 0, "stdout": "", "stderr": ""}

            with mock.patch.object(sys, "argv", args), \
                    mock.patch.object(preflight, "command", side_effect=query), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertIn(preflight.main(), (0, 2))
            self.assertEqual(before, sorted(str(path) for path in root.rglob("*")))
            self.assertTrue(all(preflight.allowed_command(argv) for argv in queried))
            self.assertTrue(all(Path(argv[0]).name not in
                                ("valkey-server", "rdma-test", "ibv_devinfo", "python3")
                                for argv in queried))

    def test_report_is_exclusive_and_private(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "report.json"
            args = ["rdma_preflight.py", "--repo", directory, "--stage", "build", "--output", str(output)]
            with mock.patch.object(sys, "argv", args), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(2, preflight.main())
                original = output.read_bytes()
                self.assertEqual("environment_blocked", json.loads(original)["status"])
                self.assertEqual(0, output.stat().st_mode & 0o077)
                with self.assertRaises(FileExistsError):
                    preflight.main()
                self.assertEqual(original, output.read_bytes())


if __name__ == "__main__":
    unittest.main()
