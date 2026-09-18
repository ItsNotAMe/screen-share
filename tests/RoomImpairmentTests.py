import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('impairment', ROOT / 'scripts/test-room-impairment.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def evidence(scenario='collapse'):
    samples = []
    for phase in ('baseline', 'impaired', 'recovery'):
        for second in range(1, 13):
            count = len(samples) + 1
            affected = phase != 'baseline'
            impaired = phase == 'impaired'
            jittered = impaired and scenario in ('loss2', 'loss5', 'reorder')
            samples.append({'phase': phase, 'second': second, 'fps': [30] * 4,
                'capacityBps': 4000000 if impaired and scenario == 'collapse' else 20000000,
                'delayMeanMs': 25 if jittered else 0, 'delayStddevMs': 10 if jittered else 0,
                'configuredLossPercent': (2 if scenario == 'loss2' else 5) if impaired and scenario in ('loss2', 'loss5') else 0,
                'allowReordering': impaired and scenario == 'reorder', 'duplicateEvery': 50 if impaired and scenario == 'duplicate' else 0,
                'ingressBps': 3500000 if phase == 'impaired' else 8000000,
                'lost': int(affected and scenario in ('loss2', 'loss5')),
                'overflow': 0, 'queued': 0, 'duplicated': int(affected and scenario == 'duplicate'),
                'reordered': int(affected and scenario == 'reorder'),
                'peers': [{'viewer': i, 'audioBlocks': count * 100, 'pending': 0,
                    'payloadBps': 7000000, 'availableOutgoingBps': 8000000, 'rttMs': 1,
                    'lossFraction': 0, 'jitterBufferMeanMs': 12} for i in range(4)]})
    return {'passed': True, 'timedOut': False, 'exitCode': 0, 'executableSha256': 'hash',
        'metrics': {'schema': 1, 'scenario': scenario, 'seed': 12345, 'released': True,
                    'inputApplied': True, 'inputRevoked': True, 'externalLatencyVerified': False,
                    'peakQueued': 200, 'peakBytes': 200000, 'maximumSchedulingDelayUs': 1000, 'samples': samples}}


class ImpairmentEvidenceTests(unittest.TestCase):
    def reject(self, mutate, scenario='collapse'):
        report = evidence(scenario)
        mutate(report)
        with self.assertRaises(ValueError):
            module.validate(report, scenario, 'hash')

    def test_complete_scenarios(self):
        for scenario in module.SCENARIOS[:-1]:
            self.assertTrue(module.validate(evidence(scenario), scenario, 'hash')['recovered'])

    def test_separate_processes(self):
        report = evidence()
        report['metrics'] = {'passed': True, 'separateProcesses': 5, 'externalLatencyVerified': False, 'physicalInput': False,
            'viewers': [{'pid': i + 1, 'passed': True, 'runtimeReleased': True, 'frames': 300, 'audioBlocks': 1000} for i in range(4)]}
        self.assertEqual(module.validate(report, 'processes', 'hash')['separateProcesses'], 5)
        report['metrics']['viewers'][1]['pid'] = 1
        with self.assertRaises(ValueError): module.validate(report, 'processes', 'hash')

    def test_process_failure(self):
        for key, value in (('passed', False), ('timedOut', True), ('exitCode', 1), ('logLimitExceeded', True), ('executableSha256', 'other')):
            self.reject(lambda r: r.update({key: value}))

    def test_missing_ownership_input_or_scope(self):
        for key in ('released', 'inputApplied', 'inputRevoked', 'externalLatencyVerified'):
            self.reject(lambda r: r['metrics'].pop(key))

    def test_missing_or_reordered_phase(self):
        self.reject(lambda r: r['metrics']['samples'].pop())
        self.reject(lambda r: r['metrics']['samples'][20].update(phase='baseline'))

    def test_missing_viewer(self):
        self.reject(lambda r: r['metrics']['samples'][0]['fps'].pop())
        self.reject(lambda r: r['metrics']['samples'][0]['peers'].pop())

    def test_stalled_healthy_viewer(self):
        self.reject(lambda r: r['metrics']['samples'][15]['fps'].__setitem__(2, 0))

    def test_failed_recovery(self):
        self.reject(lambda r: [s['fps'].__setitem__(0, 0) for s in r['metrics']['samples'][24:]])

    def test_no_actual_loss_reorder_or_duplicate(self):
        for scenario, key in (('loss2', 'lost'), ('loss5', 'lost'), ('reorder', 'reordered'), ('duplicate', 'duplicated')):
            self.reject(lambda r: [s.update({key: 0}) for s in r['metrics']['samples']], scenario)

    def test_no_effective_collapse(self):
        self.reject(lambda r: [s.update(ingressBps=2000000) for s in r['metrics']['samples'][:12]])
        self.reject(lambda r: [s.update(ingressBps=8000000) for s in r['metrics']['samples'][12:24]])
        self.reject(lambda r: [s.update(ingressBps=3000000) for s in r['metrics']['samples'][24:]])

    def test_unintended_loss(self):
        self.reject(lambda r: [s.update(overflow=1) for s in r['metrics']['samples'][12:]], 'reorder')

    def test_unbounded_queues(self):
        self.reject(lambda r: r['metrics'].update(peakQueued=2049))
        self.reject(lambda r: r['metrics']['samples'][12]['peers'][0].update(pending=2))

    def test_missing_nonfinite_telemetry(self):
        self.reject(lambda r: r['metrics']['samples'][3]['peers'][0].pop('jitterBufferMeanMs'))
        self.reject(lambda r: r['metrics']['samples'][3].update(ingressBps=float('nan')))

    def test_wrong_configuration(self):
        self.reject(lambda r: r['metrics']['samples'][13].update(capacityBps=20000000))
        self.reject(lambda r: r['metrics'].pop('maximumSchedulingDelayUs'))

    def test_stalled_audio(self):
        self.reject(lambda r: r['metrics']['samples'][10]['peers'][0].update(audioBlocks=0))


if __name__ == '__main__':
    unittest.main()
