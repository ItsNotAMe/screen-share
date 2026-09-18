"""Silent hardware-encoded window load with selectable decoding on an authorized Windows SSH peer."""
import argparse
import base64
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import time
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('build', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--peer', required=True, help='Existing SSH user@host')
    p.add_argument('--identity', type=Path, required=True)
    p.add_argument('--known-hosts', type=Path, required=True)
    p.add_argument('--origin', required=True)
    p.add_argument('--seconds', type=int, default=60)
    p.add_argument('--decoder', choices=('hardware','software'), default='hardware', help='Expected viewer decoder; host encoding stays hardware')
    p.add_argument('--consumer', choices=('pixels','presentation'), default='pixels', help='Use production frame handoff and D3D presentation on an owned viewer window')
    p.add_argument('--interactive-viewer', action='store_true', help='Run the viewer in the SSH account\'s already signed-in desktop using a temporary, non-elevated scheduled task')
    p.add_argument('--runtime-name', help='Reuse this single folder name below the test account\'s ScreenShareTests folder to avoid a new firewall application path on every run; evidence stays in fresh directories')
    args = p.parse_args()
    if not 10 <= args.seconds <= 300 or not args.origin.startswith('https://') or args.peer.startswith('-'):
        p.error('Use HTTPS, an SSH destination and 10..300 seconds')
    if args.runtime_name and not re.fullmatch(r'[A-Za-z0-9_-]{1,100}', args.runtime_name):
        p.error('Runtime name must be a single alphanumeric, dash or underscore folder name')
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    build = args.build.resolve(); executable = build / 'CrossMachineRoomProof.exe'
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    ssh_options = ['-i', str(args.identity.resolve()), '-o', 'UserKnownHostsFile=' + str(args.known_hosts.resolve()),
                   '-o', 'StrictHostKeyChecking=yes', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8']
    report = dict(schema=1, passed=False, physicalInput=False, audibleOutput=False, externalLatencyVerified=False,
                  requestedSeconds=args.seconds, decoderMode=args.decoder, consumerMode=args.consumer,
                  interactiveViewer=args.interactive_viewer, executableSha256=sha(executable),
                  sourceHashes={s: sha(ROOT / s) for s in (
                      'tools/webrtc-proof/CrossMachineRoomProof.cpp', 'tools/webrtc-proof/CrossMachineLoadProof.h',
                      'tools/backend-comparison/ComparisonScene.h', 'backend/media/webrtc/MediaNetworkPolicy.h',
                      'backend/media/webrtc/WindowsRoomRuntime.h', 'backend/media/webrtc/WindowsRoomRuntime.cpp',
                      'backend/media/webrtc/MfVideoDecoderFactory.cpp',
                      'frontend/shared/LatestRoomVideoFrame.h', 'backend/render/FramePresentationBackend.cpp',
                      'backend/render/FramePresentationBackend.h', 'backend/render/PresentationTarget.h',
                      'backend/render/Nv12D3D11Presenter.cpp',
                      'scripts/test-room-hardware-load.py', 'scripts/test-room-live-service.ps1', 'scripts/RoomLiveEvidence.ps1')})
    host = None

    def run(command, timeout=60):
        completed = subprocess.run(command, capture_output=True, timeout=timeout, creationflags=flags)
        if completed.returncode:
            # PowerShell can write only module-loading CLIXML to stderr while
            # the useful native failure/report is on stdout. Preserve both.
            stdout = completed.stdout.decode('utf-8-sig', errors='replace')[-4000:]
            stderr = completed.stderr.decode('utf-8', errors='replace')[-4000:]
            raise RuntimeError(f'Command exited {completed.returncode}\nstdout: {stdout}\nstderr: {stderr}')
        return completed.stdout.decode('utf-8-sig', errors='replace').strip()

    def remote(script, timeout=60):
        encoded = base64.b64encode(("$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; " + script).encode('utf-16le')).decode('ascii')
        return run(['ssh', *ssh_options, args.peer, 'powershell.exe -NoProfile -NonInteractive -ExecutionPolicy RemoteSigned -EncodedCommand ' + encoded], timeout)

    try:
        name = 'hardware-load-' + uuid.uuid4().hex
        remote_root = remote("$p=Join-Path $env:USERPROFILE " + quote('ScreenShareTests/' + name) +
                             "; New-Item -ItemType Directory -Path $p | Out-Null; $p")
        report['remoteDirectory'] = remote_root
        package = output / 'proof.zip'
        files = [executable, *build.glob('*.dll'), *build.glob('tls/*.dll')]
        with zipfile.ZipFile(package, 'w', zipfile.ZIP_DEFLATED) as archive:
            for path in files:
                archive.write(path, path.relative_to(build).as_posix())
            for name in ('test-room-live-service.ps1', 'RoomLiveEvidence.ps1'):
                archive.write(ROOT / 'scripts' / name, name)
        report['packageSha256'] = sha(package)
        remote_zip = remote_root.replace('\\', '/') + '/proof.zip'
        run(['scp', *ssh_options, str(package), args.peer + ':' + remote_zip])
        remote("if ((Get-FileHash -LiteralPath " + quote(remote_zip) + ").Hash.ToLowerInvariant() -ne " + quote(sha(package)) +
               ") { throw 'Package hash mismatch' }; Expand-Archive -LiteralPath " + quote(remote_zip) + ' -DestinationPath ' + quote(remote_root))
        runtime_root = remote_root
        if args.runtime_name:
            runtime_root = remote("$p=Join-Path $env:USERPROFILE " + quote('ScreenShareTests/' + args.runtime_name) +
                                  "; New-Item -ItemType Directory -Force -Path $p | Out-Null; $p")
            report['remoteRuntimeDirectory'] = runtime_root
            runtime_exe = runtime_root + '/CrossMachineRoomProof.exe'
            remote("$target=[IO.Path]::GetFullPath(" + quote(runtime_exe) + "); "
                   "foreach($p in [Diagnostics.Process]::GetProcessesByName('CrossMachineRoomProof')) { "
                   "try { if ($p.MainModule.FileName -eq $target) { throw 'Selected runtime is already in use' } } finally { $p.Dispose() } }")
            copies = []
            for path in files:
                relative = path.relative_to(build).as_posix()
                destination = runtime_root + '/' + relative
                copies.append("New-Item -ItemType Directory -Force -Path (Split-Path -Parent " + quote(destination) + ") | Out-Null; "
                              "Copy-Item -LiteralPath " + quote(remote_root + '/' + relative) + ' -Destination ' + quote(destination) + '; '
                              'if ((Get-FileHash -LiteralPath ' + quote(destination) + ').Hash.ToLowerInvariant() -ne ' + quote(sha(path)) +
                              ") { throw 'Runtime file hash mismatch' }")
            # EncodedCommand expands UTF-16 text; keep below the Windows SSH
            # command-shell length limit even with long user-profile paths.
            for index in range(0, len(copies), 2):
                remote('; '.join(copies[index:index + 2]))
        report['remoteRuntimeDirectory'] = runtime_root
        ready = output / 'ready.json'
        env = os.environ.copy()
        # Do not inherit PowerShell 7 module paths into Windows PowerShell 5.1.
        powershell = str(Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe')
        env['PSModulePath'] = str(Path(powershell).parent / 'Modules')
        with (output / 'host-runner.log').open('wb') as log:
            host = subprocess.Popen([powershell, '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'RemoteSigned', '-File', str(ROOT / 'scripts/test-room-live-service.ps1'),
                '-Executable', str(executable), '-Origin', args.origin, '-Scenario', 'load-host', '-Seconds', str(args.seconds),
                '-ReadyFile', str(ready), '-Decoder', args.decoder, '-Consumer', args.consumer, '-OutputDirectory', str(output / 'host')], stdout=log, stderr=subprocess.STDOUT,
                env=env, creationflags=flags)
            deadline = time.monotonic() + 60
            while not ready.exists():
                if host.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('Host did not publish readiness; inspect host-runner.log')
                time.sleep(.1)
            room = json.loads(ready.read_text())['roomId']
            remote_script = ('& ' + quote(remote_root + '/test-room-live-service.ps1') + ' -Executable ' +
                quote(runtime_root + '/CrossMachineRoomProof.exe') + ' -Origin ' + quote(args.origin) +
                ' -Scenario load-viewer -Decoder ' + args.decoder + ' -Consumer ' + args.consumer + ' -Seconds ' + str(args.seconds) + ' -RoomId ' + quote(room) +
                ' -OutputDirectory ' + quote(remote_root + '/viewer'))
            # Copy evidence even if the endpoint fails; never discard a rejected run.
            try:
                if args.interactive_viewer:
                    # InteractiveToken uses an existing login, never a saved
                    # password, elevated token, auto-login or desktop unlock.
                    action = base64.b64encode(remote_script.encode('utf-16le')).decode('ascii')
                    task_name = 'ScreenShareProof-' + uuid.uuid4().hex
                    report['temporaryTask'] = task_name
                    task_script = (
                        "$svc=New-Object -ComObject Schedule.Service; $svc.Connect(); $folder=$svc.GetFolder('\\'); "
                        "$task=$svc.NewTask(0); $task.Principal.UserId=[Security.Principal.WindowsIdentity]::GetCurrent().Name; "
                        "$task.Principal.LogonType=3; $task.Principal.RunLevel=0; $task.Settings.Hidden=$true; "
                        "$task.Settings.DisallowStartIfOnBatteries=$false; $task.Settings.StopIfGoingOnBatteries=$false; "
                        "$task.Settings.ExecutionTimeLimit='PT" + str(args.seconds + 110) + "S'; "
                        "$action=$task.Actions.Create(0); $action.Path=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'; "
                        "$action.Arguments='-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy RemoteSigned -EncodedCommand " + action + "'; "
                        "$registered=$folder.RegisterTaskDefinition(" + quote(task_name) + ", $task, 2, $task.Principal.UserId, $null, 3); "
                        "try { $running=$registered.Run($null); $deadline=[DateTime]::UtcNow.AddSeconds(" + str(args.seconds + 100) + "); "
                        "while ($registered.State -eq 4 -or -not (Test-Path -LiteralPath " + quote(remote_root + '/viewer/result.json') + ")) { "
                        "if ([DateTime]::UtcNow -gt $deadline) { throw 'Interactive viewer deadline; ensure test account is signed in' }; Start-Sleep -Milliseconds 500 }; "
                        "Get-Content -LiteralPath " + quote(remote_root + '/viewer/result.json') + " -Raw "
                        "} finally { try { $registered.Stop(0) } finally { $folder.DeleteTask(" + quote(task_name) + ",0) } }")
                    text = remote(task_script, args.seconds + 120)
                else:
                    text = remote(remote_script, args.seconds + 120)
                (output / 'viewer-runner.log').write_text(text, encoding='utf-8')
            except Exception as error:
                report['remoteError'] = str(error)
            finally:
                try:
                    run(['scp', *ssh_options, '-r', args.peer + ':' + remote_root.replace('\\', '/') + '/viewer', str(output / 'viewer')])
                except Exception as error:
                    report['evidenceCopyError'] = str(error)
            if 'remoteError' in report or 'evidenceCopyError' in report:
                raise RuntimeError(report.get('remoteError', report.get('evidenceCopyError')))
            if host.wait(timeout=45) != 0:
                raise RuntimeError('Host rejected the hardware run; inspect host evidence')
        reports = {role: json.loads((output / role / 'result.json').read_text(encoding='utf-8-sig')) for role in ('host', 'viewer')}
        for role, value in reports.items():
            if value.get('passed') is not True or value.get('timedOut') is not False or value.get('exitCode') != 0 or \
                    value.get('decoderMode') != args.decoder or value.get('consumerMode') != args.consumer or \
                    value.get('executableSha256') != report['executableSha256'] or \
                    value.get('validatorSha256') != sha(ROOT / 'scripts/RoomLiveEvidence.ps1') or \
                    value.get('runnerSha256') != sha(ROOT / 'scripts/test-room-live-service.ps1'):
                raise RuntimeError('Failed or mismatched ' + role + ' evidence')
        if not reports['host']['machine'] or reports['host']['machine'] == reports['viewer']['machine']:
            raise RuntimeError('Endpoint machines must differ')
        report['endpoints'] = reports
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    finally:
        if host and host.poll() is None:
            # The endpoint runner owns a bounded native-process deadline; allow it
            # to clean up its room rather than orphaning its child by killing it.
            try:
                host.wait(timeout=args.seconds + 115)
            except subprocess.TimeoutExpired:
                subprocess.run(['taskkill', '/PID', str(host.pid), '/T', '/F'], capture_output=True, creationflags=flags)
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'passed': report['passed'], 'error': report.get('error'), 'evidence': str(output)}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
