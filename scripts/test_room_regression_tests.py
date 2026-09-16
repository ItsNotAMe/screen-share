"""Failure-path tests for the room regression runner; no media or physical devices."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
import json

spec = importlib.util.spec_from_file_location("room_regression", Path(__file__).with_name("test-room-regression.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.log = self.root / "run.log"

    def tearDown(self):
        self.directory.cleanup()

    def run_code(self, code, **options):
        return runner.run_process([sys.executable, "-c", code], self.log, dict(os.environ), **options)

    def test_success_and_failure_exit_codes(self):
        self.assertTrue(self.run_code("print('complete')")["passed"])
        result = self.run_code("import sys; print('failure'); sys.exit(7)")
        self.assertEqual(result["exitCode"], 7)
        self.assertFalse(result["passed"])
        self.assertIn("failure", self.log.read_text())

    def test_runaway_output_is_bounded_and_fails(self):
        result = self.run_code("import os;\nwhile True: os.write(1, b'x' * 65536)", max_log_bytes=4096)
        self.assertTrue(result["logLimitExceeded"])
        self.assertFalse(result["passed"])
        self.assertLessEqual(self.log.stat().st_size, 4096)

    def descendant(self, parent_wait):
        heartbeat = self.root / "heartbeat"
        child = "import os,time; f=open(" + repr(str(heartbeat)) + ", 'wb', buffering=0);\nwhile True: f.write(b'x'); time.sleep(.01)"
        code = ("import subprocess,sys,time; from pathlib import Path; "
                "subprocess.Popen([sys.executable, '-c', " + repr(child) + "]); "
                "p=Path(" + repr(str(heartbeat)) + "); "
                "\nwhile not p.exists(): time.sleep(.01)\n" + parent_wait)
        return heartbeat, code

    def test_timeout_kills_descendants(self):
        heartbeat, code = self.descendant("time.sleep(30)")
        result = self.run_code(code, timeout=1)
        self.assertTrue(result["timedOut"])
        self.assertFalse(result["passed"])
        self.assertTrue(heartbeat.exists())
        before = heartbeat.stat().st_size
        time.sleep(.2)
        self.assertEqual(heartbeat.stat().st_size, before)

    def test_success_also_cleans_orphan_descendants(self):
        heartbeat, code = self.descendant("time.sleep(.1)")
        result = self.run_code(code)
        self.assertTrue(result["passed"])
        before = heartbeat.stat().st_size
        time.sleep(.2)
        self.assertEqual(heartbeat.stat().st_size, before)

    def test_default_matrix_never_selects_device_audio_or_desktop(self):
        self.assertTrue(all(mode != "windows-media" for _, _, mode, _ in runner.cases(False)))
        self.assertFalse(any("AudioDevice" in executable for _, executable, _, _ in runner.cases(True)))

    def test_failure_stops_round_and_preserves_machine_readable_evidence(self):
        fixture = self.root / "signaling-worker/tests/run-native-service.mjs"
        fixture.parent.mkdir(parents=True)
        fixture.write_text("fixture")
        dependency = self.root / "signaling-worker/node_modules/miniflare/package.json"
        dependency.parent.mkdir(parents=True)
        dependency.write_text("{}")
        output = self.root / "evidence"
        actual_run = runner.run_process
        def fail_process(command, log, environment):
            return actual_run([sys.executable, "-c", "import sys; print('expected failure'); sys.exit(7)"], log, environment)
        selected = [("first", sys.executable, None, None), ("must-not-run", sys.executable, None, None)]
        with mock.patch.object(runner, "ROOT", self.root), mock.patch.object(runner, "cases", return_value=selected), \
             mock.patch.object(runner.shutil, "which", return_value=sys.executable), \
             mock.patch.object(runner, "run_process", side_effect=fail_process) as execute:
            self.assertEqual(runner.main([str(self.root), str(output), "--repeat", "2"]), 1)
            self.assertEqual(execute.call_count, 1)
        report = json.loads((output / "result.json").read_text())
        self.assertFalse(report["passed"])
        self.assertEqual(report["plannedRuns"], 4)
        self.assertEqual(len(report["runs"]), 1)
        self.assertEqual(report["runs"][0]["exitCode"], 7)
        self.assertIn("expected failure", (output / report["runs"][0]["log"]).read_text())


if __name__ == "__main__":
    unittest.main()
