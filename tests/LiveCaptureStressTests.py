import importlib.util
import json
from pathlib import Path
import unittest
import sys
import tempfile
import time
import contextlib
import io
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("stress", Path(__file__).parents[1] / "scripts/stress-live-capture.py")
stress = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stress)
matrix_spec = importlib.util.spec_from_file_location("capture_matrix", Path(__file__).parents[1] / "scripts/test-capture-lifecycle.py")
matrix = importlib.util.module_from_spec(matrix_spec)
matrix_spec.loader.exec_module(matrix)


class StressEvidenceTests(unittest.TestCase):
    def log(self, cycles):
        lines = ['LIFECYCLE ' + json.dumps({'cycle': 0})]
        for cycle in cycles:
            lines += [f'Capture cycle {cycle} destroyed', 'LIFECYCLE ' + json.dumps({'cycle': cycle})]
        return '\n'.join(lines)

    def test_complete(self):
        self.assertTrue(stress.summarize(self.log([1, 2]), 2, 0, False)['passed'])

    def test_crash_preserves_completed_cycles(self):
        result = stress.summarize(self.log([1]), 100, -1073741819, False)
        self.assertFalse(result['passed'])
        self.assertEqual(result['exitCodeHex'], '0xC0000005')
        self.assertEqual(result['completedCycles'], [1])

    def test_missing_or_duplicate_completion_cannot_pass(self):
        for cycles in ([1], [1, 1], [2, 1]):
            self.assertFalse(stress.summarize(self.log(cycles), 2, 0, False)['passed'])

    def test_timeout_cannot_pass(self):
        self.assertFalse(stress.summarize(self.log([1, 2]), 2, 0, True)['passed'])

    def test_truncated_crash_sample_keeps_evidence(self):
        for tail in ('{', 'null', '[]', '{"cycle":true}'):
            result = stress.summarize(self.log([1]) + '\nLIFECYCLE ' + tail, 2, 1, False)
            self.assertFalse(result['passed'])
            self.assertEqual(result['malformedSamples'], 1)
            self.assertEqual(result['completedCycles'], [1])

    def resource_log(self, growth):
        lines = ['LIFECYCLE ' + json.dumps({'cycle': 0, 'handles': 100, 'privateBytes': 1000})]
        for cycle in range(1, 101):
            lines += [f'Capture cycle {cycle} destroyed', 'LIFECYCLE ' + json.dumps({'cycle': cycle, 'handles': 350 + cycle * growth, 'privateBytes': 1000 + cycle * 100})]
        return '\n'.join(lines)

    def test_handle_growth_fails_even_with_complete_cycles(self):
        result = stress.summarize(self.resource_log(8), 100, 0, False, 16)
        self.assertFalse(result['passed'])
        self.assertEqual(result['handleTrend']['growth'], 720)

    def test_stable_handles_pass(self):
        self.assertTrue(stress.summarize(self.resource_log(0), 100, 0, False, 16)['passed'])

    def test_unknown_handles_do_not_pass_resource_check(self):
        self.assertFalse(stress.summarize(self.log(range(1, 101)), 100, 0, False, 16)['passed'])

    def test_memory_is_observed_unless_a_bound_is_requested(self):
        result = stress.summarize(self.resource_log(0), 100, 0, False, 8)
        self.assertTrue(result['passed'])
        self.assertEqual(result['resourceTrends']['privateBytes']['growth'], 9000)
        self.assertFalse(stress.summarize(self.resource_log(0), 100, 0, False, 8, 8999)['passed'])
        self.assertTrue(stress.summarize(self.resource_log(0), 100, 0, False, 8, 9000)['passed'])

    def test_missing_or_invalid_resource_sample_cannot_pass_requested_bound(self):
        for bad in (None, -1, True, 1.5):
            lines = self.resource_log(0).splitlines()
            sample = json.loads(lines[40].removeprefix('LIFECYCLE '))
            sample['handles'] = bad
            lines[40] = 'LIFECYCLE ' + json.dumps(sample)
            self.assertFalse(stress.summarize('\n'.join(lines), 100, 0, False, 8)['passed'])
        self.assertFalse(stress.summarize(self.log(range(1, 101)), 100, 0, False, None, 8)['passed'])


class WatchdogTests(unittest.TestCase):
    def run_child(self, code, timeout=3, limit=4096):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            result = stress.run_capture([sys.executable, '-c', code], output, timeout, limit)
            logs = [(output / name).read_bytes() for name in ('stdout.log', 'stderr.log')]
            self.assertLessEqual(sum(map(len, logs)), limit)
            return result, logs

    def test_native_failure_preserves_diagnostics(self):
        result, logs = self.run_child("import sys; print('failed', file=sys.stderr); sys.exit(7)")
        self.assertEqual(result, (7, False, False))
        self.assertIn(b'failed', logs[1])

    def test_timeout_terminates_and_joins_child(self):
        start = time.monotonic()
        result, _ = self.run_child("import time; time.sleep(60)", timeout=.2)
        self.assertTrue(result[1])
        self.assertNotEqual(result[0], 0)
        self.assertLess(time.monotonic() - start, 5)

    def test_log_overflow_fails_even_when_child_exits_zero(self):
        result, _ = self.run_child("import sys; sys.stderr.write('x'*10000)")
        self.assertTrue(result[2])

    def test_continuous_log_output_is_stopped(self):
        result, _ = self.run_child("import os\nwhile True: os.write(2,b'x'*4096)")
        self.assertTrue(result[2])
        self.assertNotEqual(result[0], 0)


class MatrixEvidenceTests(unittest.TestCase):
    def exercise(self, fault=None, blocked=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build, output = root / 'build', root / 'evidence'
            build.mkdir()
            for name in ('LiveCaptureTest.exe', 'WindowsCaptureLifecycleTest.exe'):
                (build / name).write_bytes(b'fixture binary')
            def launch(command, *args, **kwargs):
                evidence = Path(command[3]); evidence.mkdir()
                if fault != 'missing':
                    detail = {'passed': fault != 'rejected', 'executableSha256': matrix.runner.sha256(Path(command[2]))}
                    if fault == 'identity': detail['executableSha256'] = 'wrong binary'
                    (evidence / 'result.json').write_text(json.dumps(detail), encoding='utf-8')
                return {'passed': True, 'exitCode': 0}
            with patch.object(sys, 'argv', ['matrix', str(build), str(output), '--cycles', '20']), \
                 patch.object(matrix.runner, 'desktop_unavailable', return_value=blocked), \
                 patch.object(matrix.runner, 'run_process', side_effect=launch) as calls, \
                 contextlib.redirect_stdout(io.StringIO()):
                code = matrix.main()
            return code, json.loads((output / 'result.json').read_text()), calls.call_count

    def test_zero_exit_cannot_hide_failed_missing_or_wrong_binary_evidence(self):
        for fault in ('missing', 'rejected', 'identity'):
            code, report, count = self.exercise(fault)
            self.assertEqual(code, 1)
            self.assertFalse(report['passed'])
            self.assertEqual(count, 1)

    def test_unavailable_desktop_records_block_without_launch(self):
        code, report, count = self.exercise(blocked='locked test desktop')
        self.assertEqual(code, 2)
        self.assertFalse(report['passed'])
        self.assertTrue(report['blocked'])
        self.assertEqual(count, 0)

    def test_all_four_case_reports_are_required(self):
        code, report, count = self.exercise()
        self.assertEqual(code, 0)
        self.assertTrue(report['passed'])
        self.assertEqual(count, 4)


if __name__ == '__main__':
    unittest.main()
