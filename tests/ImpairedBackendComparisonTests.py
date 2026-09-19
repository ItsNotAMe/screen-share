import copy
import importlib.util
from pathlib import Path
import unittest
from BackendComparisonTests import evidence

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('impaired', ROOT / 'scripts/compare-impaired-backends.py')
impaired = importlib.util.module_from_spec(spec)
spec.loader.exec_module(impaired)


def sample():
    r = evidence()
    r.update(scene='motion', variant='legacy-hardware', timerPolicy='honor-resolution', measuredSeconds=36.1,
             networkAdaptation='legacy-feedback-enabled')
    r['resourceSamples'] = r['resourceSamples'] * 2
    r['resourceSamples'] = r['resourceSamples'][:36]
    phases = []
    for index, phase in enumerate(['baseline', 'impaired', 'recovery']):
        counters = lambda n: dict(received=n * 10000, delivered=n * 10000, deliveredBytes=n * 12000000, lost=0, overflow=0)
        phases.append(dict(phase=phase, seconds=12.01, capacityBps=4000000 if index == 1 else 20000000,
            lossPercent=0, delayMs=0, jitterMs=0, before=counters(index), after=counters(index + 1),
            receivers=[dict(frames=600, invalidMarkers=0, freshFrames=550, freshFps=550 / 12.01,
                displayedAgeSamples=1200, displayedAgeP95Ms=80, displayedAgeP99Ms=100, displayedAgeMaximumMs=120)]))
    r['network'] = dict(scenario='collapse', seed=12345, impairedViewer=0, queuePackets=256, invalid=False, phases=phases)
    return r


class ImpairedComparisonTests(unittest.TestCase):
    def validate(self, report):
        return impaired.validate(report, 'legacy-hardware', 1, 36, 'collapse')

    def test_complete(self):
        self.validate(sample())

    def test_normal_and_impaired_cannot_mix(self):
        with self.assertRaises(ValueError):
            impaired.comparison.validate(sample(), 'legacy', 'motion', 1, 36, 'legacy-hardware', 'honor-resolution')
        r = sample(); del r['network']
        with self.assertRaises(ValueError): self.validate(r)

    def test_missing_or_wrong_model(self):
        for key, value in [('seed', 7), ('impairedViewer', False), ('queuePackets', 257), ('scenario', 'loss5'),
                           ('invalid', True), ('phases', [])]:
            r = sample(); r['network'][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): self.validate(r)

    def test_wrong_phase_or_bypassed_network(self):
        for key, value in [('phase', 'baseline'), ('capacityBps', 20000000), ('seconds', 2), ('lossPercent', 5),
                           ('delayMs', 25), ('jitterMs', 10), ('receivers', [])]:
            r = sample(); r['network']['phases'][1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): self.validate(r)
        r = sample(); r['network']['phases'][0]['after']['deliveredBytes'] = 100
        with self.assertRaises(ValueError): self.validate(r)
        r = sample(); r['network']['phases'][1]['after']['received'] = 10000
        with self.assertRaises(ValueError): self.validate(r)

    def test_stale_image_samples_cannot_be_omitted(self):
        for key, value in [('displayedAgeSamples', 20), ('displayedAgeP95Ms', float('nan')),
                           ('displayedAgeP99Ms', 70), ('displayedAgeMaximumMs', 90), ('freshFrames', 900),
                           ('freshFps', 60), ('invalidMarkers', 601), ('frames', 600.0)]:
            r = sample(); r['network']['phases'][1]['receivers'][0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): self.validate(r)

    def test_different_encoder_rejected(self):
        r = sample(); r.update(backend='v2', variant='v2', audioPlayoutMode='paced-discard', softwareFallbacks=0,
                               senders=[{'encoder': 'mf-h264-software'}], networkAdaptation='webrtc-congestion-control')
        with self.assertRaises(ValueError): impaired.validate(r, 'v2', 1, 36, 'collapse')

    def test_advice_only_legacy_is_not_a_fair_adaptive_control(self):
        r = sample(); r.pop('networkAdaptation')
        with self.assertRaises(ValueError): self.validate(r)


if __name__ == '__main__':
    unittest.main()
