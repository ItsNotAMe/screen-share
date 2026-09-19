import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import rtc_probe_evidence as rtc


def integer(value):
    output = bytearray()
    while value > 127:
        output.append((value & 127) | 128)
        value >>= 7
    output.append(value)
    return bytes(output)


def event(tag, values):
    payload = b''.join(integer(key * 8) + integer(value) for key, value in values)
    return integer(tag * 8 + 2) + integer(len(payload)) + payload


BEGIN = event(16, [(1, 1000), (2, 2), (3, 900000)])
END = event(17, [(1, 2000)])


class RtcProbeEvidenceTests(unittest.TestCase):
    def test_events_preserve_repeated_cluster_results_and_history(self):
        cluster = event(21, [(1, 900), (2, 7), (3, 12000000), (4, 5), (5, 22500)])
        success = event(22, [(1, 1100), (2, 7), (3, 11900000)])
        failure = event(23, [(1, 1200), (2, 7), (3, 2)])
        result = rtc.parse(BEGIN + cluster + success + success + failure + END)
        self.assertEqual(result['counts'], {'cluster': 1, 'success': 2, 'failure': 1, 'alr': 0})
        self.assertEqual(result['events'][0]['relativeMs'], -100)
        self.assertNotIn('utcTimeMs', str(result))

    def test_reject_incomplete_or_malformed_logs(self):
        invalid = [b'', BEGIN, END, BEGIN + END[:-1], BEGIN + BEGIN + END,
                   BEGIN + END + END, event(16, [(1, 1), (2, 3), (3, 1)]) + END,
                   BEGIN + event(22, [(1, 1100), (2, 1)]) + END,
                   BEGIN + event(24, [(1, 1100), (2, 2)]) + END,
                   BEGIN + event(23, [(1, 1100), (2, 1), (3, 4)]) + END,
                   BEGIN + event(24, [(1, 2100), (2, 1)]) + END,
                   BEGIN + event(24, [(1, 1100), (1, 1100), (2, 1)]) + END,
                   b'\x00', b'\x80' * 10, b'\x08' + b'\xff' * 9 + b'\x02',
                   b'\x0a\x00', b'\x0b', b'x' * (rtc.LIMIT + 1)]
        for value in invalid:
            with self.subTest(value=value[:20]), self.assertRaises(ValueError):
                rtc.parse(value)

    def test_skips_private_transport_events(self):
        result = rtc.parse(BEGIN + event(25, [(1, 123456)]) + END)
        self.assertEqual(result['events'], [])
        self.assertNotIn('123456', str(result))


if __name__ == '__main__':
    unittest.main()
