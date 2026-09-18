"""Validate local decoded-frame handoff observations; never infer display latency."""
import math


def validate(value, previous=None):
    if not isinstance(value, dict) or type(value.get('schema')) is not int or value['schema'] != 1 or value.get('scope') != 'decoded-frame-handoff':
        raise ValueError('Missing decoded-frame handoff evidence')
    counts = ('received', 'replaced', 'delivered', 'failed', 'discardedOnStop', 'inFlight', 'pending')
    if any(type(value.get(key)) is not int or value[key] < 0 for key in counts):
        raise ValueError('Invalid handoff ownership counters')
    if value['pending'] > 1 or value['inFlight'] > 1 or value['received'] != sum(value[key] for key in counts[1:]):
        raise ValueError('Unbounded or unaccounted handoff ownership')
    if value['failed'] or value['discardedOnStop'] or value['inFlight']:
        raise ValueError('Active single-consumer proof lost frames or sampled during conversion')
    for key in ('pendingAgeMs', 'lastWaitMs', 'maxWaitMs'):
        if key not in value:
            raise ValueError('Missing handoff timing field')
        age = value[key]
        if age is not None and (type(age) not in (int, float) or not math.isfinite(age) or age < 0):
            raise ValueError('Invalid handoff timing')
    if (value['pendingAgeMs'] is None) != (value['pending'] == 0) or \
       (value['lastWaitMs'] is None) != (value['delivered'] == 0) or value['maxWaitMs'] is None or \
       (value['lastWaitMs'] is not None and value['lastWaitMs'] > value['maxWaitMs']):
        raise ValueError('Inconsistent handoff timing availability')
    if previous and any(value[key] < previous[key] for key in (*counts[:5], 'maxWaitMs')):
        raise ValueError('Handoff counters moved backwards')


def summarize(values):
    if not values:
        return {'measured': False, 'externalLatencyVerified': False}
    return {'measured': True, 'scope': 'decoded-frame-handoff', 'samples': len(values),
            'maximumWaitMs': max(v['maxWaitMs'] for v in values),
            'maximumSampledPendingAgeMs': max((v['pendingAgeMs'] for v in values if v['pendingAgeMs'] is not None), default=None),
            'externalLatencyVerified': False}
