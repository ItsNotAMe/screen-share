"""Trace surviving WGC/RPC handles in a test-owned process, never a user app.

Requires the Debug production-owner proof, its PDB and the x64 Windows SDK CDB.
This deliberately stops the proof at the final snapshot; it is not acceptance.
"""
import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("room_regression", ROOT / "scripts/test-room-regression.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def commands(symbols, start, end):
    # These values become debugger commands, not shell arguments.
    if any(char in str(symbols) for char in '\r\n";') or not 1 <= start < end <= 999:
        raise ValueError("Unsafe symbol path or invalid snapshot interval")
    lines = [f'.sympath srv*{symbols}*https://msdl.microsoft.com/download/symbols',
             'r @$t0=0',
             'bu0 WindowsCaptureLifecycleTest!screenshare::DesktopCapturer::InitializeWinRt "r @$t0=@$tid; gc"',
             f'bu1 WindowsCaptureLifecycleTest!proof::LifecycleSample ".if (@ecx == 0n{start}) '
             '{ be 2; be 3; !htrace -enable 0x20000; !htrace -snapshot; .echo CAPTURE_TRACE_START; gc } '
             f'.else {{ .if (@ecx == 0n{end}) '
             '{ !htrace -diff; !handle 0 f; .echo CAPTURE_TRACE_COMPLETE; .kill; q } .else { gc } }"']
    # Only one capture owner runs at a time in this proof. Keep the return
    # breakpoint armed if another thread hits the same RPC return address.
    returned = '.if (@$tid == @$t0) { .printf "PORT_RETURN %p status %p thread %x\\n", poi(@$t2), @rax, @$tid; bc 4; gc } .else { gc }'
    escaped_return = returned.replace('\\', '\\\\').replace('"', '\\"')
    for index, name in enumerate(('NtAlpcConnectPort', 'NtAlpcConnectPortEx'), 2):
        action = f'.if (@$tid == @$t0) {{ r @$t2=@rcx; k 0n30; bp4 @$ra "{escaped_return}"; gc }} .else {{ gc }}'
        escaped_action = action.replace('\\', '\\\\').replace('"', '\\"')
        lines.append(f'bu{index} ntdll!{name} "{escaped_action}"')
    # A condition that just resumes still traps every allocation. Disable the
    # breakpoints entirely until warm-up ends to preserve the native deadlines.
    lines += ['bd 2', 'bd 3', 'g']
    return '\n'.join(lines) + '\n'


def summarize(log):
    start = re.search(r'^CAPTURE_TRACE_START\r?$', log, re.M)
    end = re.search(r'^CAPTURE_TRACE_COMPLETE\r?$', log, re.M)
    complete = bool(start and end and end.start() > start.end())
    interval = log[start.end():end.start()] if complete else log
    types = {}
    for match in re.finditer(r'^Handle ([0-9a-f]+)\r?\n\s+Type\s+([^\r\n]+)', interval, re.M | re.I):
        types[int(match[1], 16)] = match[2].strip()
    returns = {}
    for block in interval.split('Child-SP')[1:]:
        match = re.search(r'^PORT_RETURN ([0-9a-f]+) status ([0-9a-f]+) thread ([0-9a-f]+)\r?$', block, re.M | re.I)
        if match and int(match[2], 16) == 0:
            stack = [line.strip() for line in block[:match.start()].splitlines() if '!' in line]
            returns[(int(match[1], 16), int(match[3], 16))] = stack
    retained = []
    for match in re.finditer(r'Handle = 0x([0-9a-f]+) - OPEN\r?\nThread ID = 0x([0-9a-f]+)', interval, re.I):
        handle, thread = int(match[1], 16), int(match[2], 16)
        stack = returns.get((handle, thread), [])
        retained.append({'handle': hex(handle), 'thread': hex(thread), 'type': types.get(handle),
                         'correlatedPortStack': stack})
    summary = re.search(r'Displayed 0x([0-9a-f]+) stack traces for outstanding handles', interval, re.I)
    complete = complete and bool(summary) and int(summary[1], 16) == len(retained)
    complete = complete and all(item['type'] for item in retained)
    return {'traceComplete': bool(complete), 'acceptanceRun': False, 'retainedHandles': retained,
            'correlatedPorts': sum(item['type'] == 'ALPC Port' and bool(item['correlatedPortStack']) for item in retained)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('output', type=Path, help='New evidence directory')
    parser.add_argument('--cdb', type=Path, default=Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) /
                        'Windows Kits/10/Debuggers/x64/cdb.exe')
    parser.add_argument('--start-cycle', type=int, default=30)
    parser.add_argument('--end-cycle', type=int, default=35)
    parser.add_argument('--timeout', type=float, default=240)
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('Timeout must be finite and positive')
    executable = args.executable.resolve(strict=True)
    pdb = executable.with_suffix('.pdb')
    if executable.name.lower() != 'windowscapturelifecycletest.exe' or not pdb.is_file():
        parser.error('Requires WindowsCaptureLifecycleTest.exe and its Debug PDB')
    cdb = args.cdb.resolve(strict=True)
    symbols = ROOT / 'build/symbols'
    try:
        script = commands(symbols, args.start_cycle, args.end_cycle)
    except ValueError as error:
        parser.error(str(error))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    symbols.mkdir(parents=True, exist_ok=True)
    command_file = output / 'commands.txt'
    command_file.write_text(script, encoding='utf-8')
    command = [str(cdb), '-G', '-cf', str(command_file), str(executable), '--cycles', str(args.end_cycle + 1)]
    execution = runner.run_process(command, output / 'native.log', os.environ.copy(),
                                   timeout=args.timeout, max_log_bytes=16 * 1024 * 1024)
    result = summarize((output / 'native.log').read_text(encoding='utf-8', errors='replace'))
    result.update(execution=execution, command=command, executableSha256=runner.sha256(executable),
                  pdbSha256=runner.sha256(pdb), commandSha256=runner.sha256(command_file),
                  startCycle=args.start_cycle, endCycle=args.end_cycle)
    result['traceComplete'] &= execution['passed']
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: result[key] for key in ('traceComplete', 'acceptanceRun', 'correlatedPorts')}))
    return 0 if result['traceComplete'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
