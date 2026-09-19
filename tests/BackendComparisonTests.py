import copy
import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('comparison', ROOT / 'scripts/compare-backends.py')
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


def evidence(backend='legacy'):
    return {'schema': 1, 'passed': True, 'backend': backend, 'scene': 'scroll', 'viewers': 1,
            'audioPlayoutMode': 'paced-discard' if backend == 'v2' else 'disabled',
            'externalLatencyVerified': False, 'physicalInput': False, 'audibleOutput': False,
            'settings': dict(width=1920, height=1080, fps=60, bitrateLimitBps=12000000,
                             audio='disabled/silent', encryption=True, warmupSeconds=5),
            'measuredSeconds': 20.1, 'cpuCorePercent': 120, 'sourceCpuCorePercent': 20,
            'mediaCpuCorePercent': 100, 'startupSeconds': 2, 'teardownSeconds': .2, 'sourceUpdates': 1200,
            'receivers': [dict(frames=1200, fps=59.7, uniqueMarkers=1100, uniqueMarkerFps=54.7,
                              invalidMarkers=0, imageAgeP50Ms=30, imageAgeP95Ms=50, imageAgeP99Ms=80,
                              lumaSamples=10000, lumaMse=40, sampledLumaPsnrDb=32)],
            'resourceSamples': [dict(privateBytes=1000000, workingSetBytes=800000, handles=200) for _ in range(20)]}


class ComparisonTests(unittest.TestCase):
    def validate(self, report):
        return comparison.validate(report, 'legacy', 'scroll', 1, 20)

    def test_complete(self):
        self.validate(evidence())

    def test_retained_only_cannot_pass_image_comparison(self):
        r = evidence(); r['consumer'] = 'retained-only'
        with self.assertRaises(ValueError): self.validate(r)

    def test_diagnostics_cannot_pass_normal_comparison(self):
        r = evidence(); r['stageDiagnostics'] = True
        with self.assertRaises(ValueError): self.validate(r)
        comparison.validate(r, 'legacy', 'scroll', 1, 20, stage_diagnostics=True)
        r = evidence(); r['consumer'] = 'capture-only-cpu-pixels'
        with self.assertRaises(ValueError): self.validate(r)

    def test_unpaced_fixture_cannot_pass(self):
        r = evidence(); del r['audioPlayoutMode']
        with self.assertRaises(ValueError): self.validate(r)
        r = evidence('v2'); r['audioPlayoutMode'] = 'unpaced-discard'
        with self.assertRaises(ValueError): comparison.validate(r, 'v2', 'scroll', 1, 20)

    def test_codec_and_timer_controls_cannot_be_mixed(self):
        r = evidence(); r.update(variant='legacy-hardware', timerPolicy='honor-resolution')
        with self.assertRaises(ValueError): self.validate(r)
        comparison.validate(r, 'legacy', 'scroll', 1, 20, 'legacy-hardware', 'honor-resolution')
        r['variant'] = 'legacy-lowlatency'
        with self.assertRaises(ValueError): comparison.validate(r, 'legacy', 'scroll', 1, 20, 'legacy-hardware', 'honor-resolution')

    def test_lossless_quality_is_not_missing_evidence(self):
        r = evidence(); r['receivers'][0].update(lumaMse=0, sampledLumaPsnrDb=None)
        self.validate(r)
        result = comparison.summarize([{'validated': True, 'metrics': r}])
        self.assertIsNone(result['scroll/1']['legacy']['medianSampledLumaPsnrDb'])
        r['receivers'][0]['lumaMse'] = 1
        with self.assertRaises(ValueError): self.validate(r)

    def test_scope_and_workload_mismatch(self):
        for key, value in [('backend', 'v2'), ('viewers', 4), ('scene', 'motion'), ('passed', False),
                           ('externalLatencyVerified', True), ('physicalInput', True), ('audibleOutput', True)]:
            r = evidence(); r[key] = value
            with self.assertRaises(ValueError): self.validate(r)
        r = evidence(); r['settings']['width'] = 640
        with self.assertRaises(ValueError): self.validate(r)

    def test_invalid_samples(self):
        for key, value in [('imageAgeP95Ms', float('nan')), ('fps', None), ('uniqueMarkers', 0),
                           ('imageAgeP50Ms', 500), ('invalidMarkers', 700), ('lumaSamples', 0)]:
            r = evidence(); r['receivers'][0][key] = value
            with self.assertRaises(ValueError): self.validate(r)
        for key in ['receivers', 'resourceSamples']:
            r = evidence(); r[key] = []
            with self.assertRaises(ValueError): self.validate(r)

    def test_process_scope_and_duration(self):
        for key, value in [('measuredSeconds', 19), ('cpuCorePercent', True),
                           ('mediaCpuCorePercent', float('inf')), ('sourceUpdates', -1)]:
            r = evidence(); r[key] = value
            with self.assertRaises(ValueError): self.validate(r)

    def test_failed_or_missing_backend_cannot_make_pair(self):
        runs = [{'validated': True, 'metrics': evidence()}]
        self.assertFalse(comparison.summarize(runs)['scroll/1']['paired'])
        runs.append({'validated': False, 'metrics': evidence('v2')})
        self.assertFalse(comparison.summarize(runs)['scroll/1']['paired'])
        runs[-1]['validated'] = True
        self.assertTrue(comparison.summarize(runs)['scroll/1']['paired'])
        runs.append(copy.deepcopy(runs[0]))
        self.assertFalse(comparison.summarize(runs)['scroll/1']['paired'])

    def test_median_of_run_percentiles_is_not_pooled_percentile(self):
        a = evidence(); b = evidence(); b['receivers'][0]['imageAgeP95Ms'] = 70
        r = comparison.summarize([{'validated': True, 'metrics': x} for x in [a, b]])
        self.assertEqual(r['scroll/1']['legacy']['medianWorstViewerImageAgeP95Ms'], 60)


if __name__ == '__main__':
    unittest.main()
