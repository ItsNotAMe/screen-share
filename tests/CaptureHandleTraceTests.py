import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('trace', Path(__file__).parents[1] / 'scripts/trace-capture-handles.py')
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)

LOG = '''CAPTURE_TRACE_START
Child-SP          RetAddr               Call Site
00000001 00000002 ntdll!NtAlpcConnectPort
00000001 00000002 GraphicsCapture!TryCreateForWindow
PORT_RETURN 0000000000000a44 status 0000000000000000 thread 8e04
Outstanding handles opened since the previous snapshot:
Handle = 0x0000000000000a44 - OPEN
Thread ID = 0x0000000000008e04, Process ID = 0x1234
Displayed 0x1 stack traces for outstanding handles opened since the previous snapshot.
Handle a44
  Type         ALPC Port
  Name         <none>
CAPTURE_TRACE_COMPLETE
'''


class HandleTraceTests(unittest.TestCase):
    def test_correlates_type_handle_and_allocating_thread(self):
        result = trace.summarize(LOG)
        self.assertTrue(result['traceComplete'])
        self.assertFalse(result['acceptanceRun'])
        self.assertEqual(result['correlatedPorts'], 1)
        self.assertEqual(result['retainedHandles'][0]['type'], 'ALPC Port')

    def test_reused_handle_on_another_thread_or_failed_connect_does_not_match(self):
        for log in (LOG.replace('thread 8e04', 'thread 9999'),
                    LOG.replace('status 0000000000000000', 'status 00000000c0000001'),
                    LOG.replace('Type         ALPC Port', 'Type         Event')):
            self.assertEqual(trace.summarize(log)['correlatedPorts'], 0)

    def test_missing_markers_type_or_trace_count_fails(self):
        for text in ('CAPTURE_TRACE_START', 'CAPTURE_TRACE_COMPLETE', '  Type         ALPC Port',
                     'Displayed 0x1 stack traces for outstanding handles opened since the previous snapshot.'):
            self.assertFalse(trace.summarize(LOG.replace(text, ''))['traceComplete'])
        self.assertFalse(trace.summarize(LOG.replace('Displayed 0x1', 'Displayed 0x2'))['traceComplete'])

    def test_echoed_debugger_command_is_not_a_completion_marker(self):
        self.assertFalse(trace.summarize(LOG.replace('CAPTURE_TRACE_COMPLETE',
            '0:000> .echo CAPTURE_TRACE_COMPLETE'))['traceComplete'])

    def test_unrelated_snapshot_outside_interval_is_excluded(self):
        self.assertEqual(len(trace.summarize('Handle = 0xbeef - OPEN\nThread ID = 0xabcd\n' + LOG)
                             ['retainedHandles']), 1)

    def test_commands_disable_traps_until_explicit_decimal_interval(self):
        script = trace.commands(Path('C:/symbols'), 30, 35)
        self.assertIn('@ecx == 0n30', script)
        self.assertIn('@ecx == 0n35', script)
        self.assertTrue(script.endswith('bd 2\nbd 3\ng\n'))
        self.assertIn('bc 4', script)
        for start, end in ((0, 35), (35, 35), (35, 30), (30, 1000)):
            with self.assertRaises(ValueError):
                trace.commands(Path('C:/symbols'), start, end)
        with self.assertRaises(ValueError):
            trace.commands(Path('C:/symbols;g'), 30, 35)


if __name__ == '__main__':
    unittest.main()
