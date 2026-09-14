import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("stress", Path(__file__).parents[1] / "scripts/stress-live-capture.py")
stress = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stress)


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
        lines = ['LIFECYCLE ' + json.dumps({'cycle': 0, 'handles': 100})]
        for cycle in range(1, 101):
            lines += [f'Capture cycle {cycle} destroyed', 'LIFECYCLE ' + json.dumps({'cycle': cycle, 'handles': 350 + cycle * growth})]
        return '\n'.join(lines)

    def test_handle_growth_fails_even_with_complete_cycles(self):
        result = stress.summarize(self.resource_log(8), 100, 0, False, 16)
        self.assertFalse(result['passed'])
        self.assertEqual(result['handleTrend']['growth'], 720)

    def test_stable_handles_pass(self):
        self.assertTrue(stress.summarize(self.resource_log(0), 100, 0, False, 16)['passed'])

    def test_unknown_handles_do_not_pass_resource_check(self):
        self.assertFalse(stress.summarize(self.log(range(1, 101)), 100, 0, False, 16)['passed'])


if __name__ == '__main__':
    unittest.main()
