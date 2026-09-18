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
            samples.append({'phase': phase, 'second': second, 'intervalSeconds': 1.0, 'fps': [30] * 4,
                'capacityBps': (4000000 if impaired else 20000000) if scenario == 'collapse' else 100000000,
                'delayMeanMs': 25 if jittered else 0, 'delayStddevMs': 10 if jittered else 0,
                'configuredLossPercent': (2 if scenario == 'loss2' else 5) if impaired and scenario in ('loss2', 'loss5') else 0,
                'allowReordering': impaired and scenario == 'reorder', 'duplicateEvery': 50 if impaired and scenario == 'duplicate' else 0,
                'ingressBps': 3500000 if phase == 'impaired' else 8000000,
                'lost': int(affected and scenario in ('loss2', 'loss5')),
                'overflow': 0, 'queued': 0, 'duplicated': int(affected and scenario == 'duplicate'),
                'reordered': int(affected and scenario == 'reorder'),
                'peers': [{'viewer': i, 'audioBlocks': count * 100, 'pending': 0,
                    'payloadBps': 7000000, 'availableOutgoingBps': 8000000, 'rttMs': 1,
                    'lossFraction': 0, 'jitterBufferMeanMs': 12, 'jitterBufferRecentMs': 10} for i in range(4)]})
    return {'passed': True, 'timedOut': False, 'exitCode': 0, 'executableSha256': 'hash',
        'metrics': {'schema': 2, 'scenario': scenario, 'seed': 12345, 'released': True, 'fastAudioExperiment': False,
                    'inputApplied': True, 'inputRevoked': True, 'externalLatencyVerified': False,
                    'inputResponseInternalMs': 100, 'inputResponseSamples': 1, 'inputResponseEndpoint': 'decoded-frame-consumption',
                    'peakQueued': 200, 'peakBytes': 200000, 'maximumSchedulingDelayUs': 1000,
                    'warmupIngressBps': [8000000] * 20, 'samples': samples}}


class ImpairmentEvidenceTests(unittest.TestCase):
    def test_handoff_measurement_and_missing_data(self):
        report = evidence('loss2')
        with self.assertRaises(ValueError):
            module.validate(report, 'loss2', 'hash', require_handoff=True)
        self.assertFalse(module.validate(report, 'loss2', 'hash')['decodedFrameHandoff']['measured'])
        for sample in report['metrics']['samples']:
            for peer in sample['peers']:
                peer['handoff'] = {'schema': 1, 'scope': 'decoded-frame-handoff',
                    'received': 10, 'delivered': 10, 'replaced': 0, 'failed': 0,
                    'discardedOnStop': 0, 'inFlight': 0, 'pending': 0,
                    'pendingAgeMs': None, 'lastWaitMs': 2, 'maxWaitMs': 3}
        result = module.validate(report, 'loss2', 'hash', require_handoff=True)
        self.assertEqual(result['decodedFrameHandoff']['samples'], 144)
        self.assertFalse(result['decodedFrameHandoff']['externalLatencyVerified'])
        report['metrics']['samples'][5]['peers'][0]['handoff']['received'] += 1
        with self.assertRaises(ValueError): module.validate(report, 'loss2', 'hash', require_handoff=True)

    def test_invalid_handoff_evidence(self):
        valid = {'schema': 1, 'scope': 'decoded-frame-handoff', 'received': 11,
                 'delivered': 10, 'replaced': 0, 'failed': 0, 'discardedOnStop': 0,
                 'inFlight': 0, 'pending': 1, 'pendingAgeMs': 2, 'lastWaitMs': 3, 'maxWaitMs': 4}
        module.handoff.validate(valid)
        for key, bad in (('schema', True), ('received', True), ('pending', 2), ('inFlight', 1),
                         ('failed', 1), ('discardedOnStop', 1), ('pendingAgeMs', None),
                         ('lastWaitMs', None), ('lastWaitMs', 5), ('maxWaitMs', None),
                         ('pendingAgeMs', float('nan')), ('maxWaitMs', float('inf'))):
            with self.subTest(key=key, bad=bad), self.assertRaises(ValueError):
                module.handoff.validate({**valid, key: bad})
        for key in valid:
            malformed = dict(valid); malformed.pop(key)
            with self.subTest(missing=key), self.assertRaises(ValueError): module.handoff.validate(malformed)
        with self.assertRaises(ValueError): module.handoff.validate(valid, {**valid, 'maxWaitMs': 5})

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
        self.reject(lambda r: r['metrics']['samples'][3]['peers'][0].pop('jitterBufferRecentMs'))
        self.reject(lambda r: r['metrics']['samples'][3]['peers'][0].update(jitterBufferRecentMs=float('inf')))

    def test_explicit_unknown_bandwidth_during_congestion(self):
        report = evidence()
        report['metrics']['samples'][13]['peers'][0]['availableOutgoingBps'] = None
        self.assertEqual(module.validate(report, 'collapse', 'hash')['unknownBandwidthEstimateSamples'], 1)
        self.reject(lambda r: r['metrics']['samples'][13]['peers'][0].pop('availableOutgoingBps'))
        self.reject(lambda r: r['metrics']['samples'][13]['peers'][0].update(availableOutgoingBps=float('nan')))

    def test_no_emission_interval_is_unknown_under_impairment(self):
        report = evidence()
        for sample in report['metrics']['samples'][12:24]: sample['peers'][0]['jitterBufferRecentMs'] = None
        result = module.validate(report, 'collapse', 'hash')
        self.assertEqual(result['unknownRecentBufferSamples'], 12)
        self.assertIsNone(result['recentBufferingMs']['impaired'][0])
        self.assertEqual(result['recentBufferingSamples']['impaired'][0], 0)
        self.reject(lambda r: r['metrics']['samples'][30]['peers'][0].update(jitterBufferRecentMs=None))

    def test_recent_buffering_does_not_infer_latency_acceptance(self):
        report = evidence()
        for sample in report['metrics']['samples'][24:]:
            sample['peers'][0]['jitterBufferRecentMs'] = 150
        result = module.validate(report, 'collapse', 'hash')
        self.assertEqual(result['recentBufferingMs']['recovery'][0], 150)
        self.assertFalse(result['externalLatencyVerified'])

    def test_older_schema_cannot_claim_input_image_measurement(self):
        report = evidence(); report['metrics']['schema'] = 1
        result = module.validate(report, 'collapse', 'hash')
        self.assertFalse(result['internalInputResponseMeasured'])
        self.assertIsNone(result['inputResponseInternalMs'])

    def test_wrong_configuration(self):
        self.reject(lambda r: r['metrics']['samples'][13].update(capacityBps=20000000))
        self.reject(lambda r: r['metrics'].pop('maximumSchedulingDelayUs'))
        self.reject(lambda r: r['metrics'].update(fastAudioExperiment=True))
        self.reject(lambda r: r['metrics'].pop('warmupIngressBps'))
        self.reject(lambda r: r['metrics'].pop('inputResponseInternalMs'))
        self.reject(lambda r: r['metrics'].update(inputResponseEndpoint='physical-display'))
        self.reject(lambda r: r['metrics']['samples'][13].pop('intervalSeconds'))

    def test_stalled_audio(self):
        self.reject(lambda r: r['metrics']['samples'][10]['peers'][0].update(audioBlocks=0))


if __name__ == '__main__':
    unittest.main()
