import importlib.util
import copy
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('lifecycle', Path(__file__).parents[1] / 'scripts/test-room-lifecycle.py')
lifecycle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lifecycle)

class EvidenceTests(unittest.TestCase):
    def evidence(self):
        return {'passed': True, 'executableSha256': 'binary', 'cycles': 100, 'soakSeconds': 0,
                'completed': list(range(1, 101)),
                'samples': [{'cycle': n, 'handles': 300, 'privateBytes': 1000000} for n in range(101)]}

    def test_complete_restart_evidence_and_bound(self):
        value = self.evidence()
        result = lifecycle.evaluate(value, 100, 0, 'binary')
        self.assertTrue(result['handleBoundApplied'])
        self.assertFalse(result['soakAcceptanceComplete'])
        for sample in value['samples'][11:]: sample['handles'] += 9
        with self.assertRaises(ValueError): lifecycle.evaluate(value, 100, 0, 'binary')

    def test_failed_partial_mismatched_or_missing_evidence_cannot_pass(self):
        base = self.evidence()
        for key, bad in (('passed', False), ('completed', list(range(1, 100))),
                         ('executableSha256', 'other'), ('cycles', 99), ('samples', [])):
            value = copy.deepcopy(base); value[key] = bad
            with self.assertRaises(ValueError): lifecycle.evaluate(value, 100, 0, 'binary')

    def test_bad_handle_samples_fail_closed(self):
        for bad in (None, -1, True, 2.5):
            value = self.evidence(); value['samples'][50]['handles'] = bad
            with self.assertRaises(ValueError): lifecycle.evaluate(value, 100, 0, 'binary')
        value = self.evidence(); value['samples'][1]['cycle'] = True
        with self.assertRaises(ValueError): lifecycle.evaluate(value, 100, 0, 'binary')

    def test_short_run_does_not_apply_or_claim_resource_acceptance(self):
        value = self.evidence(); value.update(cycles=1, completed=[1], samples=value['samples'][:2])
        result = lifecycle.evaluate(value, 1, 0, 'binary')
        self.assertFalse(result['handleBoundApplied'])

    def test_soak_needs_elapsed_time_and_continuous_samples(self):
        value = self.evidence(); value.update(cycles=1, soakSeconds=30, completed=[1],
            samples=value['samples'][:2] + [{'cycle': -1, 'handles': 500, 'privateBytes': 2000000} for _ in range(5)],
            progress=[{} for _ in range(5)], elapsedSeconds=30)
        lifecycle.evaluate(value, 1, 30, 'binary')
        for invalid in (float('nan'), float('inf'), True):
            value['elapsedSeconds'] = invalid
            with self.assertRaises(ValueError): lifecycle.evaluate(value, 1, 30, 'binary')
        value['elapsedSeconds'] = 1
        with self.assertRaises(ValueError): lifecycle.evaluate(value, 1, 30, 'binary')
        value['elapsedSeconds'] = 30; value['samples'].pop()
        with self.assertRaises(ValueError): lifecycle.evaluate(value, 1, 30, 'binary')

if __name__ == '__main__': unittest.main()
