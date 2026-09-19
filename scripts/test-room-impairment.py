"""Silent, isolated real-packet network scenarios with fail-closed evidence checks."""
import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import statistics

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('room_runner', ROOT / 'scripts/test-room-regression.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
spec = importlib.util.spec_from_file_location('handoff_evidence', ROOT / 'scripts/frame_handoff_evidence.py')
handoff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(handoff)
SCENARIOS = ('collapse', 'loss2', 'loss5', 'reorder', 'duplicate', 'processes')


def numeric(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def validate(report, scenario, executable_hash, fast_audio=False, require_handoff=False, require_phase_response=False, require_response_stages=False, require_freshness=False, require_settling=False):
    if report.get('passed') is not True or report.get('timedOut') is not False or report.get('exitCode') != 0 or \
       report.get('logLimitExceeded', False) or report.get('executableSha256') != executable_hash:
        raise ValueError('Native process failed, timed out, or executable identity differs')
    metrics = report.get('metrics', {})
    if scenario == 'processes':
        viewers = metrics.get('viewers')
        if metrics.get('passed') is not True or metrics.get('separateProcesses') != 5 or \
           metrics.get('externalLatencyVerified') is not False or metrics.get('physicalInput') is not False or \
           not isinstance(viewers, list) or len(viewers) != 4:
            raise ValueError('Missing separate-process evidence')
        ids = set()
        for viewer in viewers:
            if viewer.get('passed') is not True or viewer.get('runtimeReleased') is not True or \
               type(viewer.get('pid')) is not int or viewer['pid'] <= 0 or viewer['pid'] in ids or \
               not numeric(viewer.get('frames')) or viewer['frames'] < 250 or \
               not numeric(viewer.get('audioBlocks')) or viewer['audioBlocks'] < 700:
                raise ValueError('Invalid separate-process receiver result')
            ids.add(viewer['pid'])
        return {'separateProcesses': 5, 'runtimeReleased': True, 'externalLatencyVerified': False}
    if type(metrics.get('schema')) is not int or metrics['schema'] not in (1, 2, 3) or metrics.get('scenario') != scenario or metrics.get('seed') != 12345 or \
       metrics.get('released') is not True or metrics.get('externalLatencyVerified') is not False or \
       metrics.get('inputApplied') is not True or metrics.get('inputRevoked') is not True:
        raise ValueError('Missing scenario, seed, ownership, or scope evidence')
    for key, maximum in (('peakQueued', 2048), ('peakBytes', 16 * 1024 * 1024)):
        if type(metrics.get(key)) is not int or not 0 < metrics[key] <= maximum:
            raise ValueError('Missing or excessive packet ownership')
    if metrics.get('fastAudioExperiment') is not fast_audio:
        raise ValueError('Audio experiment configuration differs from request')
    response_measured = metrics['schema'] >= 2
    freshness = metrics.get('imageFreshnessByPhase')
    if require_freshness or require_settling or freshness is not None:
        if metrics.get('staleImageThresholdMs') != 150 or not isinstance(freshness, dict) or set(freshness) != {'baseline', 'impaired', 'recovery'}:
            raise ValueError('Missing visual image freshness evidence')
        for phase, peers in freshness.items():
            if not isinstance(peers, list) or len(peers) != 4:
                raise ValueError('Missing visual freshness peers')
            for index, peer in enumerate(peers):
                if not isinstance(peer, dict) or type(peer.get('viewer')) is not int or peer['viewer'] != index or \
                   type(peer.get('validMarkers')) is not int or peer['validMarkers'] < 50 or \
                   type(peer.get('invalidMarkers')) is not int or not 0 <= peer['invalidMarkers'] <= peer['validMarkers'] / 20 or \
                   not numeric(peer.get('maximumDisplayedImageAgeMs')) or not 0 < peer['maximumDisplayedImageAgeMs'] < 60000 or \
                   not numeric(peer.get('lastStaleAtPhaseMs')) or peer['lastStaleAtPhaseMs'] > 60000:
                    raise ValueError('Invalid or insufficient visual freshness samples')
                if require_settling and scenario == 'collapse' and peer['lastStaleAtPhaseMs'] > 3000:
                    raise ValueError('Displayed image did not settle within three seconds')
                if (peer['maximumDisplayedImageAgeMs'] > 150) != (peer['lastStaleAtPhaseMs'] > 0):
                    raise ValueError('Inconsistent displayed-image stale interval')
                if 'maximumSettledImageAgeMs' in peer and (not numeric(peer['maximumSettledImageAgeMs']) or
                        not 0 < peer['maximumSettledImageAgeMs'] <= peer['maximumDisplayedImageAgeMs'] or
                        (peer['maximumSettledImageAgeMs'] > 150) != (peer['lastStaleAtPhaseMs'] > 3000)):
                    raise ValueError('Inconsistent displayed-image age after settling period')
                if require_settling and scenario == 'collapse' and peer['invalidMarkers']:
                    raise ValueError('Unknown displayed-image age cannot certify settling')
    phase_response = metrics.get('inputResponseByPhaseMs')
    response_stages = metrics.get('inputResponseStagesByPhase')
    if require_response_stages or response_stages is not None:
        if not isinstance(response_stages, dict) or set(response_stages) != {'baseline', 'impaired', 'recovery'}:
            raise ValueError('Missing response stage phases')
        for phase, stages in response_stages.items():
            if not isinstance(stages, list) or len(stages) != 5:
                raise ValueError('Missing response stage samples')
            for stage in stages:
                if not isinstance(stage, dict) or any(type(stage.get(k)) is not int or stage[k] < 0 for k in
                        ('inputDeliveryMs', 'returnImageMs', 'totalMs', 'phaseMaximumLinkResidenceUs')):
                    raise ValueError('Invalid response stage measurements')
                if stage['inputDeliveryMs'] + stage['returnImageMs'] != stage['totalMs']:
                    raise ValueError('Response stage sum differs from total')
            if not isinstance(phase_response, dict) or [s['totalMs'] for s in stages] != phase_response.get(phase):
                raise ValueError('Response stage totals differ from phase samples')
    if require_phase_response and metrics['schema'] < 3:
        raise ValueError('Missing repeated phase response evidence')
    if metrics['schema'] >= 3:
        if not isinstance(phase_response, dict) or set(phase_response) != {'baseline', 'impaired', 'recovery'}:
            raise ValueError('Missing response phases')
        for values in phase_response.values():
            if not isinstance(values, list) or len(values) != 5 or any(type(v) is not int or not 0 <= v <= 10000 for v in values):
                raise ValueError('Invalid repeated phase response samples')
        if metrics.get('inputResponseInternalMs') != phase_response['impaired'][2]:
            raise ValueError('Inconsistent legacy response sample')
    if response_measured and (type(metrics.get('inputResponseInternalMs')) is not int or not 0 <= metrics['inputResponseInternalMs'] <= 10000 or \
       metrics.get('inputResponseSamples') != 1 or metrics.get('inputResponseEndpoint') != 'decoded-frame-consumption'):
        raise ValueError('Missing internal input-to-image response evidence')
    if response_measured and metrics.get('inputResponseScene', 'full-frame') not in ('full-frame', 'localized-marker'):
        raise ValueError('Unknown input response scene')
    if type(metrics.get('maximumSchedulingDelayUs')) is not int or metrics['maximumSchedulingDelayUs'] < 0:
        raise ValueError('Missing simulator scheduling observation')
    samples = metrics.get('samples')
    warmup = metrics.get('warmupIngressBps')
    if not isinstance(warmup, list) or len(warmup) != 20 or not all(numeric(n) for n in warmup):
        raise ValueError('Missing fixed convergence interval')
    if not isinstance(samples, list) or len(samples) != 36:
        raise ValueError('Missing phase samples')
    previous = None
    phases = {name: [] for name in ('baseline', 'impaired', 'recovery')}
    handoffs = []
    for index, sample in enumerate(samples):
        phase = ('baseline', 'impaired', 'recovery')[index // 12]
        if sample.get('phase') != phase or sample.get('second') != index % 12 + 1:
            raise ValueError('Unexpected phase ordering')
        if response_measured and (not numeric(sample.get('intervalSeconds')) or not 0 < sample['intervalSeconds'] <= 10):
            raise ValueError('Missing observation interval')
        impaired = phase == 'impaired'
        jittered = impaired and scenario in ('loss2', 'loss5', 'reorder')
        configuration = {'capacityBps': (4000000 if impaired else 20000000) if scenario == 'collapse' else 100000000,
            'delayMeanMs': 25 if jittered else 0, 'delayStddevMs': 10 if jittered else 0,
            'configuredLossPercent': (2 if scenario == 'loss2' else 5) if impaired and scenario in ('loss2', 'loss5') else 0,
            'allowReordering': impaired and scenario == 'reorder', 'duplicateEvery': 50 if impaired and scenario == 'duplicate' else 0}
        if any(sample.get(key) != value for key, value in configuration.items()):
            raise ValueError('Recorded network configuration differs from requested scenario')
        fps, peers = sample.get('fps'), sample.get('peers')
        if not isinstance(fps, list) or len(fps) != 4 or not all(numeric(n) and n <= 120 for n in fps):
            raise ValueError('Missing or invalid four-viewer frame rates')
        if not isinstance(peers, list) or len(peers) != 4:
            raise ValueError('Missing peer telemetry')
        for i, peer in enumerate(peers):
            if require_handoff or 'handoff' in peer:
                handoff.validate(peer.get('handoff'), previous['peers'][i].get('handoff') if previous else None)
                if peer['handoff']['pending'] != peer.get('pending'):
                    raise ValueError('Handoff pending count differs from presentation')
                handoffs.append(peer['handoff'])
            if peer.get('viewer') != i or not all(numeric(peer.get(key)) for key in
                ('audioBlocks', 'payloadBps', 'rttMs', 'lossFraction', 'jitterBufferMeanMs')):
                raise ValueError('Missing peer observations')
            # WebRTC can temporarily omit the bandwidth estimate under congestion.
            # Keep explicit unknowns; packet-byte counters independently prove shaping.
            if 'availableOutgoingBps' not in peer or (peer['availableOutgoingBps'] is not None and not numeric(peer['availableOutgoingBps'])):
                raise ValueError('Missing or invalid bandwidth estimate field')
            if type(peer.get('pending')) is not int or not 0 <= peer['pending'] <= 1:
                raise ValueError('Presentation queue is missing or unbounded')
            recent = peer.get('jitterBufferRecentMs')
            if 'jitterBufferRecentMs' not in peer or (recent is not None and (not numeric(recent) or recent > 60000)) or \
               (phase != 'impaired' and index >= 2 and recent is None):
                raise ValueError('Missing or invalid recent receiver buffering')
            if previous and peer['audioBlocks'] <= previous['peers'][i]['audioBlocks']:
                raise ValueError('Audio stopped progressing')
        for key in ('lost', 'overflow', 'queued', 'duplicated', 'reordered'):
            if type(sample.get(key)) is not int or sample[key] < 0 or (key != 'queued' and previous and sample[key] < previous[key]):
                raise ValueError('Invalid packet accounting')
        if sample['queued'] > 2048 or not numeric(sample.get('ingressBps')):
            raise ValueError('Invalid ingress/queue measurement')
        phases[phase].append(sample)
        previous = sample
    # Ignore the initial convergence period, but never discard its raw evidence.
    baseline, recovery = phases['baseline'][-5:], phases['recovery'][-5:]
    for viewer in range(4):
        healthy_fps = statistics.mean(s['fps'][viewer] for s in baseline)
        if healthy_fps < 20 or statistics.mean(s['fps'][viewer] for s in recovery) < max(20, healthy_fps * .8):
            raise ValueError('Healthy media or recovery frame rate failed')
        if viewer and min(s['fps'][viewer] for s in phases['impaired']) < healthy_fps * .7:
            raise ValueError('Impairment disrupted a healthy viewer')
    before, after = phases['baseline'][-1], phases['impaired'][-1]
    if scenario == 'collapse':
        if statistics.mean(s['ingressBps'] for s in baseline) <= 5000000:
            raise ValueError('Insufficient offered load to exercise 4 Mbps collapse')
        if statistics.mean(s['ingressBps'] for s in phases['impaired'][-5:]) > 4400000:
            raise ValueError('Bandwidth shaping was bypassed')
        if statistics.mean(s['ingressBps'] for s in recovery) <= 4400000:
            raise ValueError('Sender failed to regain bandwidth after recovery')
    else:
        counter = 'lost' if scenario in ('loss2', 'loss5') else 'reordered' if scenario == 'reorder' else 'duplicated'
        if after[counter] <= before[counter]:
            raise ValueError('Requested impairment was not observed')
        if scenario in ('reorder', 'duplicate') and (after['lost'] != before['lost'] or after['overflow'] != before['overflow']):
            raise ValueError('Reordering/duplication scenario introduced packet loss')
    recent_tails = {phase: [[s['peers'][i]['jitterBufferRecentMs'] for s in samples[-5:]
        if s['peers'][i]['jitterBufferRecentMs'] is not None] for i in range(4)] for phase, samples in phases.items()}
    return {'healthyViewerIsolation': True, 'recovered': True,
            'imageFreshnessByPhase': freshness, 'settlingRequired': require_settling and scenario == 'collapse',
            'inputResponseStagesByPhase': response_stages,
            'inputResponseByPhaseMs': phase_response if metrics['schema'] >= 3 else None,
            'decodedFrameHandoff': handoff.summarize(handoffs),
            'internalInputResponseMeasured': response_measured,
            'inputResponseInternalMs': metrics.get('inputResponseInternalMs') if response_measured else None,
            'inputResponseScene': metrics.get('inputResponseScene', 'full-frame') if response_measured else None,
            'unknownBandwidthEstimateSamples': sum(p['availableOutgoingBps'] is None for s in samples for p in s['peers']),
            'baselineIngressBps': statistics.mean(s['ingressBps'] for s in baseline),
            'impairedIngressBps': statistics.mean(s['ingressBps'] for s in phases['impaired'][-5:]),
            'recoveryFps': [statistics.mean(s['fps'][i] for s in recovery) for i in range(4)],
            'recentBufferingMs': {phase: [statistics.mean(values) if values else None for values in peers]
                for phase, peers in recent_tails.items()},
            'recentBufferingSamples': {phase: [len(values) for values in peers] for phase, peers in recent_tails.items()},
            'unknownRecentBufferSamples': sum(p['jitterBufferRecentMs'] is None for s in samples for p in s['peers']),
            'externalLatencyVerified': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--scenario', choices=SCENARIOS, action='append', help='May be repeated; default runs every case')
    parser.add_argument('--fast-audio-experiment', action='store_true', help='Proof-only NetEq acceleration experiment; not production policy')
    parser.add_argument('--event-logs', action='store_true', help='Bounded synthetic host RTC traces and probe/ALR summaries')
    parser.add_argument('--require-settling', action='store_true', help='Require collapse displayed-image freshness to settle within three seconds')
    args = parser.parse_args()
    if args.event_logs and (not args.scenario or 'processes' in args.scenario):
        parser.error('Event logs require explicit packet scenarios (not processes)')
    if args.fast_audio_experiment and (not args.scenario or 'processes' in args.scenario):
        parser.error('Audio experiment requires explicit packet scenarios (not processes)')
    args.output.mkdir(parents=True, exist_ok=False)
    executable = args.build.resolve() / 'RoomImpairmentProof.exe'
    digest = runner.sha256(executable)
    node = shutil.which('node')
    if not node:
        raise RuntimeError('Node is required')
    report = {'schema': 1, 'passed': False, 'scenarios': {}, 'externalLatencyVerified': False,
              'fastAudioExperiment': args.fast_audio_experiment,
              'eventLogs': args.event_logs,
              'requireSettling': args.require_settling,
              'sourceHashes': {str(p): runner.sha256(ROOT / p) for p in (
                  'backend/media/webrtc/MediaNetworkPolicy.h', 'backend/media/webrtc/MediaEngine.cpp',
                  'tools/webrtc-proof/RoomImpairmentProof.cpp', 'tools/webrtc-proof/FrameAgeMarker.h',
                  'tools/webrtc-proof/ImpairedPacketSocket.h')},
              'probeValidatorSha256': runner.sha256(ROOT / 'scripts/rtc_probe_evidence.py') if args.event_logs else None,
              'runnerSha256': runner.sha256(Path(__file__)),
              'handoffValidatorSha256': runner.sha256(ROOT / 'scripts/frame_handoff_evidence.py'),
              'fixtureSha256': runner.sha256(ROOT / 'signaling-worker/tests/run-native-service.mjs'),
              'pinnedWebRtc': json.loads((ROOT / 'cmake/dependencies/webrtc-source.json').read_text())['commit'],
              'limitations': ['640x360@30 software H264; synthetic noise and discarded audio',
                              'One viewer UDP ingress only; no real NIC/NAT or physical input',
                              'Jitter is Gaussian (25 ms mean, 10 ms standard deviation), not capped at 50 ms',
                              'Mean jitter-buffer telemetry is not capture-to-display latency']}
    try:
        failures = []
        for scenario in dict.fromkeys(args.scenario or SCENARIOS):
            output = args.output / scenario
            output.mkdir()
            process = runner.run_process([node, ROOT / 'signaling-worker/tests/run-native-service.mjs', executable,
                output, 'network-impairment', scenario, *(['--fast-audio-experiment'] if args.fast_audio_experiment else []),
                *(['--event-logs'] if args.event_logs else [])],
                output / 'runner.log', os.environ.copy(), timeout=110)
            report['scenarios'][scenario] = {'process': process}
            try:
                if args.event_logs:
                    import rtc_probe_evidence
                    traces = sorted(output.glob('native-service-*/rtc-events/host-*.rtc'))
                    if len(traces) != 4:
                        raise ValueError('Missing or ambiguous RTC traces')
                    report['scenarios'][scenario]['probeTraces'] = [rtc_probe_evidence.read(path) for path in traces]
                if not process['passed']:
                    raise ValueError('Native process failed; inspect retained logs')
                files = list(output.glob('native-service-*/result.json'))
                if len(files) != 1:
                    raise ValueError('Missing or ambiguous native result')
                report['scenarios'][scenario]['validation'] = validate(json.loads(files[0].read_text()), scenario, digest, args.fast_audio_experiment, require_handoff=True, require_phase_response=True, require_response_stages=True, require_freshness=True, require_settling=args.require_settling)
            except Exception as error:
                report['scenarios'][scenario]['error'] = str(error)
                failures.append(scenario)
        report['passed'] = not failures
        if failures:
            report['error'] = 'Failed scenarios: ' + ', '.join(failures)
    except Exception as error:
        report['error'] = str(error)
    finally:
        (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
