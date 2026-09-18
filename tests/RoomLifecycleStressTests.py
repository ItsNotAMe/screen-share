import importlib.util
import copy
import math
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('lifecycle', Path(__file__).parents[1] / 'scripts/test-room-lifecycle.py')
lifecycle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lifecycle)

class EvidenceTests(unittest.TestCase):
    def sample(self, cycle, seconds=1):
        return {'cycle': cycle, 'seconds': seconds, 'handles': 300, 'privateBytes': 1000000,
                'workingSetBytes': 2000000, 'gdiObjects': 3, 'userObjects': 2}

    def evidence(self):
        return {'schema': 2, 'passed': True, 'executableSha256': 'binary', 'cycles': 100, 'soakSeconds': 0,
                'idleSeconds': 0, 'slowViewer': False, 'elapsedSeconds': 100, 'memoryAccounting': False,
                'ownership': [{'cycle': n, 'ownershipReleased': True, 'peakCaptureResources': 2, 'activeSeconds': 0}
                              for n in range(1, 101)],
                'completed': list(range(1, 101)),
                'samples': [self.sample(n) for n in range(101)]}

    def continuous(self, seconds=30, slow=False, interval=5):
        value = self.evidence()
        value.update(cycles=1, soakSeconds=seconds, slowViewer=slow, completed=[1], elapsedSeconds=seconds,
                     samples=[self.sample(0)], progress=[], ownership=[{
                         'cycle': 1, 'ownershipReleased': True, 'peakCaptureResources': 2, 'activeSeconds': seconds}])
        frames, audio, replaced = [15] * 4, [10] * 4, [0] * 4
        for tick in range(1, math.ceil(seconds / interval)):
            elapsed = tick * interval
            def phase_at(at): return 'healthy' if not slow or at < 15 else 'slow' if at < 35 else 'recovered'
            phase = phase_at(elapsed) if phase_at(elapsed) == phase_at(elapsed - interval) else 'transition'
            viewers = []
            for i in range(4):
                delta = 20 if slow and phase == 'slow' and i == 0 else 150
                frames[i] += delta; audio[i] += 500
                replaced[i] += 150 - delta
                viewers.append({'viewer': i, 'frames': frames[i], 'frameDelta': delta,
                                'audioBlocks': audio[i], 'audioDelta': 500, 'received': frames[i] + replaced[i],
                                'replaced': replaced[i], 'pending': 0, 'receiver': {'jitterBufferMeanMs': 12}})
            value['progress'].append({'cycle': 1, 'frames': sum(frames), 'elapsedSeconds': elapsed,
                'intervalSeconds': interval, 'phase': phase, 'viewers': viewers, 'peakCaptureResources': 2, 'maxCaptureHandoffUs': 100})
            value['samples'].append(self.sample(-1, interval))
        value['samples'].append(self.sample(1, seconds + 5))
        return value

    def test_complete_restart_evidence_and_bound(self):
        value = self.evidence()
        result = lifecycle.evaluate(value, 100, 0, 'binary')
        self.assertTrue(result['handleBoundApplied'])
        self.assertTrue(result['ownershipVerified'])
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
        value['ownership'] = value['ownership'][:1]
        result = lifecycle.evaluate(value, 1, 0, 'binary')
        self.assertFalse(result['handleBoundApplied'])

    def test_soak_needs_elapsed_time_and_continuous_samples(self):
        value = self.continuous()
        lifecycle.evaluate(value, 1, 30, 'binary')
        for invalid in (float('nan'), float('inf'), True):
            value['elapsedSeconds'] = invalid
            with self.assertRaises(ValueError): lifecycle.evaluate(value, 1, 30, 'binary')
        value['elapsedSeconds'] = 1
        with self.assertRaises(ValueError): lifecycle.evaluate(value, 1, 30, 'binary')
        value = self.continuous(); value['samples'].pop(1)
        with self.assertRaises(ValueError): lifecycle.evaluate(value, 1, 30, 'binary')

    def test_two_hour_scheduling_drift_does_not_invent_missing_samples(self):
        value = self.continuous(7200, interval=5.006)
        self.assertEqual(len(value['progress']), 1438)
        result = lifecycle.evaluate(value, 1, 7200, 'binary')
        self.assertEqual(result['continuousDurationVerified'], 7200)
        self.assertFalse(result['soakAcceptanceComplete'])
        missing = copy.deepcopy(value)
        missing['progress'].pop(100); missing['samples'].pop(101)
        with self.assertRaises(ValueError): lifecycle.evaluate(missing, 1, 7200, 'binary')
        truncated = copy.deepcopy(value)
        truncated['progress'].pop(); truncated['samples'].pop(-2)
        with self.assertRaises(ValueError): lifecycle.evaluate(truncated, 1, 7200, 'binary')

    def test_owned_dependencies_and_capture_budget_are_required(self):
        for key, bad in [('ownershipReleased', False), ('peakCaptureResources', 11), ('peakCaptureResources', True)]:
            value = self.evidence(); value['ownership'][20][key] = bad
            with self.assertRaises(ValueError): lifecycle.evaluate(value, 100, 0, 'binary')
        value = self.evidence(); value['ownership'].pop()
        with self.assertRaises(ValueError): lifecycle.evaluate(value, 100, 0, 'binary')

    def test_idle_cleanup_cannot_replace_failed_restart_bound(self):
        value = self.evidence()
        value.update(idleSeconds=30, idleSamples=[self.sample(n, n) for n in range(31)], idleProgress=list(range(31)))
        lifecycle.evaluate(value, 100, 0, 'binary', 30)
        for sample in value['samples'][11:]: sample['handles'] += 30
        with self.assertRaises(ValueError): lifecycle.evaluate(value, 100, 0, 'binary', 30)

    def test_missing_idle_unknown_memory_and_short_idle_fail(self):
        for fault in ('missing', 'unknown', 'early'):
            value = self.evidence()
            value.update(idleSeconds=30, idleSamples=[self.sample(n, n) for n in range(31)], idleProgress=list(range(31)))
            if fault == 'missing': value['idleSamples'].pop()
            if fault == 'unknown': value['idleSamples'][15]['privateBytes'] = None
            if fault == 'early': value['idleSamples'][-1]['seconds'] = 29
            with self.assertRaises(ValueError): lifecycle.evaluate(value, 100, 0, 'binary', 30)

    def test_aggregate_growth_cannot_hide_a_stalled_viewer(self):
        value = self.continuous(); value['progress'][2]['viewers'][3]['frameDelta'] = 0
        with self.assertRaises(ValueError): lifecycle.evaluate(value, 1, 30, 'binary')
        value = self.continuous(); value['progress'][2]['viewers'].pop()
        with self.assertRaises(ValueError): lifecycle.evaluate(value, 1, 30, 'binary')

    def test_slow_viewer_isolation_and_recovery(self):
        value = self.continuous(60, True)
        result = lifecycle.evaluate(value, 1, 60, 'binary', slow_viewer=True)
        self.assertEqual(result['slowViewerIsolation']['impairedFps'], [4, 30, 30, 30])
        # Change one healthy viewer's progress while keeping internally valid counters.
        for entry in value['progress']:
            if entry['phase'] == 'slow': entry['viewers'][1]['frameDelta'] = 20
        self.recount(value)
        with self.assertRaises(ValueError): lifecycle.evaluate(value, 1, 60, 'binary', slow_viewer=True)

    def recount(self, value):
        totals = [15] * 4
        for entry in value['progress']:
            for i, viewer in enumerate(entry['viewers']):
                totals[i] += viewer['frameDelta']; viewer['frames'] = totals[i]
                viewer['received'] = viewer['frames'] + viewer['replaced'] + viewer['pending']
            entry['frames'] = sum(totals)

    def test_no_impairment_or_failed_recovery_cannot_pass(self):
        for fault in ('none', 'recovery'):
            value = self.continuous(60, True)
            for entry in value['progress']:
                if fault == 'none' and entry['phase'] == 'slow': entry['viewers'][0]['frameDelta'] = 150
                if fault == 'recovery' and entry['phase'] == 'recovered': entry['viewers'][0]['frameDelta'] = 20
            self.recount(value)
            with self.assertRaises(ValueError): lifecycle.evaluate(value, 1, 60, 'binary', slow_viewer=True)

    def test_short_or_fabricated_active_duration_cannot_pass(self):
        value = self.continuous(); value['ownership'][0]['activeSeconds'] = 29
        with self.assertRaises(ValueError): lifecycle.evaluate(value, 1, 30, 'binary')
        value = self.continuous(); value['progress'][-1]['elapsedSeconds'] = 30
        with self.assertRaises(ValueError): lifecycle.evaluate(value, 1, 30, 'binary')

    def test_backlog_catchup_and_invented_phases_cannot_pass(self):
        for fault in ('queue', 'burst', 'phase', 'replacement'):
            value = self.continuous(60, True)
            if fault == 'queue': value['progress'][4]['viewers'][0]['pending'] = 2
            if fault == 'burst': value['progress'][6]['viewers'][0]['frameDelta'] = 340
            if fault == 'phase': value['progress'][4]['phase'] = 'healthy'
            if fault == 'replacement':
                for entry in value['progress']:
                    entry['viewers'][0]['replaced'] = 0
            self.recount(value)
            with self.assertRaises(ValueError): lifecycle.evaluate(value, 1, 60, 'binary', slow_viewer=True)

    def test_memory_accounting_requires_complete_measured_snapshots(self):
        value = self.evidence(); value['memoryAccounting'] = True
        value['memorySamples'] = [{'stage': 'cycle', 'cycle': n, 'complete': True, **dict.fromkeys(
            ('heaps', 'heapBusyBytes', 'heapFreeBytes', 'heapOverheadBytes', 'heapBusyBlocks',
             'heapRegionCommittedBytes', 'privateCommittedBytes', 'mappedCommittedBytes', 'imageCommittedBytes'), 1)}
            for n in range(0, 101, 10)]
        self.assertTrue(lifecycle.evaluate(value, 100, 0, 'binary', memory_accounting=True)['memoryAccountingVerified'])
        for fault in ('missing', 'partial', 'unknown'):
            bad = copy.deepcopy(value)
            if fault == 'missing': bad['memorySamples'].pop()
            if fault == 'partial': bad['memorySamples'][2]['complete'] = False
            if fault == 'unknown': bad['memorySamples'][2]['heapBusyBytes'] = None
            with self.assertRaises(ValueError): lifecycle.evaluate(bad, 100, 0, 'binary', memory_accounting=True)

    def test_good_frame_rate_cannot_hide_receive_buffering(self):
        for bad in (None, {}, {'jitterBufferMeanMs': 101}, {'jitterBufferMeanMs': True}):
            value = self.continuous(); value['progress'][-1]['viewers'][2]['receiver'] = bad
            with self.assertRaises(ValueError): lifecycle.evaluate(value, 1, 30, 'binary')
        result = lifecycle.evaluate(self.continuous(), 1, 30, 'binary')
        self.assertEqual(result['receiverBuffering']['maximumReportedMeanMs'], 12)
        self.assertFalse(result['receiverBuffering']['externalLatencyVerified'])

if __name__ == '__main__': unittest.main()
