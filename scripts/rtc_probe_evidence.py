"""Extract only probe/ALR events from the pinned SDK's version-2 RTC logs.

Does not decode RTP, addresses, identities, or delta-compressed BWE batches.
Raw logs belong in private build artifacts, never in committed reports.
"""
import argparse
import hashlib
import json
from pathlib import Path

PINNED_WEBRTC = '8d208e76fb1ad2b92d557f752e9d1d7d49059efa'
LIMIT = 8 * 1024 * 1024
FIELDS = {
    16: ('begin', ('timestampMs', 'version', 'utcTimeMs')),
    17: ('end', ('timestampMs',)),
    21: ('cluster', ('timestampMs', 'id', 'bitrateBps', 'minPackets', 'minBytes')),
    22: ('success', ('timestampMs', 'id', 'bitrateBps')),
    23: ('failure', ('timestampMs', 'id', 'reason')),
    24: ('alr', ('timestampMs', 'inAlr')),
}


def varint(data, offset):
    value = 0
    for i in range(10):
        if offset >= len(data):
            raise ValueError('Truncated varint')
        byte = data[offset]
        offset += 1
        if i == 9 and byte > 1:
            raise ValueError('Varint overflow')
        value |= (byte & 127) << (7 * i)
        if not byte & 128:
            return value, offset
    raise ValueError('Unterminated varint')


def fields(data):
    offset = 0
    while offset < len(data):
        tag, offset = varint(data, offset)
        number, wire = tag >> 3, tag & 7
        if not 0 < number < 2**29:
            raise ValueError('Invalid field tag')
        if wire == 0:
            value, offset = varint(data, offset)
        else:
            if wire == 2:
                size, offset = varint(data, offset)
            elif wire in (1, 5):
                size = 8 if wire == 1 else 4
            else:
                raise ValueError('Unsupported wire type')
            if size > len(data) - offset:
                raise ValueError('Truncated field')
            value = data[offset:offset + size]
            offset += size
        yield number, wire, value


def parse(data):
    if not data or len(data) > LIMIT:
        raise ValueError('Empty or oversized RTC log')
    events, begin, end = [], None, None
    for number, wire, payload in fields(data):
        if number == 1:
            raise ValueError('Legacy RTC format unsupported')
        if wire != 2:
            raise ValueError('Invalid event wire type')
        if number not in FIELDS:
            continue
        kind, names = FIELDS[number]
        values = {}
        for key, inner_wire, value in fields(payload):
            if not 1 <= key <= len(names) or key in values or inner_wire != 0:
                raise ValueError('Unexpected or duplicate event field')
            values[key] = value
        if len(values) != len(names):
            raise ValueError('Missing event fields')
        event = {name: values[i + 1] for i, name in enumerate(names)}
        if kind == 'begin':
            if begin is not None or event['version'] != 2:
                raise ValueError('Duplicate start or unsupported RTC version')
            begin = event['timestampMs']
        elif kind == 'end':
            if end is not None:
                raise ValueError('Duplicate log end')
            end = event['timestampMs']
        else:
            if kind == 'alr' and event['inAlr'] not in (0, 1):
                raise ValueError('Invalid ALR value')
            if kind == 'failure' and event['reason'] not in range(4):
                raise ValueError('Unknown probe failure')
            events.append({'kind': kind, **event})
    if begin is None or end is None or end < begin:
        raise ValueError('Incomplete RTC log')
    for event in events:
        # StartLogging flushes history, so pre-start timestamps are legitimate.
        event['relativeMs'] = event.pop('timestampMs') - begin
        if event['relativeMs'] > end - begin:
            raise ValueError('Event after log end')
    events.sort(key=lambda value: value['relativeMs'])
    return {'schema': 1, 'pinnedWebRtc': PINNED_WEBRTC,
            'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data),
            'durationMs': end - begin, 'events': events,
            'counts': {kind: sum(e['kind'] == kind for e in events)
                       for kind in ('cluster', 'success', 'failure', 'alr')},
            'limitations': ['Probe and ALR events only; compressed BWE batches not decoded',
                            'Relative times are per peer; not phase or external latency measurements']}


def read(path):
    with Path(path).open('rb') as stream:
        return parse(stream.read(LIMIT + 1))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    print(json.dumps(read(args.log), indent=2))
